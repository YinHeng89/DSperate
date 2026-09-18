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
  in the VERTEX shader for both modes (W-buffer depth is linear in 1/w, so
  a perspective-correct interpolation of w through `gl_Position.w` gives it)
  and keep fixed-function depth.
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
