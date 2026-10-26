#version 460
// A GRAPHICS fragment program of the tests: the vertices' colour.
layout(location = 0) in vec4 color;
layout(location = 0) out vec4 outColor;
void main() { outColor = color; }
