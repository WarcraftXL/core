// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// What the renderer's own programs share with every program the engine's draws use (the client's
// translated ones included, shaders/Interface.hpp, whose numbers this file repeats): the bindless
// table, the 128 bytes of push constants and the varyings at fixed locations, so that any vertex
// program pairs with any pixel program, as D3D9 lets the engine pair them. Plus the draw's data
// block the renderer's own programs read.
//
// Vertex inputs sit at the location of the engine's attribute (EGxVertexAttrib): position 0,
// normal 3, colour 4, texture coordinates 0 and 1 at 6 and 7. An input the vertex does not carry
// reads zeros (a buffer of its own); draw.inputs.w says which it carries.
//
// The engine's matrices travel as it keeps them: rows, a point multiplied on their left
// (p' = p * M). Uploaded as they are, a GLSL mat4's column i is the engine's row i, so the product
// is written as a sum over the columns (Row() below) and never as M * p, which would transpose it.

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_nonuniform_qualifier : require

/// The bindless table: every texture's image at the slot its record holds, and the samplers.
layout(set = 0, binding = 0) uniform texture2D uImages[];
layout(set = 0, binding = 1) uniform sampler uSamplers[];

/// The draw's data, written into the frame's ring by the renderer (draw::DrawData, same layout).
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer DrawData
{
    mat4  worldView;    ///< the engine's world then view transforms
    mat4  projection;   ///< its native projection (D3D's depth range, [0, 1])
    mat4  texMatrix[2]; ///< stages 0 and 1: the transform D3D9's fixed function applies to their coordinates
    vec4  constants[4]; ///< the vertex program's registers c0..c3 (the interface's program)
    vec4  fog;          ///< x, y: f = z * x + y on the view depth; z: 1 when fog is on
    ivec4 ops;          ///< colour op 0, alpha op 0, colour op 1, alpha op 1 (EGxTexOp); -1: the stage is off
    ivec4 texgen;       ///< stage 0's coordinate source (EGxTexGen), its transform flags, stage 1's, its flags
    ivec4 inputs;       ///< x, y: the coordinate set stages 0 and 1 read; z: unused; w: the attributes carried
    vec4  lightPosition[4];    ///< camera space: a point light's position (w 1), or the direction towards a directional one (w 0)
    vec4  lightDiffuse[4];
    vec4  lightAmbient[4];
    vec4  lightSpecular[4];
    vec4  lightAttenuation[4]; ///< constant, linear, quadratic, range
    vec4  matDiffuse;
    vec4  matAmbient;
    vec4  matEmissive;
    vec4  matSpecular;         ///< w: the power
    vec4  globalAmbient;
    ivec4 lighting;            ///< x: the lights on (a bit each), y: lighting on, z: specular on, w: normals normalised
    ivec4 sources;             ///< diffuse, ambient, emissive, specular: 0 the material, 1 the vertex's colour
};

layout(push_constant) uniform Push
{
    uvec2 data;      ///< the address of the draw's DrawData
    uvec2 vertexRegs;
    uvec2 pixelRegs;
    float alphaRef;  ///< the alpha test's reference, 1..255; 0: no test
    uint  flags;     ///< kFlag* below
    vec4  clipPlane; ///< against D3D9's clip-space position
    uint  fogColor;  ///< 0xAARRGGBB
    float pointSize;
    vec2  pointRange; ///< D3D9's POINTSIZE_MIN and POINTSIZE_MAX
    uint  stages[16]; ///< a stage's texture slot (low 16 bits) and sampler (high 16)
} pc;

const uint kFlagFog          = 1u << 1;
const uint kFlagPointSprite  = 1u << 2;
const uint kFlagClip         = 1u << 3;
const uint kFlagSwizzleShift = 16u;

// The varyings' locations (shaders::Varying).
#define WXL_COLOR0    0
#define WXL_COLOR1    1
#define WXL_TEXCOORD0 2
#define WXL_FOG       10

// The attributes in DrawData.inputs.w, by EGxVertexAttrib.
const int kHasNormal = 1 << 3;
const int kHasColor  = 1 << 4;
const int kHasTex0   = 1 << 6;
const int kHasTex1   = 1 << 7;

/// p * M, the engine's way round.
vec4 Row(vec4 p, mat4 m)
{
    return p.x * m[0] + p.y * m[1] + p.z * m[2] + p.w * m[3];
}

/// The D3D9 alpha test with ALPHAFUNC GREATEREQUAL (the device's only function): the alpha as the
/// 8-bit value D3D9 compares, below the reference discards.
bool AlphaRejected(float alpha)
{
    return pc.alphaRef > 0.0 && floor(clamp(alpha, 0.0, 1.0) * 255.0 + 0.5) < pc.alphaRef;
}

/// D3D9's fog after the pixel stage, when the draw asks: the fog colour by the factor's complement.
vec4 Fogged(vec4 color, float factor)
{
    if ((pc.flags & kFlagFog) == 0u) return color;
    vec3 fog = vec3(float((pc.fogColor >> 16) & 0xFFu), float((pc.fogColor >> 8) & 0xFFu), float(pc.fogColor & 0xFFu)) / 255.0;
    return vec4(mix(fog, color.rgb, clamp(factor, 0.0, 1.0)), color.a);
}
