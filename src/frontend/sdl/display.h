// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "frontend/sdl/gpu_present.h"
#include "core/gpu/gpu.h"
#include "core/types.h"
#include "display_disp.h"
#include "display_drm.h"  // complete types for the unique_ptr
#include "display_fbdev.h"
#include "display_wl.h"

#include <SDL2/SDL.h>

#include <memory>
#include <string>
#include <vector>

namespace ds::sdl {

// One window showing some of the DS screens, drawn as a list of views
// (which screen goes in which rectangle). Textures belong to a renderer and
// can't be shared between windows, so they live here rather than in the app.
class Display {
public:
  static constexpr int SCREENS = 2;

  // How the two screens share the window; `primary` is the screen shown
  // alone (Single), large (Pip) or dominant (DominantV/H), else first in the
  // stack/row. DominantV/H: primary fitted to width/height, secondary
  // `dominant` times its size. With `dominant_auto`, the primary instead
  // takes the largest whole scale leaving the secondary >= `dominant_min`.
  enum class Mode : u8 { Vertical, Horizontal, Single, Pip, DominantV, DominantH, Count };
  enum class Corner : u8 { TopLeft, TopRight, BottomLeft, BottomRight, Count };
  struct Layout {
    Mode   mode = Mode::Vertical;
    int    primary = 0;
    Corner corner = Corner::BottomRight;
    double pip = 1.0 / 3.0;      // inset size relative to the large screen
    double dominant = 0.5;       // secondary size relative to the dominant screen
    bool   dominant_auto = true; // pick the primary's whole scale instead (see above); a ratio when false
    double dominant_min = 0.25;  // auto: the smallest secondary the primary may leave
    double pip_alpha = 1.0;      // inset opacity at rest, 0..1 (see set_inset_alpha)
    int    gap = 0;              // video.screen_gap: pixels between the two screens of a pair (negative overlaps)
  };
  // Forced integer scaling of full-size views (PiP inset / dominant
  // secondary keep their own ratio). Under: largest whole scale that fits,
  // letterboxed. Over: smallest that covers, cropped and centred (a stacked
  // pair keeps its shared edge).
  enum class IntScale : u8 { Off, Under, Over };
  static const char* int_scale_name(IntScale m);
  static bool parse_int_scale(const std::string& s, IntScale& m);
  static double snap_scale(double s, IntScale m);
  void set_integer_scale(IntScale m) { int_scale_ = m; }
  IntScale integer_scale() const { return int_scale_; }
  static const char* mode_name(Mode m);      // "vertical" ... "dominant_h"
  static bool parse_mode(const std::string& s, Mode& m);
  static const char* corner_name(Corner c);  // "tl" "tr" "bl" "br"
  static bool parse_corner(const std::string& s, Corner& c);
  // The window size that shows the layout at `scale` window pixels per DS pixel.
  static void natural_size(const Layout& l, double scale, int& w, int& h);

  bool open(const char* title, int scale, bool fullscreen, bool linear, bool vsync, const Layout& layout, int only_screen = -1, int display_index = 0);
  void close();

  void draw(const u32* const fb[SCREENS]);

  // Per-scanline scaling: the core scales each line into place as it's
  // produced, straight into the window surface or a scanout tier's buffer
  // (dropping SDL_Renderer, since the destination must be what's actually
  // presented). draw() is unavailable while this is on. Destination tiers
  // tried in order at open(): (1) a scanout tier (scanout.h) -- DmabufOut
  // under Wayland, DrmOut under KMSDRM; DS_DMABUF=0/1 disables/requires it;
  // (2) the window surface (SDL shm under a compositor; on KMSDRM this is
  // secretly a hidden GLES renderer, see display_drm.h).
  // `grid`: full grid needs >=2x (or a >=4px chunky cell) to fit a lit pixel
  // beside a seam, so small views (PiP inset, dominant secondary) go plain;
  // a dimmed grid applies from 1x instead. See grid_on().
  // `xrun_plain`: the nearest map before chunky rewrote it into pairs/cells.
  // `y_lo`/`y_hi`: rect rows inside the buffer; only cropped under integer overscale.
  struct Target { u32* px; u32 pitch; u32 h; const u16* xrun; const u8* seam_w; const u16* lin_sx; const u8* lin_wx; bool grid; const u16* xrun_plain; u32 y_lo = 0, y_hi = 0; };

