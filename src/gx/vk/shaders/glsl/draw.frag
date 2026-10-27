#version 460
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw's fragment program on the Vulkan renderer: the vertex's colour, a line's edges faded
// over its softness, as DrawPs (gx/d3d9/shaders/draw.hlsl).

layout(location = 0) in vec4 color;
layout(location = 1) in vec3 line;
layout(location = 0) out vec4 outColor;

void main()
{
    const float cover = line.z > 0.0 ? clamp((line.y - abs(line.x)) / line.z, 0.0, 1.0) : 1.0;
    outColor = vec4(color.rgb, color.a * cover);
}
