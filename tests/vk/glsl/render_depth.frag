#version 460
// A FULLSCREEN program of the tests: a depth image's value, as red.
#extension GL_GOOGLE_include_directive : require
#include "wxl/render/render.glsl"
layout(push_constant) uniform Push { uint image; uint samplerIndex; } pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(WxlSample(pc.image, pc.samplerIndex, uv).r, 0.0, 0.0, 1.0); }
