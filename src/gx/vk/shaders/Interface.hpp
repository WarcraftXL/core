// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// What every program the renderer draws the engine's work with agrees on: the client's translated
// programs (Translate.hpp), the renderer's own GLSL ones (glsl/engine.glsl, which repeats these
// numbers) and the renderer that records the draws (draw/). One interface for all, fixed by
// meaning, so that any vertex program pairs with any pixel program, as D3D9 lets the engine pair
// them:
//
//   vertex inputs   at the location of the engine's attribute (EGxVertexAttrib): position 0, blend
//                   weight 1, blend indices 2, normal 3, colours 4 and 5, texture coordinates 0..7
//                   at 6..13; an input the vertex does not carry reads zeros
//   varyings        at fixed locations by meaning (Varying below), each a vec4; every vertex
//                   program writes all of them, (0, 0, 0, 1) where it has nothing to say (the fog
//                   1, unfogged), so a pixel program reads any it wants
//   push constants  128 bytes, both stages (Push below)
//   descriptors     set 0: binding 0 the sampled images (2D and cube views of the same array, by
//                   the texture's slot), binding 1 the samplers

#pragma once

#include <cstdint>

namespace wxl::vk::shaders
{
    /// The engine's vertex attributes (EGxVertexAttrib), the vertex inputs' locations.
    constexpr uint32_t kVertexInputs = 14;

    /// D3DDECLUSAGE (and its index) as the engine's attribute it feeds: -1 for one the engine has none of.
    inline int InputLocation(uint32_t usage, uint32_t index)
    {
        switch (usage)
        {
        case 0: return index == 0 ? 0 : -1;  // position
        case 1: return index == 0 ? 1 : -1;  // blend weight
        case 2: return index == 0 ? 2 : -1;  // blend indices
        case 3: return index == 0 ? 3 : -1;  // normal
        case 10: return index < 2 ? static_cast<int>(4 + index) : -1; // colour 0, 1
        case 5: return index < 8 ? static_cast<int>(6 + index) : -1;  // texture coordinate 0..7
        default: return -1;
        }
    }

    /// The varyings' locations.
    enum Varying : uint32_t
    {
        kColor0    = 0,
        kColor1    = 1,
        kTexcoord0 = 2, ///< .. kTexcoord0 + 7
        kFog       = 10,
        kNormal    = 11,
        kTangent   = 12,
        kBinormal  = 13,
        kVaryings  = 14,
    };

    /// D3DDECLUSAGE (and its index) of a vertex program's output or a pixel program's input as the
    /// varying that carries it: -1 for one the interface has none of.
    inline int VaryingLocation(uint32_t usage, uint32_t index)
    {
        switch (usage)
        {
        case 10: return index < 2 ? static_cast<int>(kColor0 + index) : -1;      // colour
        case 5: return index < 8 ? static_cast<int>(kTexcoord0 + index) : -1;     // texture coordinate
        case 11: return index == 0 ? static_cast<int>(kFog) : -1;                 // fog
        case 3: return index == 0 ? static_cast<int>(kNormal) : -1;               // normal
        case 6: return index == 0 ? static_cast<int>(kTangent) : -1;              // tangent
        case 7: return index == 0 ? static_cast<int>(kBinormal) : -1;             // binormal
        default: return -1;
        }
    }

    /// The texture stages a program samples (D3D9's s0..s15).
    constexpr uint32_t kStages = 16;
    /// The rows of a register file (c0..c255).
    constexpr uint32_t kRegisterRows = 256;

    /**
     * The push constants of every draw. A stage's word is its texture's bindless slot (low 16 bits)
     * and its sampler's (high 16 bits). The register files are the frame's copies of the engine's
     * vertex and pixel registers, as many rows as the bound programs read; the draw's data is the
     * block the renderer's own programs read (draw::DrawData).
     */
    struct Push
    {
        uint32_t data[2];     ///< the address of the draw's DrawData
        uint32_t vertex[2];   ///< the address of the vertex registers (vec4 rows)
        uint32_t pixel[2];    ///< the address of the pixel registers
        float    alpha_ref;   ///< the alpha test's reference, 1..255; 0: no test
        uint32_t flags;       ///< kFlag* below
        float    clip_plane[4]; ///< the user clip plane, against D3D9's clip-space position
        uint32_t fog_color;   ///< 0xAARRGGBB: D3D9's fog colour, for the fog applied after a pixel program below ps_3_0
        float    point_size;  ///< D3D9's point size, where a vertex program writes none
        float    point_range[2]; ///< D3D9's POINTSIZE_MIN and POINTSIZE_MAX, every point's size clamped to them
        uint32_t stages[kStages];
    };
    static_assert(sizeof(Push) == 128, "the guaranteed push constant size");

    /// Push::flags.
    enum : uint32_t
    {
        kFlagFog         = 1u << 1, ///< D3D9's fog after the pixel program (below ps_3_0, and fixed function)
        kFlagPointSprite = 1u << 2, ///< points drawn as sprites: every texture coordinate is the point's
        kFlagClip        = 1u << 3, ///< the user clip plane is on
        /// From this bit, one per vertex input location: a D3DCOLOR the device reads in RGBA order
        /// (no B8G8R8A8 vertex format), its program swaps it back.
        kFlagSwizzleShift = 16,
    };

    constexpr uint32_t StageWord(uint32_t image_slot, uint32_t sampler) { return (image_slot & 0xFFFFu) | (sampler << 16); }
}