  bool scaling() const { return scaled_; }
  // Panel rotation on the scanline tiers (0/90/180/270, same DS_ROTATE the
  // display-engine tier reads). Drawing works in the logical (unrotated)
  // frame into a staging buffer; present() rotates that into the presented
  // buffer as one pass. Set before open() (open() also reads DS_ROTATE). No
  // effect on the SDL_Renderer fallback.
  void set_rotation(int rot) { rot_wanted_ = rot; }
  int rotation() const { return rot_; }
  // Chunky: each 2x2 block of DS pixels drawn as one cell from its top-left
  // pixel. Set before open(). cell: 0 = 2x2 pair path; N = panel pixels per
  // cell when N divides both screen dims (else pair path); -1 = auto (smallest N>=4 that does).
  void set_chunky(bool on, int cell = 0) { chunky_ = on; chunky_cell_ = cell; }
  bool chunky() const { return chunky_; }
  // Present through the display engine's scaler layer (display_disp.h) when
  // available; DS_ROTATE gives the panel rotation. Set before open().
  void set_disp(bool on) { disp_wanted_ = on; }
  // LCD grid on the display-engine tier, as its overlay layer (DispOut::set_grid). Set before open().
  void set_disp_grid(u8 alpha) { disp_grid_ = alpha; }
  bool disp() const { return disp_ != nullptr; }
  // Display-engine tier: suspend the scaler's chunky divisor while a text
  // page (pause menu, loader notice) is on the panel, since it would merge
  // glyphs the way it merges DS pixels. No-op on the other tiers.
  void set_page(bool on);
  // On the display-engine tier, chunky is applied by the scanline scaler at
  // 1:1 then rotated; grid/seams/bilinear need panel pixels and are off there.
  bool effects_at_source() const { return disp_ != nullptr && scaled_; }
  // Chunky is left plain on a view shown smaller than the screen (PiP inset, dominant secondary).
  bool chunky_on(int screen) const { return src_chunky_[screen]; }
  void set_grid_strength(double s) { grid_strength_ = s; }   // 0..1; dimmed grid is an overlay, needs no seam-adjacent room
  // Whether the LCD grid applies to `screen`'s view: panel cells, or >=2x
  // for the full grid and >=1x for a dimmed one (see Target::grid).
  bool grid_on(int screen) const {
    if (cells_[screen].x.cells) return true;
    const int need = grid_strength_ >= 1.0 ? 2 : 1;
    for (int i = 0; i < nviews_; ++i) if (views_[i].screen == screen) return views_[i].rect.w >= need * static_cast<int>(SCREEN_W);
    return true;
  }
  // GPU present stage (gpu_present.h): on a dma-buf scanout tier, a compute
  // dispatch lays 256x192 frames into the tier's buffer via Vulkan, replacing
  // the scanline scaler where the import works. Set before open().
  void set_gpu_present(bool on) { gpu_wanted_ = on; }
  bool gpu_present() const { return gpu_ != nullptr; }
  // The frame's hi-res 3D layer for the GPU composite (Gpu::frame_hires): set before draw().
  void set_gpu_layer(u64 handle, size_t bytes, u32 scale, int screen, u64 edge = 0) { gpu_layer_ = handle; gpu_layer_bytes_ = bytes; gpu_layer_scale_ = scale; gpu_layer_screen_ = screen; gpu_layer_edge_ = edge; }
  void set_smooth3d(bool on) { smooth3d_ = on; }   // video.smooth3d: rebuild polygon edges at panel resolution
  // Present straight through /dev/fb0 (display_fbdev.h). Set before open().
  void set_fbdev(bool on) { fbdev_wanted_ = on; }
  const void* cell_map(int screen) const { return cells_[screen].x.cells ? &cells_[screen] : nullptr; }
  // Locks the panel-sized texture and fills in one target per screen. False
  // if the lock failed, in which case the caller must fall back to draw().
  bool begin_frame(Target out[SCREENS]);
  void end_frame();   // unlock and present
  void on_resize() { layout(); build_scale(); margins_dirty_ = true; }
  void toggle_fullscreen();
  bool fullscreen() const { return fullscreen_; }
  // Resizes a windowed window to the new mode's natural size at the current
  // scale. Ignored on a single-screen (dual-window) display.
  void set_layout(const Layout& l);
  const Layout& current_layout() const { return layout_; }
  double dominant_ratio() const;   // secondary/primary size as placed; 1 outside the dominant layouts
  // Inset opacity for the coming frame, 0..255 (frontend ramps it between
  // Layout::pip_alpha and opaque while the bottom screen is touched).
  void set_inset_alpha(u8 a) { inset_alpha_ = a; }
  u8   inset_alpha() const { return inset_alpha_; }
  // dst[i] = dst[i] + (src[i] - dst[i]) * alpha / 255 per channel, alpha
  // forced opaque. Shared with the screenshot writer.
  static void blend_row(u32* dst, const u32* src, size_t n, u32 alpha);
  bool across() const { return layout_.mode == Mode::Horizontal || layout_.mode == Mode::DominantH; }

