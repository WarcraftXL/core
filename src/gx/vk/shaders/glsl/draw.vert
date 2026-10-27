#version 460
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw's vertex program on the Vulkan renderer, a GRAPHICS program of wxl.render: the Direct3D 9
// program DrawVs (gx/d3d9/shaders/draw.hlsl) line for line, its vertices read from the frame's
// memory by address (gx/shapes/Geometry.hpp, Vertex: 32 bytes), its picture turned upright for
// Vulkan's y at the end.

#extension GL_GOOGLE_include_directive : require
#include "wxl/render/render.glsl"

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer DrawVertices
{
    uint words[];
};

layout(push_constant) uniform Push
{
    vec4  viewColumns[4]; ///< the view-projection's columns: clip = dot([p 1], column)
    vec4  screen;         ///< the viewport's width and height in pixels, half a pixel's width at 1080 lines / 8
    vec4  lift;           ///< the pull towards the eye: a fraction of the distance, yards
    uvec2 vertices;       ///< the stream's address
    uvec2 surface;        ///< a surface's numbers (draw_surface.frag)
} pc;

layout(location = 0) out vec4 color;
layout(location = 1) out vec3 line;  ///< a line's: pixels across from its middle, half its drawn width, its softness
layout(location = 2) out vec3 plane; ///< a surface's: its plane's x and y, a polygon's distance to its border

vec4 Clip(vec3 p)
{
    const vec4 v = vec4(p, 1.0);
    return vec4(dot(v, pc.viewColumns[0]), dot(v, pc.viewColumns[1]), dot(v, pc.viewColumns[2]), dot(v, pc.viewColumns[3]));
}

vec3 Lifted(vec3 p, float on)
{
    const float d    = length(p);
    const float pull = min(on * (pc.lift.x * d + pc.lift.y), 0.5 * d);
    return p * (1.0 - pull / max(d, 0.0001));
}

void main()
{
    DrawVertices stream = DrawVertices(pc.vertices);
    const uint   at     = uint(gl_VertexIndex) * 8u;
    const vec3   position = vec3(uintBitsToFloat(stream.words[at]), uintBitsToFloat(stream.words[at + 1u]), uintBitsToFloat(stream.words[at + 2u]));
    const vec3   other    = vec3(uintBitsToFloat(stream.words[at + 3u]), uintBitsToFloat(stream.words[at + 4u]), uintBitsToFloat(stream.words[at + 5u]));
    const uint   argb     = stream.words[at + 6u];
    const uint   bytes    = stream.words[at + 7u];
    const vec4   data     = vec4(float(bytes & 255u), float((bytes >> 8) & 255u), float((bytes >> 16) & 255u), float(bytes >> 24));

    const float on    = mod(data.w, 2.0);
    const float cap   = mod(floor(data.w * 0.5), 2.0);
    const float soft  = floor(data.w * 0.25);
    float       down  = floor(data.x * 0.25);
    float       right = floor(data.y * 0.5);
    down  = down >= 32.0 ? down - 64.0 : down;
    right = right >= 32.0 ? right - 64.0 : right;
    vec4        a    = Clip(Lifted(position, on));
    vec4        b    = Clip(Lifted(other, on));
    const float side = mod(data.x, 4.0) - 1.0;
    line             = vec3(0.0, 1.0, 0.0);
    if (side != 0.0)
    {
        const vec4 a0 = a;
        const vec4 b0 = b;
        if (a0.z < 0.0) a = mix(a0, b0, clamp(a0.z / (a0.z - b0.z), 0.0, 1.0));
        if (b0.z < 0.0) b = mix(b0, a0, clamp(b0.z / (b0.z - a0.z), 0.0, 1.0));
        const vec2  halfScreen = 0.5 * pc.screen.xy;
        const float wa = max(a.w, 0.000001);
        const float wb = max(b.w, 0.000001);
        const vec2  sa = a.xy / wa * halfScreen;
        const vec2  sb = b.xy / wb * halfScreen;
        vec2        d  = sb - sa;
        const float len = length(d);
        d = len > 0.0001 ? d / len : vec2(1.0, 0.0);
        const float along     = 1.0 - 2.0 * mod(data.y, 2.0);
        const vec2  n         = vec2(-d.y, d.x) * along;
        const float halfWidth = data.z * pc.screen.z + 2.0 * soft * pc.screen.z;
        const vec2  offset    = (n * side - d * cap) * halfWidth;
        a.xy += offset / halfScreen * wa;
        line = vec3(side * halfWidth, halfWidth, soft * 4.0 * pc.screen.z);
    }
    // A shadow's move on the screen, in half pixels at 1080 lines (y down).
    a.xy += vec2(right, -down) * 8.0 * pc.screen.z / (0.5 * pc.screen.xy) * a.w;
    plane = other;
    color       = vec4(float((argb >> 16) & 255u), float((argb >> 8) & 255u), float(argb & 255u), float(argb >> 24)) / 255.0;
    gl_Position = vec4(a.x, -a.y, a.z, a.w);
}
