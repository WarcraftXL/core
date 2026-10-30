#version 460
// A FULLSCREEN program of the tests: the colour of its push constants.
#extension GL_GOOGLE_include_directive : require
#include "wxl/render/render.glsl"
layout(push_constant) uniform Push { vec4 color; } pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() { outColor = pc.color; }
