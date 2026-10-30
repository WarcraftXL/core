#version 450
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.render's full-screen triangle: three vertices cover the pass, `uv` (0,0) at its top-left
// corner and (1,1) at its bottom-right, so that at a pixel's centre it is (x + 0.5) / width.

layout(location = 0) out vec2 uv;

void main()
{
    const vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    uv                = corner;
    gl_Position       = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
