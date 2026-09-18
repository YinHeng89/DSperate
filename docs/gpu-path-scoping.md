# GPU render/present path — scoping and P0 measurements

Branch `gpu-path` (off `main`, 2026-09-17). The design is in the approved
plan (context, hard-limit table, architecture, phases); this document is the
measured record, in the style of `gpu-raster-scoping.md` on the
`gpu-raster` branch, which is now a parts bin for this work.

Device: RG DS Plus, RK3566, Mali-G52, libmali 1.3 (driver 0x7401000),
ROCKNIX 20260917, **two 1024x768 panels**, governor pinned to performance.

## P0.1 — importing the frontend's dma-heap buffers into Vulkan: PASS

`tools/vk_import_probe.c` (+ `.sh`, two shaders). Allocates from
`/dev/dma_heap/linux,cma` exactly as DmabufOut/DrmOut do, imports the fd.

| | result |
| --- | --- |
| import as storage BUFFER | OK (`vkGetMemoryFdPropertiesKHR` + dedicated allocation) |
| import as LINEAR IMAGE | OK with `VK_EXT_image_drm_format_modifier`, explicit plane layout, **`size = 0`** (a real size is rejected with `VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT`); B8G8R8A8 lists exactly one modifier, LINEAR, storage-capable; driver pitch == our pitch at 640, 1024, 1280 wide |
| GPU write visible to the CPU / display without `DMA_BUF_IOCTL_SYNC` | yes: 0 wrong pixels with and without the ioctl, buffer and image arms |
| CPU mapping of the CMA buffer | cached: read 2.0 GB/s = malloc, write 7.9-9.8 GB/s = malloc |
| GPU fill of one panel, buffer path | 640x480 1.17 ms, 1024x768 2.97 ms, 1280x720 1.80 ms (~1-2 GB/s; device-local memory is only 1.16x faster, so the import is not the cost) |
| GPU fill of one panel, **imageStore path** | 640x480 0.50 ms, **1024x768 1.25 ms**, 1280x720 1.47 ms |

So the present stage writes the imported buffers **as images**; the dual
1024x768 frame costs ~2.5 ms of GPU to write. The CPU scaler writes the same
6 MB per frame today.

## P0.4 — the pre-resolved 2D/3D composite on the GPU: PASS

`tools/vk_composite_probe.c/.comp`: four native-res u32 planes (top, second,
packed id/kind/alpha/sid, window) + a per-line BLDCNT word + the 3D layer at
scale S, `kern::composite_line` transcribed, one invocation per output pixel.

| S | output | throughput | single frame round trip |
| --- | --- | --- | --- |
| 1 | 256x192 | 0.43 ms | 0.85 ms |
| 2 | 512x384 | 1.46 ms | 1.90 ms |
| 3 | 768x576 | 3.20 ms | 3.65 ms |

Against the gate (1.5 ms at S=1) this is comfortable. The "6.5-9.1 ms
naive 2D composite" of the old probe was the palette/BG/OBJ work, which
stays on NEON.

## P0.2 — the compute raster at scale 2 and 3: FAILS THE 6 ms RULE

Measured live rather than by replay: the `gpu-raster` branch built in a
worktree with `DS_VK_SCALE=S` (scale from the environment; host upload in
scaled coordinates; `yrng` widened to 16 bits per bound -- it overflowed 8 at
S>1; the raster pass dispatched with S² sub-blocks per tile so an invocation
still owns one pixel; span table sized by S; frames with shadow masks refused
at S>1 -- Spirit Tracks loses half its frames to that, its S>1 rows are the
other half). The composite still reads 256-wide rows, so the picture is wrong
above 1; only the per-pass GPU times are the result. `DS_VK_TIMING=1`, GPU
ms per frame, fence-stamped:

| scene | S=1 | S=2 | S=3 | S=2 by pass (span / vis / raster / post) |
| --- | --- | --- | --- | --- |
| Etrian Odyssey | 3.0 | 8.3 | 17.0 | 0.8 / 3.0 / 4.4 / 0 |
| Mario & Luigi | 2.7 | 6.7 | 13.0 | 1.0 / 2.1 / 3.5 / 0 |
| Spirit Tracks (unmasked frames) | 5.7 | 11.1 | 20.2 | 0.2 / 3.0 / 6.4 / 1.5 |
| Golden Sun | 6.9 | 18.9 | 39.3 | 1.3 / 4.7 / 11.6 / 1.2 |

