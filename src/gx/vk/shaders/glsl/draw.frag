#version 460
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw's fragment program on the Vulkan renderer: the vertex's colour, as DrawPs
// (gx/d3d9/shaders/draw.hlsl).

layout(location = 0) in vec4 color;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = color;
}
