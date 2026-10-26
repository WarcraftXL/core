// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The vertex side of the interface (engine.glsl): what a vertex program reads and writes.

layout(location = 0) in vec4 inPosition;
layout(location = 3) in vec4 inNormal;
layout(location = 4) in vec4 inColor;
layout(location = 6) in vec4 inTex0;
layout(location = 7) in vec4 inTex1;

layout(location = 0) out vec4  vColor; ///< the diffuse colour
layout(location = 1) out vec4  vTex0;  ///< stage 0's coordinate (for a program, its texcoord0)
layout(location = 2) out vec4  vTex1;  ///< stage 1's coordinate
layout(location = 3) out float vFog;   ///< 1: unfogged

/// The vertex's colour, white when it carries none, in RGBA whatever order the device read it in.
vec4 VertexColor(DrawData d)
{
    if ((d.inputs.w & kHasColor) == 0) return vec4(1.0);
    return (pc.flags & kFlagColorRgba) != 0u ? inColor.bgra : inColor;
}
