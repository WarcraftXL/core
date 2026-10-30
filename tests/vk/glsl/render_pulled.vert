#version 460
// A GRAPHICS vertex program of the tests: its vertices read from a buffer by address (x, y in
// clip space, then a colour), indexed by gl_VertexIndex.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Vertices { vec4 v[]; };
layout(push_constant) uniform Push { uvec2 vertices; } pc;
layout(location = 0) out vec4 color;
void main()
{
    Vertices verts = Vertices(pc.vertices);
    const vec4 p   = verts.v[gl_VertexIndex * 2];
    color          = verts.v[gl_VertexIndex * 2 + 1];
    gl_Position    = vec4(p.xy, 0.5, 1.0);
}
