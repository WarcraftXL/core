#version 450
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// D3D9's fixed-function vertex processing, as the engine drives it with no vertex program bound.
// Adapted from the legacy mod's ff.vert.
//
//   - The position goes through the engine's world-view then its native projection; with
//     WXL_CLIP, the user clip plane's distance (the renderer gives the plane in clip space).
//   - The colour is the vertex's (white when it has none), or D3D9's lighting in camera space with
//     LOCALVIEWER on: up to four lights (directional or point, with attenuation and range), the
//     material's diffuse, ambient, emissive and specular, each from the material or the vertex's
//     colour (D3D9's material sources); the global ambient (the engine's ambient-only mode is
//     lighting with a white ambient and no light). The specular goes to the second colour.
//   - Each stage's coordinate: the set its TEXCOORDINDEX names, or the camera-space position,
//     normal or reflection its texgen makes, then its texture transform with D3D's padding: the
//     input padded with 1 in its first missing component and 0 after ((u, v) -> (u, v, 1, 0)),
//     times the matrix, its first COUNT components kept. PROJECTED is divided per pixel (ff.frag).
//   - Linear fog on the view depth, D3D's vertex fog: f = z * fog.x + fog.y, clamped; 1 when off.

#extension GL_GOOGLE_include_directive : require
#include "engine.glsl"
#include "interface.vert.glsl"

#ifdef WXL_CLIP
out float gl_ClipDistance[1];
#endif

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

/// D3D9's lighting of the vertex at @p e (camera space): the diffuse colour, and the specular in @p specular.
vec4 Lit(DrawData d, vec3 e, vec4 vertexColor, out vec4 specular)
{
    vec4 diffuseMat  = d.sources.x == 1 ? vertexColor : d.matDiffuse;
    vec4 ambientMat  = d.sources.y == 1 ? vertexColor : d.matAmbient;
    vec4 emissiveMat = d.sources.z == 1 ? vertexColor : d.matEmissive;
    vec4 specularMat = d.sources.w == 1 ? vertexColor : vec4(d.matSpecular.rgb, 0.0);

    vec3 n = CameraNormal(d);
    if (d.lighting.w != 0 && dot(n, n) > 0.0) n = normalize(n);
    vec3 toEye = -normalize(e);

    vec3 ambient = d.globalAmbient.rgb;
    vec3 diffuse = vec3(0.0);
    vec3 shine   = vec3(0.0);
    for (int i = 0; i < 4; ++i)
    {
        if ((d.lighting.x & (1 << i)) == 0) continue;
        vec4  p = d.lightPosition[i];
        vec3  l;
        float atten = 1.0;
        if (p.w == 0.0) l = p.xyz;
        else
        {
            vec3  toLight = p.xyz - e;
            float dist    = length(toLight);
            vec4  a       = d.lightAttenuation[i];
            atten         = dist > a.w ? 0.0 : 1.0 / max(a.x + a.y * dist + a.z * dist * dist, 1e-6);
            l             = dist > 0.0 ? toLight / dist : vec3(0.0, 0.0, 1.0);
        }
        ambient += d.lightAmbient[i].rgb * atten;
        float nl = max(dot(n, l), 0.0);
        diffuse += d.lightDiffuse[i].rgb * nl * atten;
        if (d.lighting.z != 0)
        {
            vec3 h = normalize(l + toEye);
            shine += d.lightSpecular[i].rgb * pow(max(dot(n, h), 0.0), d.matSpecular.w) * atten;
        }
    }
    specular = vec4(clamp(specularMat.rgb * shine, 0.0, 1.0), 0.0);
    vec3 rgb = emissiveMat.rgb + ambientMat.rgb * ambient + diffuseMat.rgb * diffuse;
    return vec4(clamp(rgb, 0.0, 1.0), diffuseMat.a);
}

void main()
{
    DrawData d    = DrawData(pc.data);
    vec4     e    = Row(vec4(inPosition.xyz, 1.0), d.worldView);
    vec4     clip = Row(e, d.projection);
#ifdef WXL_CLIP
    gl_ClipDistance[0] = (pc.flags & kFlagClip) != 0u ? dot(pc.clipPlane, clip) : 1.0;
#endif
    gl_Position = vec4(clip.x, -clip.y, clip.z, clip.w);
    DefaultVaryings();

    vec4 color = VertexColor(d);
    if (d.lighting.y != 0) vColor = Lit(d, e.xyz, color, vSpecular);
    else vColor = color;

    vec2 set0 = (d.inputs.w & kHasTex0) != 0 ? inTex0.xy : vec2(0.0);
    vec2 set1 = (d.inputs.w & kHasTex1) != 0 ? inTex1.xy : set0;
    vTex0     = StageCoord(d, 0, d.inputs.x == 1 ? set1 : set0, e.xyz);
    vTex1     = StageCoord(d, 1, d.inputs.y == 1 ? set1 : set0, e.xyz);

    vFog = vec4(d.fog.z > 0.5 ? clamp(e.z * d.fog.x + d.fog.y, 0.0, 1.0) : 1.0, 0.0, 0.0, 0.0);
    gl_PointSize = clamp(pc.pointSize, pc.pointRange.x, pc.pointRange.y);
}
