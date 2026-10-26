// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// What every program the renderer draws the engine's work with shares (src/gx/vk/draw/): the
// bindless table, the push constants and the draw's data block, and the interface between the
// stages. One interface for all, fixed by meaning, so that any vertex program pairs with any
// pixel program, as D3D9 lets the engine pair them.
//
// Vertex inputs sit at the location of the engine's attribute (EGxVertexAttrib): position 0,
// normal 3, colour 4, texture coordinates 0 and 1 at 6 and 7. An input the vertex does not carry
// reads zeros (a buffer of its own); draw.inputs says which it carries.
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
    vec4  material;     ///< the colour a vertex without one takes (colour mode 1), or white
    vec4  fog;          ///< x, y: f = z * x + y on the view depth; z: 1 when fog is on
    vec4  fogColor;
    ivec4 ops;          ///< colour op 0, alpha op 0, colour op 1, alpha op 1 (EGxTexOp); -1: the stage is off
    ivec4 texgen;       ///< stage 0's coordinate source (EGxTexGen), its transform flags, stage 1's, its flags
    ivec4 inputs;       ///< x, y: the coordinate set stages 0 and 1 read; z: colour mode; w: the attributes carried
};

layout(push_constant) uniform Push
{
    uvec2 data;     ///< the address of the draw's DrawData
    uint  image0;   ///< bindless slots of the textures of stages 0 and 1
    uint  image1;
    uint  sampler0; ///< and of their samplers
    uint  sampler1;
    float alphaRef; ///< the alpha test's reference, 1..255; 0: no test
    uint  flags;    ///< kFlag* below
} pc;

/// The vertex colour arrives in RGBA byte order (the device reads no B8G8R8A8 vertex attribute).
const uint kFlagColorRgba = 1u;

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