  // Window point -> pixel in `screen`. False if the point is not on a screen.
  bool map_point(int wx, int wy, int& screen, int& sx, int& sy) const;

  // Renderer output size, what map_point's coordinates are in. NOT uniformly
  // panel resolution: on the display-engine tier it's the DS-space canvas;
  // under scanline-tier rotation it's the presented (panel) size. For
  // drawing, ask canvas() instead.
  void output_size(int& w, int& h) const;

  // The whole output buffer for the frontend's own drawing (pause menu,
  // overlays), in panel pixels. Valid between begin_frame() and present();
  // draw after finish_views() so an inset doesn't land on top. On the
  // display-engine tier it's instead the overlay layer's own transparent
  // surface, valid whether or not that tier is scaling.
  struct CanvasView { u32* px; u32 pitch; int w, h; };
  bool canvas(CanvasView& out) const;
  // True on the scanline tiers and on the display-engine tier with its
  // overlay layer; false on SDL_Renderer, which has no buffer of its own.
  bool canvas_capable() const;

  // end_frame() in two halves: finish_views() copies insets into place,
  // present() puts it on screen. finish_views() is idempotent.
  void finish_views();
  void present();
  // After a present with no frame following (the pause menu): make sure the
  // scanout tier has put it on its way. See ScanoutOut::flush.
  void flush() { if (gpu_ && out_) gpu_->flush(*out_); if (out_) out_->flush(); }

  // Where the frontend drew on the canvas this frame, so a scanout tier's
  // other buffers get it cleaned up too before reuse (an overlay isn't
  // repainted by the emulator, so it would otherwise persist as fragments).
  // Rects accumulate into one for the frame; passing nothing is harmless.
  void note_canvas_draw(int x, int y, int w, int h);
  void note_canvas_draw_all() { note_canvas_draw(0, 0, frame_w_, frame_h_); }
  SDL_Window* window() const { return win_; }
  u32 window_id() const { return win_ ? SDL_GetWindowID(win_) : 0; }

  // `direct`: the core scales straight into the window at `rect`. Otherwise
  // into side_[screen] (an inset or hidden screen needs its own target),
  // which end_frame() copies into place if `shown`.
  struct View { int screen; SDL_Rect rect; bool direct; bool shown; };
  // Where the two screens go in a w x h output under `l`, draw order (later
  // on top). Shared with the screenshot writer.
  static void place(const Layout& l, int w, int h, View out[SCREENS], IntScale snap = IntScale::Off);
  static void dominant_auto(const Layout& l, int w, int h, bool across, IntScale snap, double& s, double& s2);

private:
  void layout();
  void build_source_scale();   // display-engine tier w/ chunky: 1:1 run tables and source-side cell maps
  void build_scale();          // pick up the window surface and rebuild the x-map
  bool out_size(int& w, int& h) const;   // renderer output, or the surface in scaled mode (logical under rotation)
  bool rotated() const { return rot_ == 90 || rot_ == 270; }   // logical and presented sizes differ
  // Points the frame at the logical staging buffer under rotation, clearing
  // what the last frame left; the presented buffer is kept for present().
  void take_frame(u32* px, u32 stride, int w, int h, int idx, Target out[SCREENS]);
  void rotate_out(u32* dst, u32 dst_pitch) const;
  void clear_margins(u32* px, u32 pitch, int w, int h) const;
  void targets(u32* px, u32 stride, int w, int h, Target out[SCREENS]);
  void blit_insets();

