#version 450
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// D3D9's fixed-function vertex processing, as the engine drives it with no vertex program bound.
// Adapted from the legacy mod's ff.vert.
//
//   - The position goes through the engine's world-view then its native projection.
//   - The colour is the vertex's (white when it has none), or the draw's constant colour: the
//     engine's "ambient only" lighting, which the renderer works out on the CPU (inputs.z = 1).
//     The engine's vertex lights are not run at this step.
//   - Each stage's coordinate: the set its TEXCOORDINDEX names, or the camera-space position,
//     normal or reflection its texgen makes, then its texture transform with D3D's padding: the
//     input padded with 1 in its first missing component and 0 after ((u, v) -> (u, v, 1, 0)),
//     times the matrix, its first COUNT components kept. PROJECTED is divided per pixel (ff.frag).
//   - Linear fog on the view depth, D3D's vertex fog: f = z * fog.x + fog.y, clamped; 1 when off.

#extension GL_GOOGLE_include_directive : require
#include "engine.glsl"
#include "interface.vert.glsl"

vec3 CameraNormal(DrawData d)
{
    if ((d.inputs.w & kHasNormal) == 0) return vec3(0.0);
    return Row(vec4(inNormal.xyz, 0.0), d.worldView).xyz;
}

vec4 StageCoord(DrawData d, int stage, vec2 uv, vec3 e)
{
    int gen   = stage == 0 ? d.texgen.x : d.texgen.z;
    int flags = stage == 0 ? d.texgen.y : d.texgen.w;

    vec3 g;
    int  comps;
    if (gen == 0)
    {
        g     = vec3(uv, 0.0);
        comps = 2;
    }
    else if (gen == 5)
    {
        g     = CameraNormal(d);
        comps = 3;
    }
    else if (gen == 4 || gen == 6)
    {
        g     = reflect(normalize(e), CameraNormal(d));
        comps = 3;
    }
    else
    {
        g     = e;
        comps = 3;
    }

    int count = flags & 0xFF;
    if (count == 0) return vec4(g, 0.0);

    vec4 t = comps == 2 ? vec4(g.xy, 1.0, 0.0) : vec4(g, 1.0);
    t      = Row(t, d.texMatrix[stage]);
    if (count < 4) t.w = 0.0;
    if (count < 3) t.z = 0.0;
    return t;
}

void main()
{
    DrawData d    = DrawData(pc.data);
    vec4     e    = Row(vec4(inPosition.xyz, 1.0), d.worldView);
    vec4     clip = Row(e, d.projection);
    clip.y        = -clip.y;
    gl_Position   = clip;

    vColor = d.inputs.z == 1 ? d.material : VertexColor(d);

    vec2 set0 = (d.inputs.w & kHasTex0) != 0 ? inTex0.xy : vec2(0.0);
    vec2 set1 = (d.inputs.w & kHasTex1) != 0 ? inTex1.xy : set0;
    vTex0     = StageCoord(d, 0, d.inputs.x == 1 ? set1 : set0, e.xyz);
    vTex1     = StageCoord(d, 1, d.inputs.y == 1 ? set1 : set0, e.xyz);

    vFog = d.fog.z > 0.5 ? clamp(e.z * d.fog.x + d.fog.y, 0.0, 1.0) : 1.0;
    // A point list draws points of one pixel, as D3D9 draws them without its point states.
    gl_PointSize = 1.0;
}