The cost scales ~2.5-2.8x per doubling, i.e. with pixels: both the
visibility pass (fragments through the 64-bit atomicMin) and the raster pass
(winner shading once per pixel + the ordered tail walked over every pixel)
grow with S². With composite (~1.5 ms) and present (~2.5 ms) on top, S=2
fits the 16.7 ms GPU frame only for the Etrian/Mario & Luigi class; Golden
Sun does not fit at S=2 on this design. Per the plan's decision rule this
sends the question to P0.3.

## P1a — engine B's scaler onto the LineWorker in batched frames: LOSS

SDL frontend, `--dual-window`, `DS_FRAME_STATS=1`, same binary, the knob
flipped, both orders (A B B A):

| scene | stash on | stash off |
| --- | --- | --- |
| NSMB (1200 frames) | 16.0 / 16.0 | **14.6 / 14.8** |
| Spirit Tracks | 21.2 / 21.0 | **20.2 / 20.4** |
| Golden Sun | 19.5 / 19.5 | 19.3 / 19.7 |

Reverted. Reading: the LineWorker already draws engine A's 192 lines AND
scales them to a 1024x768 panel (3 MB of writes), and the line-0 join makes
it the long pole; handing it engine B's 3 MB as well lengthens the frame.
Which is the case for P1 stated by measurement: on these panels the
panel-size scaling is on the critical path through the worker, and a GPU
present stage removes it from both threads at once.

## P0.3 — the prefix through the graphics pipeline

Built inside the worktree raster as `DS_VK_FRAG=<mode>`: a render pass
(R32_UINT colour + D32 depth, 256S x 192S), a graphics pipeline with no
vertex input, the descriptor set re-bound for vertex/fragment stages, drawn
in band 0 after the span pass and timed in its own stamp pair (`frag-proto`
column). Depth test LESS, depth write on, in submission order -- which is
the DS's opaque ownership rule for mode 0 (ties keep the earlier polygon).

### Span quads (modes 1/2): primitive-bound, LOSES

One quad per span row from the span table (`vkCmdDraw(6, opaque_rows)`),
exact coverage by construction. Mode 1 writes a constant, mode 2 fetches one
texel and alpha-tests. GPU ms per frame, against the visibility pass (V) it
would replace:

| scene | S | V (vis) | F1 depth only | F2 + texel | F1/V |
| --- | --- | --- | --- | --- | --- |
| Etrian Odyssey | 1 | 0.95 | 1.02 | 1.46 | 1.07 |
| | 2 | 2.95 | 2.08 | 3.30 | 0.71 |
| Golden Sun | 1 | 1.69 | 1.65 | 2.41 | 0.98 |
| | 2 | 4.74 | 3.17 | 5.12 | 0.67 |
| Mario & Luigi | 1 | 0.78 | 1.59 | 1.96 | 2.04 |
| | 2 | 2.13 | 3.00 | 4.02 | 1.41 |

Four times the fragments cost only ~2x, and the scene with the shortest
spans (Mario & Luigi, 13 px, 339 prefix polygons) is the worst: the cost is
per PRIMITIVE, not per fragment -- 4-12 K one-or-two-pixel-tall quads a
frame is exactly what a tiler dislikes. F1/V never reaches the plan's 0.6,
and adding the texel fetch (mode 2) puts it above the vis pass everywhere.
**The span-quad fragment path is closed** (at 1x it was already known to
have nothing to win; at hi-res it is primitive-bound).

### Polygons as triangle fans (modes 3-6): the hi-res design

Modes 3/4 draw the order-free prefix as one instance per polygon, a fan of
up to 8 triangles from its clipped vertices (`vkCmdDraw(24, first_ordered)`),
integer vertices placed at pixel centres, depth = vertex z / 2^24 interpolated
by the hardware; 5/6 draw EVERY polygon that way (tail included, no
blending) as a whole-scene fragment-throughput bound. Odd modes write a
constant, even ones fetch a texel and alpha-test. Coverage is the GPU's
triangle rule, not the DS span rule: inexact, which the user allowed for the
GPU path. GPU ms per frame:

