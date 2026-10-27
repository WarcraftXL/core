#version 460
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw's surfaces on the Vulkan renderer: a ground shape's inside with its picture, its feather
// towards the border and its fade from the centre, as SurfacePs (gx/d3d9/shaders/draw.hlsl) line
// for line. Its numbers (gx/shapes/Geometry.hpp, SurfaceConstants: six rows, then the picture's
// bindless image and sampler) are read from the frame's memory by address.

#extension GL_GOOGLE_include_directive : require
#include "wxl/render/render.glsl"

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SurfaceRows
{
    vec4  rows[6];
    uvec4 picture; ///< the image's slot, the sampler's
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

float Smooth(float t)
{
    return t * t * (3.0 - 2.0 * t);
}

void main()
{
    SurfaceRows s = SurfaceRows(pc.surface);
    const float x = plane.x;
    const float y = plane.y;
    const float r = sqrt(x * x + y * y);
    float       d;
    if (s.rows[3].x < 0.5)
    {
        d = s.rows[3].y - r;
        if (s.rows[3].z > 0.0) d = min(d, r - s.rows[3].z);
        if (s.rows[4].x < 3.14159265)
        {
            const float diff = s.rows[4].x - atan(abs(y), x);
            d = min(d, diff < 1.57079633 ? r * sin(diff) : r);
        }
    }
    else if (s.rows[3].x < 1.5)
        d = min(s.rows[4].y - abs(x), s.rows[4].z - abs(y));
    else
        d = plane.z;
    const float edge = s.rows[3].w > 0.0 ? Smooth(clamp(d / s.rows[3].w, 0.0, 1.0)) : 1.0;
    float       fade = 1.0;
    if (s.rows[4].w < 1.5) fade = 1.0 - Smooth(clamp((r * s.rows[5].x - s.rows[4].w) / max(1.0 - s.rows[4].w, 0.0001), 0.0, 1.0));
    vec4 texel = vec4(1.0);
    // The derivatives first, where every pixel of the quad runs.
    const vec2 uv    = vec2(dot(vec3(x, y, 1.0), s.rows[0].xyz), dot(vec3(x, y, 1.0), s.rows[1].xyz));
    const vec2 scale = s.rows[2].xy;
    const float blur = max(s.rows[5].w, 1.0);
    const vec2  gx   = dFdx(uv * scale) * blur;
    const vec2  gy   = dFdy(uv * scale) * blur;
    if (s.rows[2].w > 0.5)
    {
        const vec2 at = s.rows[2].z > 0.5 ? fract(uv) * scale : clamp(uv, 0.0, 1.0) * scale;
        texel = textureGrad(sampler2D(uImages[nonuniformEXT(s.picture.x)], uSamplers[nonuniformEXT(s.picture.y)]), at, gx, gy);
        if (s.rows[5].z > 0.5) texel.rgb = vec3(1.0);
    }
    const float a = color.a * texel.a * edge * fade;
    const vec3  c = color.rgb * texel.rgb;
    outColor = s.rows[5].y > 0.5 ? vec4(1.0 + (c - 1.0) * a, a) : vec4(c, a);
}
