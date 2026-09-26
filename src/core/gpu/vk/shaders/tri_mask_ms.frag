#version 450
// 4x MSAA: the same, once per sample (it reads the attachments it blends against).
#extension GL_GOOGLE_include_directive : require
#define DS_MSAA 1
#define DS_PER_SAMPLE 1
#include "tri_mask_body.glsl"
