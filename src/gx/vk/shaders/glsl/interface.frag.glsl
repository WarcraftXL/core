// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The pixel side of the interface (engine.glsl).

layout(location = 0) in vec4  vColor;
layout(location = 1) in vec4  vTex0;
layout(location = 2) in vec4  vTex1;
layout(location = 3) in float vFog;

layout(location = 0) out vec4 outColor;

vec4 Sample0(vec2 uv)
{
    return texture(sampler2D(uImages[pc.image0], uSamplers[pc.sampler0]), uv);
}

vec4 Sample1(vec2 uv)
{
    return texture(sampler2D(uImages[pc.image1], uSamplers[pc.sampler1]), uv);
}
