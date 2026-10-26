#version 460
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.highlight's outline and glow onto the scene, as the Direct3D 9 passes' CompositePs
// (gx/d3d9/shaders/highlight.hlsl), a FULLSCREEN program of wxl.render blended ONE,
// ONE_MINUS_SRC_ALPHA: the outline covers, the glow adds.

#extension GL_GOOGLE_include_directive : require
#include "wxl/render/render.glsl"

layout(push_constant) uniform Push
{
    vec4 screen;  ///< xy: one pixel in uv; z: the widest outline this frame, in pixels; w: 1 with a glow
    vec4 gain;    ///< x: how strong the glow is overall
    uint color;   ///< the colour mask, resolved: rgb times coverage, a coverage
    uint extra;   ///< the other mask, resolved: rgb the glow, a the width code, times coverage
    uint glow;    ///< the other mask blurred at half the size
    uint nearest; ///< samplers: nearest and linear, clamped
    uint linear;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

const float kWidthCode = 32.0; ///< the mask's width code is the width over this many pixels

vec4 Point(uint image, vec2 at) { return textureLod(sampler2D(uImages[nonuniformEXT(image)], uSamplers[pc.nearest]), at, 0.0); }

// One sample of the search: the covered pixel at `distance` along `direction`, kept when its own
// object's outline reaches this far and it reaches further than the best so far.
void Tap(vec2 direction, float distance, inout float best, inout vec3 color)
{
    const vec2  at       = uv + direction * distance * pc.screen.xy;
    const vec4  s        = Point(pc.color, at);
    const float coverage = max(s.a, 0.004);
    const float width    = Point(pc.extra, at).a / coverage * kWidthCode;
    const float cover    = clamp(width - distance + 1.0, 0.0, 1.0) * step(0.004, s.a);
    const float take     = step(best + 0.0001, cover);
    best  = mix(best, cover, take);
    color = mix(color, s.rgb / coverage, take);
}

void Ring(float distance, inout float best, inout vec3 color)
{
    Tap(vec2(1.0, 0.0), distance, best, color);
    Tap(vec2(0.8660254, 0.5), distance, best, color);
    Tap(vec2(0.5, 0.8660254), distance, best, color);
    Tap(vec2(0.0, 1.0), distance, best, color);
    Tap(vec2(-0.5, 0.8660254), distance, best, color);
    Tap(vec2(-0.8660254, 0.5), distance, best, color);
    Tap(vec2(-1.0, 0.0), distance, best, color);
    Tap(vec2(-0.8660254, -0.5), distance, best, color);
    Tap(vec2(-0.5, -0.8660254), distance, best, color);
    Tap(vec2(0.0, -1.0), distance, best, color);
    Tap(vec2(0.5, -0.8660254), distance, best, color);
    Tap(vec2(0.8660254, -0.5), distance, best, color);
}

void main()
{
    const float inside = Point(pc.color, uv).a;
    const vec4  near   = WxlSample(pc.glow, pc.linear, uv); // rgb the glow; a above 0 near a highlighted object

    // The outline: the nearest covered pixel within its own object's width, searched on three
    // rings out to the widest outline asked this frame, only near an object.
    float best  = 0.0;
    vec3  color = vec3(0.0);
    if (near.a > 0.0)
    {
        Ring(pc.screen.z / 3.0, best, color);
        Ring(pc.screen.z * 2.0 / 3.0, best, color);
        Ring(pc.screen.z, best, color);
    }
    const float outline = best * (1.0 - inside);

    // The glow: light added around the object, never over it.
    const vec3 light = near.rgb * pc.gain.x * pc.screen.w * (1.0 - inside);

    outColor = vec4(color * outline + light * (1.0 - outline), outline);
}
