#version 460
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.highlight's mask program, a wxl.render BATCH program: a highlighted batch drawn again behind
// its own vertex program into the two masks, as the Direct3D 9 passes' MaskPs3 does
// (gx/d3d9/shaders/highlight.hlsl): its colour and coverage, its glow and outline width; a texel
// under the batch's alpha test is no part of the silhouette.
//
// Constants (draw_batch's, rows of vec4): 0 the colour (rgb, a 1), 1 the glow (rgb times strength)
// and the outline width over 32 pixels, 2 x the alpha test's reference (0 to 1, or -1 for none),
// y the sampler the batch's first texture is read with (its bits).

#extension GL_GOOGLE_include_directive : require
#include "wxl/render/batch.glsl"

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outExtra;

void main()
{
    const vec4 test = WxlConstant(2);
    if (test.x >= 0.0 && WxlStageSampleWith(0, floatBitsToUint(test.y), wxlTexcoord0.xy).a - test.x < 0.0) discard;
    outColor = WxlConstant(0);
    outExtra = WxlConstant(1);
}