| scene | S | prefix depth only (3) | prefix + texel (4) | ALL polys depth (5) | ALL polys + texel (6) | compute design, whole raster |
| --- | --- | --- | --- | --- | --- | --- |
| Etrian Odyssey | 1 | 0.22 | 0.42 | 0.22 | 0.45 | 3.0 |
| | 2 | 0.40 | 0.90 | 0.42 | 0.99 | 8.3 |
| | 3 | -- | 1.72 | -- | 1.91 | 17.0 |
| Golden Sun | 1 | 0.52 | 0.88 | 0.58 | 1.06 | 6.9 |
| | 2 | 0.78 | 1.64 | 0.96 | 2.12 | 18.9 |
| | 3 | -- | 2.96 | -- | 3.98 | 39.3 |
| Mario & Luigi | 1 | 0.44 | 0.60 | 0.45 | 0.63 | 2.7 |
| | 2 | 0.76 | 1.08 | 0.71 | 1.06 | 6.7 |
| | 3 | -- | 1.90 | -- | 1.90 | 13.0 |

**Decision (rule's third arm, and beyond it).** The hardware raster of the
polygons is 6-9x cheaper than the compute design at S=2 and fits S=3 on
every scene with room for the real fragment work (blend modes, fog, edge
marking, the translucent tail) to cost several times the texel probe. So:

- **1x, exact:** the compute raster (the branch's five passes) or the CPU.
- **Hi-res (S >= 2), inexact by user decision:** a graphics-pipeline
  renderer in the melonDS-OpenGL shape, adapted for Mali (below). P3 is
  this renderer, not `GpuFrame::scale`.


What melonDS's OpenGL renderer (the reference for this shape,
`dsperate-research/melonDS/src/GPU3D_OpenGL.cpp`) does that we keep:
one vertex/index upload per frame, fans on the CPU, two monolithic fragment
shaders (Z / W buffer) with the blend mode as a per-polygon attribute, the
texture cache as arrays of one size per array with nearest sampling, poly
ids in the stencil so equal-id translucent polygons do not blend against
each other, the rear-plane bitmap as a clear draw, edge marking and fog as
full-screen passes over an attribute attachment (R = opaque id, G = edge
flag, B = fog flag) plus depth.

What it does that Mali punishes, and the Vulkan shape that avoids it:
- `gl_FragDepth` for W-buffered polygons kills early-Z. Compute the DS depth
  in the VERTEX shader for both modes and keep fixed-function depth. The
  W-buffer value is NOT 1 / vt.w: the DS compares w normalised per polygon
  to its own 16-bit range (`gpu3d.cpp` wshifted, carried in vt.z), so it is
  `1 / vt.z` with a GREATER test -- see "W-buffer depth" below.
- One draw call per translucent polygon with stencil state churn. Keep
  the id rule in the stencil but bake it into the pipeline state and draw
  runs of same-state polygons; `VK_EXT_rasterization_order_attachment_access`
  (present on this driver) lets the fragment shader read the destination
  in order, so the whole DS blend -- including the id rule and shadow
  polygons -- can be programmable in ONE translucent draw with no stencil
  at all. Measure both.
- The edge/fog passes re-read depth and attribute as textures. Do them as
  a second subpass with input attachments where the read is same-pixel
  (fog), and accept one render-pass split for edge marking's neighbours.
- Per-draw `glTexParameteri` wrap changes -> immutable samplers per wrap mode.

Two things the fan design cannot do and the compute path can: exact span
coverage (so the 1x GPU path stays the compute raster where exactness at 1x
matters) and anti-aliasing's coverage stack (the DS AA is an edge-coverage
blend; at hi-res the resolution itself is the anti-aliasing, and the
frontend defaults `video.aa` off anyway).

## P1 — the GPU present stage: LANDED (`--gpu-present` / `video.gpu_present`, opt-in)

`src/frontend/sdl/gpu_present.{h,cpp}` + `shaders/present.comp`. On a
DmabufOut/DrmOut tier the tier's CMA buffers are imported as LINEAR images
(`ScanoutOut::dmabuf_plane`), the core writes its 256x192 frames (the
`draw()` path, no ScaleTarget), and one compute dispatch per frame lays
out, scales (nearest), rotates and blends the inset into the buffer, which
the tier presents unchanged. Pipelined one deep: the fence wait and the
tier's `end_frame()` for frame N happen at the start of frame N+1. Two
things had to follow: the tier rotates one more buffer under the GPU stage
(`set_bufs`), and it skips the dma-buf sync ioctls (`set_gpu_writes`) --
the END|WRITE clean of a 3 MB buffer was 0.5 ms per panel, needless when
the GPU wrote it.

