#version 450
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The client's "Desaturate" pixel program (Shaders\Pixel\ps_3_0\Desaturate.bls), the image mode
// of a desaturated texture: its bytecode takes the texel's luminance with the weights
// (0.299, 0.587, 0.114) as grey, and the texel's alpha times the colour's alpha; the colour's rgb
// is not read. Then the D3D9 alpha test.

#extension GL_GOOGLE_include_directive : require
#include "engine.glsl"
#include "interface.frag.glsl"

void main()
{
    vec4  t = Sample0(vTex0.xy);
    float l = dot(t.rgb, vec3(0.299, 0.587, 0.114));
    float a = t.a * vColor.a;
    if (AlphaRejected(a)) discard;
    outColor = vec4(l, l, l, a);
}
