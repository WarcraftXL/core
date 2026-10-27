// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The client's single-pass terrain pixel programs (ps_3_0 Terrain1, Terrain2, Terrain3 and their
// _pcf forms) rewritten to blend their layers by height, as retail does (wowdev ADT/v18, MTXP).
//
// Each of those programs reads layer k's texture on sampler k at texture coordinates v(k), the
// chunk's alpha maps on sampler L (L layers: red, green, blue the alphas of layers 1 to 3, alpha
// the shadow), then mixes the layers one over the other: lerp(lerp(lerp(t0, t1, a1), t2, a2), t3,
// a3). The rewrite leaves all of that, the lighting, the shadows and the fog as they are, and
// changes only the alphas, right after the alpha maps are read:
//
//   w        each layer's share of the client's own mix: w3 = a3, w2 = a2 (1 - a3), ...,
//            w0 = (1 - a1)(1 - a2)(1 - a3)
//   pct_k    w_k * (1 + e_k (h_k * scale_k + offset_k - 1)), h_k the alpha of layer k's _h texture
//            (sampler 12 + k, at v(k)), (scale_k, offset_k, e_k) the layer's numbers (pixel
//            constant c(32 + k): MTXP's heightScale, heightOffset, 1 when the layer has its _h)
//   cut      pct_k *= 1 - saturate(max(pct) - pct_k): a layer far below the highest drops out
//   share    pct / sum(pct)
//   alphas   the shares turned back into the alphas of the client's mix: a3' = pct3,
//            a2' = pct2 / (1 - pct3), a1' = pct1 / (1 - pct2 - pct3)
//
// A chunk whose layers all have e = 0 keeps its alphas bit for bit: the client's own blend. The
// same bytecode is made by Direct3D 9 and translated by the Vulkan renderer, so both draw it alike.
//
// The four-layer programs also weigh a chunk's layers 4 to 7 (wxl.terrain's extra layers): their
// alphas on sampler 9 (laid over the four as the client lays its own), layers 4 and 5's heights
// on 10 and 11 (6 and 7 have none: no sampler is left), every layer's coordinates carried from the
// alpha maps' (c41 + k), and the pass row c40 (x: the second draw, y: the chunk has extra layers).
// The chunk is drawn twice with this one program: the first draw writes the mix of layers 0 to 3,
// each shared out among the four; the second, with layers 4 to 7 on samplers 0 to 3, writes theirs
// and, as its alpha, their share of the whole, S2 / (S1 + S2). Lighting and fog are affine in the
// mix, so alpha-blending the second over the first gives the eight-layer mix exactly. Every
// write of the colour goes to a temporary, written out once at the end with that alpha.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wxl::modern_adt::shader
{
    /// The sampler of layer 0's height texture; layer k's is this + k (WXL_TERRAIN_LAYER_SAMPLER).
    constexpr uint32_t kHeightSampler = 12;
    /// The pixel constant of layer 0's numbers; layer k's is this + k (WXL_TERRAIN_LAYER_PARAM_ROW).
    constexpr uint32_t kParamRow = 32;

    /// The sampler of the alpha maps of layers 4 to 7 (WXL_TERRAIN_EXTRA_ALPHA_SAMPLER).
    constexpr uint32_t kExtraAlphaSampler = 9;
    /// The sampler of layer 4's height texture; layer 5's is the next (WXL_TERRAIN_EXTRA_SAMPLER).
    constexpr uint32_t kExtraHeightSampler = 10;
    /// The pass row: x 1 in the second draw, y 1 for a chunk with layers past its four.
    constexpr uint32_t kPassRow = 40;
    /// Layer k's coordinates from the alpha maps': u = au * x + z, v = av * y + w (c41 + k).
    constexpr uint32_t kCoordRow = 41;

    /// What the rewrite found in a program.
    struct Found
    {
        uint32_t layers = 0;       ///< L
        uint32_t heights = 0;      ///< layers read with a height (2d samplers read at an input's coordinates)
        bool     extras = false;   ///< a four-layer program, rewritten for layers 4 to 7 too
    };

    /// @p code rewritten into @p out; false (and @p why) for a program it does not take: another
    /// profile, fewer than two layers, a layout it does not know, or registers it needs in use.
    bool BlendByHeight(const void* code, size_t size, std::vector<uint32_t>& out, Found& found, std::string& why);
}
