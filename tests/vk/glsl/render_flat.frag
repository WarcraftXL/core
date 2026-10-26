#version 460
// A BATCH program of the tests: the mod's first constant, and its second into a second attachment.
#extension GL_GOOGLE_include_directive : require
#include "wxl/render/batch.glsl"
layout(location = 0) out vec4 outColor;
void main() { outColor = WxlConstant(0); }
