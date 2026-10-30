#version 460
// A BATCH program of the tests: row 0 of the batch's bone named by the mod's first constant, read
// through batch.glsl's WxlBoneRow (the palette the batch drew with, or its registers).
#extension GL_GOOGLE_include_directive : require
#include "wxl/render/batch.glsl"
layout(location = 0) out vec4 outColor;
void main() { outColor = WxlBoneRow(uint(WxlConstant(0).x), 0u); }
