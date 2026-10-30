// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The vertex side of the interface (engine.glsl): what a vertex program reads and writes. Every
// varying is written, (0, 0, 0, 1) where the program has nothing to say, the fog 1 (unfogged).

layout(location = 0) in vec4 inPosition;
layout(location = 3) in vec4 inNormal;
layout(location = 4) in vec4 inColor;
layout(location = 6) in vec4 inTex0;
layout(location = 7) in vec4 inTex1;

layout(location = 0) out vec4 vColor;    ///< the diffuse colour
layout(location = 1) out vec4 vSpecular; ///< the specular colour
layout(location = 2) out vec4 vTex0;     ///< stage 0's coordinate (texture coordinate 0)
layout(location = 3) out vec4 vTex1;     ///< stage 1's coordinate
layout(location = 4) out vec4 vTex2;
layout(location = 5) out vec4 vTex3;
layout(location = 6) out vec4 vTex4;
layout(location = 7) out vec4 vTex5;
layout(location = 8) out vec4 vTex6;
layout(location = 9) out vec4 vTex7;
layout(location = 10) out vec4 vFog;     ///< x: 1 unfogged
layout(location = 11) out vec4 vNormal;
layout(location = 12) out vec4 vTangent;
layout(location = 13) out vec4 vBinormal;

/// The varyings nobody writes.
void DefaultVaryings()
{
    vSpecular = vec4(0.0);
    vTex2 = vTex3 = vTex4 = vTex5 = vTex6 = vTex7 = vec4(0.0, 0.0, 0.0, 1.0);
    vNormal = vTangent = vBinormal = vec4(0.0, 0.0, 0.0, 1.0);
}

/// The vertex's colour, white when it carries none, in RGBA whatever order the device read it in.
vec4 VertexColor(DrawData d)
{
    if ((d.inputs.w & kHasColor) == 0) return vec4(1.0);
    return (pc.flags & (1u << (kFlagSwizzleShift + 4u))) != 0u ? inColor.bgra : inColor;
}
