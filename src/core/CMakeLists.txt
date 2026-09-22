# SPDX-License-Identifier: GPL-3.0-or-later
add_library(dsperate_core STATIC
  nds.cpp
  nds_dsi.cpp
  crypto/aes.c
  crypto/sha1.cpp
  bios/freebios_data.cpp
  bios/firmware_gen.cpp
  profile.cpp
  host_cores.cpp
  pc_sampler.cpp
  mem/page_table.cpp
  mem/fastmem_census.cpp
  mem/fastmem.cpp
  mem/bus.cpp
  mem/timing.cpp
  dma/dma.cpp
  dma/ndma.cpp
  cart/cart.cpp
  cart/rom_source.cpp
  cart/zip.cpp
  cart/zip_cache.cpp
  cart/miniz/miniz_tinfl.c
  cart/miniz/miniz_tdef.c
  cart/miniz/miniz.c
  cheat/ar_engine.cpp
  cheat/database.cpp
  sched/scheduler.cpp
  cpu/cpu.cpp
  cpu/cp15.cpp
  cpu/idle_loop.cpp
  cpu/interp/interp.cpp
  cpu/interp/interp_arm.cpp
  cpu/interp/interp_thumb.cpp
  gpu/gpu.cpp
  gpu/pow_compat.c
  gpu/engine2d.cpp
  gpu/vram_map.cpp
  gpu/gpu3d.cpp
  gpu/render3d.cpp
  gpu/texcache.cpp
  gpu/kernels_ref.cpp
  spu/spu.cpp
  input/input_log.cpp
  io/io.cpp
  io/wifi.cpp
  io/dsi_aes.cpp
  io/dsi_dsp.cpp
  io/dsi_camera.cpp
  io/dsi_sd.cpp
  io/dsi_nand_fs.cpp
  io/dsi_nand_persist.cpp
  io/dsi_title_install.cpp
  io/dsi_nand_launch.cpp
  io/dsi_nand_synth.cpp
  io/dsi_sd_card.cpp
  io/dsi_font.cpp
  io/dsi_nwifi.cpp
)
target_include_directories(dsperate_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# The .symver in pow_compat.c only reaches the binary if this TU is kept out
# of LTO; see the file's comment.
set_source_files_properties(gpu/pow_compat.c PROPERTIES
  COMPILE_OPTIONS "-fno-lto")

# The vendored miniz DEFLATE cores (cart/miniz/README.md): inflate for the
# ZIP reader (the archive parsing in cart/zip.cpp is ours) and deflate for the
# PNG writer the frontend's auto-save thumbnail uses. miniz's own ZIP reader
# is not vendored. miniz.c is compiled without its zlib-style API, which
# leaves the CRC-32/Adler-32 and mz_free the PNG writer calls.
set_source_files_properties(cart/miniz/miniz_tinfl.c PROPERTIES
  COMPILE_DEFINITIONS "MINIZ_NO_DEFLATE_APIS;MINIZ_NO_STDIO;MINIZ_NO_TIME"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/cart/miniz")
set_source_files_properties(cart/miniz/miniz.c PROPERTIES
  COMPILE_DEFINITIONS "MINIZ_NO_ZLIB_APIS;MINIZ_NO_STDIO;MINIZ_NO_TIME"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/cart/miniz")
set_source_files_properties(cart/miniz/miniz_tdef.c PROPERTIES
  COMPILE_DEFINITIONS "MINIZ_NO_INFLATE_APIS;MINIZ_NO_STDIO;MINIZ_NO_TIME"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/cart/miniz")

if(DSPERATE_JIT)
  # The runtime is host-agnostic; the backend directory is the host's.
  target_sources(dsperate_core PRIVATE cpu/jit/runtime.cpp cpu/jit/bios_sha1.cpp)
  if(DSPERATE_HOST_AARCH64)
    target_sources(dsperate_core PRIVATE cpu/jit/a64/stubs.cpp cpu/jit/a64/translate.cpp)
  elseif(DSPERATE_HOST_ARM32)
    target_sources(dsperate_core PRIVATE cpu/jit/a32/stubs.cpp cpu/jit/a32/translate.cpp)
    target_compile_definitions(dsperate_core PUBLIC DSPERATE_JIT_A32=1)
  endif()
  target_compile_definitions(dsperate_core PUBLIC DSPERATE_JIT=1)
endif()
if(DSPERATE_NEON)
  target_sources(dsperate_core PRIVATE gpu/kernels_neon.cpp)
  target_compile_definitions(dsperate_core PUBLIC DSPERATE_NEON=1)
  # The kernels reach A64-only intrinsics through gpu/neon_compat.h; on an
  # ARMv7 host that header switches to bodies the A32 encoding can express.
  # DSPERATE_A32_SUBSET forces the same bodies on an AArch64 host (kernels only;
  # not a 32-bit build) to test the subset without an ARMv7 machine.
  if(DSPERATE_HOST_ARM32 OR DSPERATE_A32_SUBSET)
    target_compile_definitions(dsperate_core PUBLIC DS_A32_SUBSET=1)
    message(STATUS "DSperate: NEON kernels restricted to the ARMv7-expressible subset")
  endif()
endif()

# DSperate's DSi system fonts are linked in with .incbin (io/dsi_font.cpp):
# the normal table, and China's and Korea's.
set_property(SOURCE io/dsi_font.cpp APPEND PROPERTY COMPILE_DEFINITIONS
             DSPERATE_DSI_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/io/dsi_font")
set_property(SOURCE io/dsi_font.cpp APPEND PROPERTY OBJECT_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/io/dsi_font/TWLFontTable.dat"
             "${CMAKE_CURRENT_SOURCE_DIR}/io/dsi_font/TWLFontTable-cn.dat"
             "${CMAKE_CURRENT_SOURCE_DIR}/io/dsi_font/TWLFontTable-kr.dat")
# The Vulkan device layer and GPU raster. Opt-in at build time and at run
# time both: the backend loads libvulkan with dlopen, so a build with this on
# still starts on a device with no driver -- Device::create returns null and
# the software raster runs. Only the aarch64 handhelds have a GPU that can
# host it at all (the A30's Mali-400 has no compute units).
option(DSPERATE_VULKAN "Build the Vulkan compute 3D raster" ON)
if(DSPERATE_VULKAN)
  # The Vulkan headers are architecture-independent and nothing is linked
  # against (vk_device.cpp dlopens libvulkan on the device), so a cross build
  # may use the host's copy: without NO_CMAKE_FIND_ROOT_PATH the aarch64
  # toolchain looks only inside its sysroot and silently builds the handhelds
  # -- the only targets with a GPU that can run this -- without the raster.
  find_path(DSPERATE_VULKAN_INCLUDE vulkan/vulkan.h)
  if(NOT DSPERATE_VULKAN_INCLUDE AND CMAKE_CROSSCOMPILING)
    find_path(DSPERATE_VULKAN_INCLUDE vulkan/vulkan.h NO_CMAKE_FIND_ROOT_PATH)
  endif()
  if(DSPERATE_VULKAN_INCLUDE)
    target_sources(dsperate_core PRIVATE gpu/vk/vk_device.cpp gpu/vk/vk_raster.cpp gpu/vk/vk_shaders.cpp)
    # SPIR-V is checked in and .incbin'd by vk_shaders.cpp (no shader compiler
    # needed at build time); re-link when a blob changes.
    set_property(SOURCE gpu/vk/vk_shaders.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 DSPERATE_VK_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders")
    set_property(SOURCE gpu/vk/vk_shaders.cpp APPEND PROPERTY OBJECT_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/bin.spv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/raster.spv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/post.spv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/span.spv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/vis.spv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/raster_vis.spv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/tri_vert.spv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/tri_opaque_frag.spv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/tri_tail_frag.spv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/downsample.spv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/tri_flat_frag.spv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/tri_mask_frag.spv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/tri_pre_frag.spv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/expand.spv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/tri_opaque_aa_frag.spv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/shaders/tri_tail_aa_frag.spv")
    target_include_directories(dsperate_core PRIVATE ${DSPERATE_VULKAN_INCLUDE})
    target_compile_definitions(dsperate_core PUBLIC DSPERATE_VULKAN=1)
    # No link against libvulkan on purpose: vk_device.cpp dlopens it.
    message(STATUS "DSperate: Vulkan compute raster enabled (headers at ${DSPERATE_VULKAN_INCLUDE})")
  else()
    set(DSPERATE_VULKAN OFF)
    message(STATUS "DSperate: Vulkan headers not found, GPU raster disabled")
  endif()
endif()
