#version 460
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw's particles on the Vulkan renderer, as ParticlePs (gx/d3d9/shaders/draw.hlsl) line for
// line: a picture of the texture or a soft round blot, faded by the corner's height above the
// ground. Its numbers (rows 2.w textured, 5.y MODULATE, then the picture's bindless image and
// sampler) are read from the frame's memory by address, as draw_surface.frag's.

#extension GL_GOOGLE_include_directive : require
#include "wxl/render/render.glsl"

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SurfaceRows
{
    vec4  rows[6];
    uvec4 picture;
};

layout(push_constant) uniform Push
{
    vec4  viewColumns[4];
    vec4  screen;
    vec4  lift;
    uvec2 vertices;
    uvec2 surface;
} pc;

layout(location = 0) in vec4 color;
layout(location = 2) in vec3 plane;
layout(location = 0) out vec4 outColor;

void main()
{
    SurfaceRows s  = SurfaceRows(pc.surface);
    const vec2  uv = plane.xy;
    const float blur = max(s.rows[5].w, 1.0);
    const vec2  gx = dFdx(uv) * blur;
    const vec2  gy = dFdy(uv) * blur;
    vec4        texel;
    if (s.rows[2].w > 0.5)
    {
        texel = textureGrad(sampler2D(uImages[nonuniformEXT(s.picture.x)], uSamplers[nonuniformEXT(s.picture.y)]), uv, gx, gy);
        if (s.rows[5].z > 0.5) texel.rgb = vec3(1.0);
    }
    else
    {
        const vec2  d = uv * 2.0 - 1.0;
        const float a = clamp(1.0 - dot(d, d), 0.0, 1.0);
        texel = vec4(1.0, 1.0, 1.0, a * a);
    }
    const float a = color.a * texel.a * clamp(plane.z, 0.0, 1.0);
    const vec3  c = color.rgb * texel.rgb;
    outColor = s.rows[5].y > 0.5 ? vec4(1.0 + (c - 1.0) * a, a) : vec4(c, a);
}
