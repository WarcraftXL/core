#version 460
// A FULLSCREEN program of the tests: row `index` of the vec4 rows at an address (a buffer a
// dispatch filled, a bone palette), as the colour.
#extension GL_GOOGLE_include_directive : require
#include "wxl/render/render.glsl"
layout(push_constant) uniform Push { uvec2 rows; uint index; } pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() { outColor = WxlRows(pc.rows).rows[pc.index]; }