  SDL_Window*   win_ = nullptr;
  SDL_Renderer* ren_ = nullptr;
  SDL_Texture*  tex_[SCREENS] = {nullptr, nullptr};
  View          views_[SCREENS] = {};
  int           nviews_ = SCREENS;
  int           only_screen_ = -1;
  IntScale      int_scale_ = IntScale::Off;
  int           display_index_ = 0;
  bool          fullscreen_ = false;
  Layout        layout_;
  u8            inset_alpha_ = 255;

  bool              scaled_ = false;
  int               rot_wanted_ = 0;      // set_rotation(); DS_ROTATE otherwise
  int               rot_ = 0;             // in effect on the scanline tiers
  int               phys_w_ = 0, phys_h_ = 0;   // the presented buffer's size (scaled_w_/h_ are logical)
  std::vector<u32>  stage_;               // the logical frame under rotation
  u32*              phys_px_ = nullptr;   // the presented buffer this frame, under rotation
  u32               phys_pitch_ = 0;
  bool              disp_wanted_ = false;
  u8                disp_grid_ = 0;
  bool              fbdev_wanted_ = false;
  std::unique_ptr<DispOut> disp_;       // display-engine tier; null otherwise
  bool              chunky_ = false;
  double            grid_strength_ = 0.0;
  int               chunky_cell_ = 0;
  int               disp_divisor_ = 1;   // the divisor build_source_scale chose, restored after a page
  bool              page_ = false;
  std::unique_ptr<ScanoutOut> out_;     // tier 1; null on the surface tier
  bool              gpu_wanted_ = false;
  std::unique_ptr<GpuPresent> gpu_;     // the GPU present stage on top of out_; null otherwise
  u64 gpu_layer_ = 0; size_t gpu_layer_bytes_ = 0; u32 gpu_layer_scale_ = 1; int gpu_layer_screen_ = 0; u64 gpu_layer_edge_ = 0;
  bool smooth3d_ = false;
  bool try_gpu_present();               // after out_ opened: import its buffers, switch draw() over
  void draw_gpu(const u32* const fb[SCREENS]);
  SDL_Surface*      surf_ = nullptr;    // window surface; owned by SDL
  bool              margins_dirty_ = true;
  u32               out_clean_ = 0;       // scanout buffers (by index) whose letterbox is cleared
  // What the frontend drew on the canvas: this frame, and the last time each
  // buffer was used. More buffers than any tier keeps (drm 3, dmabuf 4).
  static constexpr int kMaxBufs = 8;
  mutable bool      canvas_taken_ = false;   // canvas() was handed out this frame (the overlay tier)
  SDL_Rect          canvas_drawn_{0, 0, 0, 0};
  SDL_Rect          canvas_prev_[kMaxBufs] = {};
  void clear_rect(u32* px, u32 pitch, int w, int h, const SDL_Rect& r) const;
  bool              out_frame_ = false;   // current begin_frame targeted the scanout tier
  int               scaled_w_ = 0, scaled_h_ = 0;
  std::vector<u16>  xrun_[SCREENS];   // per screen, 257 entries; see kern::scale_row
  std::vector<u16>  xrun_plain_[SCREENS];   // the same before chunky's pair/cell rewrite
  std::vector<u8>   seam_w_[SCREENS]; // per screen, 256 entries: box-filter weight of pixel s+1 in run s's last pixel
  std::vector<u16>  lin_sx_[SCREENS]; // per screen, rect.w entries: bilinear source column per destination column
  std::vector<u8>   lin_wx_[SCREENS]; // per screen, rect.w entries: weight of column lin_sx+1, 0..255
  ds::gpu::Gpu::CellMap cells_[SCREENS];   // chunky cell tables; x.cells == 0 when the pair path is in use
  std::vector<u32>  side_[SCREENS];   // scaled pixels of a non-direct view
  std::vector<u32>  src_side_[SCREENS];  // display-engine tier: the 1:1 scaler output per screen (effects_at_source)
  bool              src_chunky_[SCREENS] = {true, true};   // chunky applies to this screen's view there
  u32*              frame_px_ = nullptr;   // the buffer begin_frame handed out, for end_frame's insets
  int               frame_w_ = 0, frame_h_ = 0;   // ... and its size, for clipping an inset at the edge
  u32               frame_pitch_ = 0;
  bool              insets_done_ = false;   // finish_views() ran for this frame
};

} // namespace ds::sdl