Device, SDL `--dual-window`, `DS_FRAME_STATS=1`, same binary, flag on/off,
both orders. **`work ms` = emulation + present** (the `frame ms` line
excludes present, which is where this stage's own cost lands):

| scene | governor | GPU present, work median (over budget) | CPU scaler |
| --- | --- | --- | --- |
| Spirit Tracks | performance | **18.6-18.9 (50 %)** | 21.1 (88-91 %) |
| NSMB | performance | **12.7-12.9 (1.3-2.6 %)** | 14.9-15.3 (12-13 %) |
| Golden Sun | performance | 19.4-19.8 (96 %) | 19.7-20.1 (96 %) |
| NSMB | ondemand | **12.8-13.3 (5-11 %)** | 15.4 (16 %) |

Per display per frame the stage costs: retire 0.08-0.15 ms (fence +
end_frame), begin_frame ~0.01, upload (two 192 KB memcpys into
write-combine + flush) 0.2, record + submit 0.13-0.17. GPU time per panel
~1.25 ms (P0.1), never waited for. Golden Sun does not move because its
frame is the emulation thread with the scaler already off it.

Checked by eye on the device (grim): dual window, single window, rotation
90 on both panels, the PiP inset (its alpha blend still to be checked with `--pip-alpha 0.5`; 128 clamps to opaque), the
FPS overlay. Left for later: the pause menu and notices take the DS-space
path on this tier (`canvas_capable()` is false, as on the SDL_Renderer
tier), so they come out at DS resolution -- a panel-resolution overlay
plane is the follow-up; the box filter / grid / bilinear / chunky tables
are P4; nearest here is `floor(x * 256 / w)`, not the CPU's run table, so
a run boundary can differ by a pixel (allowed on the GPU path; P4 adopts
the tables).

## P2a — the compute raster transplanted onto main: LANDED (`--gpu-raster`, opt-in)

The gpu-raster branch's hooks (`gpu_supported`, `gpu_dispatch`, the seam
after the texcache resolve, `FrameRef::gpu`, the A/B harness `--gpu-ab`,
the `video.gpu_raster` knob which still implies `emu.timing_oc`) applied
onto main by three-way merge; three conflicts in `Renderer3D::render`, all
the branch's supersets of main's profiling lines. Gates: scene hashes of
etody and mlbis (300 frames) identical to main with the knobs off; host
A/B identical; device A/B Golden Sun 340/340 identical, Etrian Odyssey 14
of 388 frames differ by 26 pixels (the branch's known seam residual).
GPU times on the device in A/B mode (CPU drawing beside it): etody 4.4 ms,
gsdd 10.5. Not yet changed: the upload still runs on the emulation thread
(P2b moves it), and the branch's deferred-composite knob came along
(`video.gpu_defer`, measured worthless there; left off).

## P2b — the GPU job thread: LANDED, and the compute raster's verdict at 1x

`Renderer3D` now hands a GPU frame to its own thread (`gpu_thread_main`):
the polygon conversion, the upload and the submit run there, and a frame
the upload refuses is drawn by the CPU raster on that thread into `out_[]`
(`FrameRef::job`; `frame_ref()` waits for the decision, `sync_all()` joins
the job). `DS_GPU_THREAD=0` keeps the old inline path. Frame hashes of the
threaded and inline paths are identical (etody, mlbis, 600 frames, host).
The core and the frontend now share one Vulkan context
(`vk::Device::shared`), which P2c/P3 need to bind the raster's layer from
the present stage.

**The gate the plan set for P2 -- "emulation median not worse than software
+ untimed geometry" -- FAILS, and not because of the upload.** Device, SDL
dual window, GPU present on in every arm, work median (emulation + present):

| scene | software + timing_oc | compute raster, job thread | inline | compositor's fence wait |
| --- | --- | --- | --- | --- |
| NSMB | 11.0 | 11.1-11.2 | 11.6 | 0.11 ms |
| Spirit Tracks | 15.8-16.0 | 16.6-17.2 | 17.1 | 1.2-2.3 ms |
| Golden Sun | 16.8 | 22.7-23.4 | 23.0-23.7 | 6.5-8.0 ms |
| Etrian Odyssey | 6.8-6.9 | 14.7-14.9 | 14.7-14.8 | 5.3-6.0 ms |

The thread saves its 0.5 ms on NSMB and nothing elsewhere: the term is the
compositor waiting for the GPU at line 0. The raster dispatches at line 215
and the display asks for line 0 about 3 ms later; a compute frame of 3-7 ms
cannot be there, and on Golden Sun (engine A per line, capture every frame)
the wait lands squarely on the emulation thread. The deferred composite
would hide it but is refused exactly where it is needed (capture). So on
this device the compute raster is not a frame-time path at 1x either; it
stays as the **exact GPU reference, opt-in** (`--gpu-raster`), and the
freed cores do not pay for its latency.

**Consequence for the plan.** The graphics-pipeline renderer of P0.3 draws
these scenes in 0.5-2 ms at 1x (fans + texel), which fits the window with
room for its real shading. It therefore becomes the GPU 3D path at 1x as
well as at hi-res (inexact coverage, allowed by the user's decision), fed
through the same seam and `FrameRef` the compute raster uses, so P3 lands
first at S=1 against the same A/B and frame-time gates, and the composite
work (P2c) follows for S>=2.

## P3 at 1x — the triangle path: LANDED (default GPU raster; `DS_VK_MODE=compute` for the old one)

`vk_raster.cpp` grew a second mode: polygons drawn as fans through the
hardware rasteriser (`shaders/tri.vert`, `tri_opaque.frag`, `tri_tail.frag`,
`tri_frag_common.glsl` over `ds_shade.glsl`), one render pass with three
R32_UINT colour attachments (the layer record, the attribute plane, the DS
depth plane) and a D32 depth buffer, then copies into the compute path's
`out[]`/`depth`/`attr` buffers -- so the final pass (fog, edge marking), the
output contract, `FrameRef`, the A/B harness and the host see exactly what
the compute passes gave them. The opaque prefix is one draw with the
hardware depth test in submission order (LESS on z, or GREATER on 1 / the
normalised w for W-buffer frames, chosen by `GpuFrame::flags`); the translucent tail is drawn
in runs by depth-write bit with the DS blend and the equal-id rule in the
fragment stage, reading the attachments it writes -- ordered per pixel by
`VK_EXT_rasterization_order_attachment_access` on libmali, or behind a
per-polygon barrier on drivers without it (RADV). Not yet: shadow masks and
shadow polygons (skipped), edge flags (edge marking marks nothing), the
mode-1 back-facing depth rule, anti-aliasing (gated as before).

Fill and sampling rules that had to be learnt from the picture (user's eye on
the Etrian menu, `tools/compare_frames.py` and a thresholded red overlay of
the differing pixels), with the count of pixels differing from the software
raster over Etrian's replay (700 frames, 488 rasterised):
- vertices at the pixel centre, (sx + 0.5, sy + 0.5): that is where the DS
  evaluates a pixel's attributes (its integer position), so texture rows and
  columns land where the CPU puts them; the right-side vertices a hair
  (0.001 px) further so the inclusive last column of the span is covered
  (a whole-pixel push covers it too but stretches the attributes: shifted
  glyphs, wrong corners);
- texture coordinates TRUNCATE (12.4 >> 4, as the DS) with a 0.01-unit bias
  before the floor; colours ROUND. The menu panels stretch ONE texel column
  across 102 pixels (s 22.0 -> 23.0) and step t by exactly 16.0 a row, so
  rounding flipped columns onto the edge texel early (the "vertical cut")
  and float error floored 15.9999 onto the row above (the one-row shift in
  the text);
- zero-width / zero-height polygons get a pixel of extent (a line to the DS).
Measured along the way: round everything 5.0 M pixels but the cut; floor
everything 7.0 M; whole-pixel push 6.1 M; final 5.0 M with 249 of 488
frames differing and the menu clean. `DS_GPU_DUMP_AT=x,y` prints the
polygons over a pixel, which is how the stretched quad was found.

Device (SDL, dual window, GPU present on, work median, both orders):

| scene | software + timing_oc | triangle path | over budget |
| --- | --- | --- | --- |
| Golden Sun | 16.8 | **15.5-15.6** | 52 % -> 28 % |
| Spirit Tracks | 15.9 | **15.7** | 17-23 % -> 10-12 % |
| NSMB | 10.9-11.0 | 11.0-11.2 | 0.2 % -> 0.1 % |
| Etrian Odyssey | 6.7 | 6.6-6.7 | 2.3 % -> 2.1 % |

GPU time per frame (device, A/B mode): Golden Sun 9.7 ms raster + 0.9 final
pass -- higher than the P0.3 probe's 2 ms because this is the full shading
with the real tail, the three attachments and the copies, and the A/B run
draws on the CPU beside it; the compositor's fence wait is 0.1-0.6 ms
against the compute raster's 6.5-8. The first GPU 3D path that is never
slower than the software one here, and it idles the three band workers.

## P2c + P3 hi-res — the GPU composite and `--internal-res N`: LANDED (opt-in)

- Core: `Engine2D::set_layer_export` / `export_planes` -- a 3D line takes the
  full select and copies `resolve16_full`'s top, second, ids, kind, alpha
  and window into 256x192 planes the frontend owns; `Gpu::LayerExport` adds
  the per-line BLDCNT word and MASTER_BRIGHT with bit 31 = exported. The
  CPU composite still runs, so `fb_`, capture and states are untouched.
- Raster: `DS_VK_SCALE` / `video.internal_res` scales the triangle path's
  coordinates on upload; `out[]` is the hi-res layer and `downsample.comp`
  makes the native plane the CPU reads (top-left subpixel; at S=2 the A/B
  against the software raster matches S=1 to within a few pixels).
  `FrameRef` carries the frame's hi-res buffer; `Gpu::frame_hires` hands it
  to the frontend, which is what keeps the composite on the frame the
  display lines used rather than the one the raster submitted at line 215.
- Frontend: `shaders/composite.comp` (kern::composite_line with the hi-res
  3D pixel substituted, then master brightness and the 6->8 expansion) runs
  in the present stage's command buffer before `present.comp`, which samples
  screen 0 from the composited buffer at S x. Verified by reading the tier's
  own panel buffer back at frame 400 (`DS_GPU_COMP_DUMP`): the Etrian logo at
  1024x768 composited at 2x. (Screenshots timed with `grim` caught the
  intro's fades and showed black; the readback is the tool.)

Known limits: an OSD label or the save flash drawn into screen 0's copy does
not show on 3D lines (the composite takes the planes) until the overlays get
their own plane; the composite at S>1 chooses the layer beneath by the
native pixel's kind, so a hi-res 3D silhouette over 2D is native-res-exact
only at the pixel level.

Cost, the thing to fix next (device, headless, `DS_VK_TIMING=1`, GPU ms):

| gsdd | full | flat fragment | nothing drawn | nothing drawn, no copies |
| --- | --- | --- | --- | --- |
| 1x | 6.6 | 4.3 | 2.9 | **0.9** |
| 2x | 8.8 | 5.7 | | |

| etody 1x | full 2.2 | flat 2.2 | nothing drawn 2.0 |

The three image-to-buffer copies of the attachments cost ~1.9 ms a frame at
1x (a tiled-to-linear conversion the driver does slowly), which is most of
Etrian's whole pass and a third of Golden Sun's; the real fragment stage is
2.2 ms on Golden Sun over the flat one, and the tail is free. Next: render
into LINEAR images aliased on the buffers' memory (no copies), then the
fragment stage. Timestamps inside the render pass read zero on this tiler,
so attribution is by leaving parts out (`DS_VK_TRI_NOTAIL`, `_NOOPAQUE`,
`_NOCOPY`, `DS_VK_TRI_FLAT`).

## Aliased attachments, the overlay plane, and the 2x gate (2026-09-18)

**No more copies.** The triangle path's colour, attribute and depth-plane
attachments are LINEAR images bound to the memory of `out[i]`, `attr` and
`depth` (`alias_image` in `tri_setup`; libmali renders R32_UINT linear, and
asks 198208 bytes for a 196608-byte plane, so the buffers carry 16 KB of
slack). Where the driver refuses, the copies remain (`DS_VK_TRI_COPY=1`
forces them). Headless GPU ms per frame: Etrian 2.2 -> 1.9 at 1x, 4.2 at 2x;
Golden Sun 6.6 -> 7.7 (noise across runs; its pass is fragment work). Same
A/B residuals to the pixel.

**The overlay plane.** `GpuPresent::overlay()` is the GPU tier's canvas
(`Display::canvas`/`canvas_capable`): a host-cached plane per slot in the
logical frame, cleared where the frame before last drew, blended last by
`present.comp`. The OSD, toasts, the pause menu and the loader notice come
out at panel resolution again on this tier, over the composited 3D screen.
Two traps that cost a session: the overlays were sized at open, when the
window is still 512x384, and `reimport()` after the fullscreen resize kept
them -- a GPU read fault (dmesg `JOB_READ_FAULT`, `vkWaitForFences`
returning DEVICE_LOST after 25 frames) that showed as a black picture with
a flickering label; and `note_canvas_draw_all()` measured the canvas by the
scanline path's frame size, zero here, so nothing was blended at all. The
unscaled canvas path now notes only the label rectangles.

**The gate at 1x and 2x** (device, dual window, GPU present, work median,
both orders; `sw-oc` = software raster + untimed geometry):

| scene | sw-oc | triangle 1x | triangle 2x |
| --- | --- | --- | --- |
| Golden Sun | 16.9 | **15.1-15.6** | 20.8 |
| Spirit Tracks | 16.3-16.5 | 16.5-16.7 | 16.8 |
| NSMB | 11.3-11.4 | 11.2-11.7 | 11.5 |
| Etrian Odyssey | 7.6-7.7 | 7.6 | 14.1 |

At 1x: never worse, Golden Sun 1.5 ms better, the three band-worker cores
idle. At 2x NSMB is free and Etrian and Golden Sun pay 4-6.5 ms, all of it
the compositor's fence wait at line 0: a 2x pass of 4-10 ms plus the 2x
composite and present on the shared GPU cannot land in the ~3 ms between the
dispatch at line 215 and the first display line. So `video.internal_res`
stays a per-game choice for now; the plan's auto cap (drop to 1x after N
over-budget fence waits) is the follow-up, and the fragment stage is the
remaining GPU lever (flat shading measured 2.2 ms cheaper on Golden Sun).

## Shadow volumes on the triangle path (2026-09-18)

The depth attachment is D24_UNORM_S8 (or D32_S8) where the driver has it;
a run of mask polygons clears the stencil and draws with no colour and no
depth write, setting the stencil where its depth test FAILS (the volume's
interior); a run of shadow polygons draws with the stencil test EQUAL and
the shadow's own id rule in `tri_tail.frag` (against the destination's
translucent id when it has one, else its opaque id). The DS clears its
stencil per scanline when a run begins; whole-frame is the approximation.
Host A/B, stencil off -> on: Spirit Tracks 4.78 M -> 4.54 M differing
pixels over 300 frames, Dragon Ball 12006 -> 11905 (the rest is the usual
rounding residual). `DS_VK_TRI_NOSTENCIL=1` measures without.

## W-buffer depth and the swapped screen (2026-09-18)

Spirit Tracks on the panel showed the hills in front of the train and the
shadows as spikes. Two bugs, both found with `--gpu-ab-dump` on the host
(`tools/compare_frames.py --png`) and `DS_GPU_DUMP_AT=x,y`:

1. **The W-buffer depth was 1 / vt.w.** vt.w is the interpolation w; the DS
   compares `wshifted` (gpu3d.cpp:1292), w normalised per polygon to its own
   16-bit range, which travels in vt.z. Polygons of a different w size do
   not share a scale, so 1 / vt.w ranked the hills (w ~5500, z = 16 w) in
   front of the train (w ~37000, z = w). Now `gl_Position.z = w / vt.z`
   (perspective-correct, exactly the DS's hyperbolic interpolation of W),
   GREATER, cleared to 1 / clear_depth, and the depth format prefers
   D32_SFLOAT_S8 (24 fixed bits of 1 / 40000 resolve ~100 DS units; the
   shadow volumes sit within a few hundred of the ground). Frame 399 of
   st-intro: 32851 -> 12559 differing pixels, the rest texel rounding.
2. **The GPU composite was one frame ahead and always on screen 0.**
   `Gpu::begin_frame` runs at line 0 BEFORE run_frame returns, so the
   frontend read the coming frame's 3D layer against the finished frame's
   planes; and Spirit Tracks flips POWCNT1 bit 15 every frame (30 Hz
   alternation through display capture), so engine A's layer belongs to a
   different screen each frame. Both panels showed the same scene,
   alternating. `frame_hires` now returns the layer latched at begin_frame
   plus engine A's screen; `GpuPresent::present` composites that screen
   (`view.w` in present.comp names it, composite.comp reads that screen's
   fb). Verified by `DS_GPU_COMP_DUMP` (now per display and presented
   frame, 400-403): four consecutive frames, both panels steady.

## Edge marking and the facing rule on the triangle path (2026-09-18)

- **Edge marking** ran (the final pass is shared with the compute path) but
  never marked: it tests bits 0-3 of the attribute record, the span
  raster's edge flags, and the triangle shaders wrote none. Every opaque
  pixel now carries 0xF and the neighbour id + depth test alone decides,
  as melonDS's GL renderer does. Spirit Tracks' character outlines appear
  (frame 400 of st-intro, before/after crops in the session record).
  Scenes that use it: st (every frame, with fog); gsdd has fog only;
  etody/mlbis/sm64/meteos neither (new "with edge marking / with fog"
  counters in the headless stats).
- **Depth mode 1** (`Renderer3D::depth_pass`): a front-facing polygon takes
  an opaque back-facing pixel at EQUAL depth. Without it the pixel column
  where a side face and a front face share a vertical edge went to
  whichever was drawn first (a dark 1 px line right of the train's wheel,
  x=206 rows 45-76 of frame 399). The opaque prefix is now two instanced
  draws over the same range: back faces with LESS, then front faces with
  LESS_OR_EQUAL (GREATER / GREATER_OR_EQUAL in W-buffer mode); tri.vert
  collapses the polygons of the other facing from `GpuFrame::flags`
  (DS_FF_FACE_BACK/FRONT pushed between the draws). Front-over-front at
  equal depth now goes to the later polygon where the DS keeps the earlier
  (same surface in practice). Device cost: none measurable (st 1x 15.1 ms,
  gsdd 15.3). Equal-depth mode (attr bit 14, +-0x200 tolerance) is still
  LESS.
- `DS_VK_TRI_IDCOL=1` colours every opaque pixel by polygon index (r = i &
  63, g = i >> 6, b = i >> 12) so an A/B dump names the owner of a pixel;
  with `DS_GPU_DUMP_AT=x,y` that is the whole diagnosis loop.

## GPU-side stalls (2026-09-18, OPEN)

Single frames of 270-976 ms at random points (about one per 25 s at 1x,
three per 25 s at 2x on st). The new stall report (line waits over 50 ms)
shows the job thread woke within 0.1 ms and uploaded in 1-2 ms every
time: the fence itself took the whole stall, i.e. the GPU frame completed
late. No kernel messages, no page-reclaim counters moving, compute mode
not yet caught in the act. Suspects: kbase's completion workqueue starved
by the process's SCHED_RR threads until RT throttling (950 ms / 1 s -- the
magnitude fits), tiler-heap regrowth through GPU page faults (fits "more
at 2x").

Later the same day: with the GPU devfreq governor set to `performance`
(800 MHz) the stall came on EVERY run at the SAME frame (st 2x: nds frame
986, three runs; gsdd 1x: frame 271) -- and frame 986 holds FIVE polygons
(the sky screen), so the GPU was not slow on our work, its completion was
held. Then, with a rebuilt binary carrying the stall-triggered panel dump,
the same command did not stall in two 1500-frame runs; nor did four
ondemand runs, nor two with emu.realtime=off. Panels around frame 986
(both displays, ten frames) are consistent: no edge-marking or composite
transition. Tools left in place: `DS_GPU_COMP_DUMP=<f>
DS_GPU_COMP_DUMP_ON_STALL=1 [DS_GPU_COMP_DUMP_COUNT=n]` dumps both panels
for the frames presented after each stall report; `DS_GPU_DUMP_FRAME=N`
prints every polygon of NDS frame N; `DS_GPU_COMP_DUMP_FROM/COUNT` set a
fixed window. Verdict so far: a driver-side hold of GPU job completion,
timing-dependent, not content-dependent and not the frame limiter (the
wait is inside the raster's fence at line 0).
