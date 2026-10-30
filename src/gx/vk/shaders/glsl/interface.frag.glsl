// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The pixel side of the interface (engine.glsl): the varyings the renderer's own pixel programs read.

layout(location = 0) in vec4 vColor;
layout(location = 2) in vec4 vTex0;
layout(location = 3) in vec4 vTex1;
layout(location = 10) in vec4 vFog;

layout(location = 0) out vec4 outColor;

vec4 SampleStage(uint stage, vec2 uv)
{
    uint word = pc.stages[stage];
    return texture(sampler2D(uImages[word & 0xFFFFu], uSamplers[word >> 16]), uv);
}

/// Stage 0's and 1's coordinates: the point's own when points are drawn as sprites.
vec4 StageCoord0() { return (pc.flags & kFlagPointSprite) != 0u ? vec4(gl_PointCoord, 0.0, 1.0) : vTex0; }
vec4 StageCoord1() { return (pc.flags & kFlagPointSprite) != 0u ? vec4(gl_PointCoord, 0.0, 1.0) : vTex1; }

vec4 Sample0(vec2 uv) { return SampleStage(0u, uv); }
vec4 Sample1(vec2 uv) { return SampleStage(1u, uv); }
