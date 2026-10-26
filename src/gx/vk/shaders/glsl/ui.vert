#version 450
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The client's interface vertex program, "UI" (Shaders\Vertex\vs_3_0\UI.bls), as its bytecode
// reads: four dp4 of the position against c0..c3 (the engine's view-projection, transposed), the
// colour and the first coordinate passed on. Permutation 1 (WXL_STEREO) is the stereo one: x and y
// multiplied by the dp4 with c3, which is also w. The engine's clip space is D3D's, +y up; Vulkan's
// is +y down, so y is negated here. z is left: its native projection maps depth to [0, 1] already.

#extension GL_GOOGLE_include_directive : require
#include "engine.glsl"
#include "interface.vert.glsl"

void main()
{
    DrawData d = DrawData(pc.data);
    vec4     p = vec4(inPosition.xyz, 1.0);
#ifdef WXL_STEREO
    vec3 r      = vec3(dot(d.constants[0], p), dot(d.constants[1], p), dot(d.constants[3], p));
    gl_Position = vec4(r.xy * r.z, dot(d.constants[2], p), r.z);
#else
    gl_Position = vec4(dot(d.constants[0], p), dot(d.constants[1], p), dot(d.constants[2], p), dot(d.constants[3], p));
#endif
    gl_Position.y = -gl_Position.y;
    vColor        = VertexColor(d);
    vTex0         = (d.inputs.w & kHasTex0) != 0 ? vec4(inTex0.xy, 0.0, 0.0) : vec4(0.0);
    vTex1         = vec4(0.0);
    vFog          = 1.0;
    // A point list draws points of one pixel, as D3D9 draws them without its point states.
    gl_PointSize = 1.0;
}
