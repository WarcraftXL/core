#version 460
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.highlight's glow: nine taps of a Gaussian along one direction, as the Direct3D 9 passes'
// BlurPs (gx/d3d9/shaders/highlight.hlsl), a FULLSCREEN program of wxl.render.

#extension GL_GOOGLE_include_directive : require
#include "wxl/render/render.glsl"

layout(push_constant) uniform Push
{
    vec2 step;         ///< one tap's step in the source's coordinates
    uint image;        ///< the source
    uint samplerIndex; ///< linear, clamped
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

const float kWeights[5] = float[5](0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162);

void main()
{
    vec4 sum = WxlSample(pc.image, pc.samplerIndex, uv) * kWeights[0];
    for (int i = 1; i < 5; ++i)
    {
        const vec2 offset = pc.step * float(i);
        sum += WxlSample(pc.image, pc.samplerIndex, uv + offset) * kWeights[i];
        sum += WxlSample(pc.image, pc.samplerIndex, uv - offset) * kWeights[i];
    }
    outColor = sum;
}
