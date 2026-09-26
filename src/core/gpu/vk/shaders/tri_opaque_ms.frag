#version 450
// 4x MSAA: the same, with alpha to coverage for textures with transparent texels.
#extension GL_GOOGLE_include_directive : require
#define DS_MSAA 1
#include "tri_opaque_body.glsl"
