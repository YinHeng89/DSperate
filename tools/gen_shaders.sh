#!/bin/sh
# Compile the GPU raster's compute shaders to SPIR-V and check the blobs in.
#
# The .spv files next to the .comp sources are generated but tracked, so a
# build needs no shader compiler -- vk_shaders.cpp .incbin's them, the way
# io/dsi_font.cpp does its font tables. Run this after editing any .comp and
# commit the .spv beside it.
#
#   tools/gen_shaders.sh
#
# The compiler is, in order of preference:
#   $GLSLANG                          an explicit override
#   toolchains/glslang/bin/...        the vendored prebuilt (see toolchains/README)
#   glslangValidator / glslc          whatever is on PATH
# Blobs are validated with spirv-val when it is available, because a shader
# that compiles is not necessarily one the driver will accept.
set -e
DIR=$(cd "$(dirname "$0")/.." && pwd)
SRC="$DIR/src/core/gpu/vk/shaders"
INC="$DIR/src/core/gpu/vk"

if [ -n "$GLSLANG" ]; then GV="$GLSLANG"
elif [ -x "$DIR/../toolchains/glslang/bin/glslangValidator" ]; then GV="$DIR/../toolchains/glslang/bin/glslangValidator"
elif command -v glslangValidator >/dev/null 2>&1; then GV=glslangValidator
elif command -v glslc >/dev/null 2>&1; then GV=glslc
else
  echo "gen_shaders: no GLSL compiler found." >&2
  echo "  fetch one:  mkdir -p toolchains/glslang && cd toolchains/glslang &&" >&2
  echo "              curl -sSL -o g.zip https://github.com/KhronosGroup/glslang/releases/download/master-tot/glslang-master-linux-Release.zip &&" >&2
  echo "              unzip -oq g.zip && rm g.zip" >&2
  exit 1
fi

for f in "$DIR"/src/frontend/sdl/shaders/*.comp; do
  [ -e "$f" ] || continue
  out="${f%.comp}.spv"
  case "$GV" in
    *glslc) "$GV" -O --target-env=vulkan1.1 "$f" -o "$out" ;;
    *)      "$GV" -V --target-env vulkan1.1 "$f" -o "$out" >/dev/null ;;
  esac
  if command -v spirv-val >/dev/null 2>&1; then spirv-val "$out"; fi
done
for f in "$SRC"/*.comp; do
  out="${f%.comp}.spv"
  case "$GV" in
    *glslc) "$GV" -O --target-env=vulkan1.1 "-I$INC" "$f" -o "$out" ;;
    *)      "$GV" -V --target-env vulkan1.1 "-I$INC" "$f" -o "$out" >/dev/null ;;
  esac
  if command -v spirv-val >/dev/null 2>&1; then spirv-val "$out"; fi
  printf '%-16s %6d bytes\n' "$(basename "$out")" "$(wc -c < "$out")"
done
echo "gen_shaders: done -- commit the .spv files beside their sources"
