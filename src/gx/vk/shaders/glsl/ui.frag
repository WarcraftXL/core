#version 450
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The client's interface pixel program, "UI" (Shaders\Pixel\ps_3_0\UI.bls): the texel of s0 times
// the colour. Then the D3D9 alpha test, which runs after a pixel program as after fixed function.
// A ps_3_0 program does its own fog, and this one has none.

#extension GL_GOOGLE_include_directive : require
#include "engine.glsl"
#include "interface.frag.glsl"

void main()
{
    vec4 c = Sample0(StageCoord0().xy) * vColor;
    if (AlphaRejected(c.a)) discard;
    outColor = c;
}
