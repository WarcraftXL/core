// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.terrain's layers: each texture layer the client makes is shown to the mods' layer providers
// (set_layer_provider), and drawn at the texture scale they answer on every path the client draws
// terrain with. A layer answered 1 (or no provider) is drawn exactly as the client draws it.
//
//   LayerCreated  CMapRenderChunk::CreateLayer has appended a layer (seam MapRenderChunk.CreateLayer
//                 After): the providers are asked, the answer kept per render chunk and layer
//   Enter/Leave   one of the eight functions that draw a chunk's layers runs (seams
//                 MapRenderChunk.Render*): which path, for which chunk
//   BeforeDraw    the device is about to draw (seams Gx.D3dDraw, Gx.D3d9ExDraw; the Vulkan device's
//   AfterDraw     draw_begin and draw_end): for a draw of the chunk being drawn, its layers' texture
//                 coordinates are scaled through the client's own state -- the vertex program's
//                 constants c18 + k (scale) and c13 + k (animation offset), or the texture stages'
//                 matrices -- before the device reads it, and given back after; the same under
//                 Direct3D 9 and the Vulkan renderer, which both read that state at the draw.
//                 On the single-pass paths, the layers' numbers go in pixel rows c32 + k and their
//                 extra textures on samplers 12 + k (abi 0.1.43), given back the same way.
//                 A chunk with layers past its four (abi 0.1.44, a provider's chunk_fn) is drawn a
//                 second time on the vertex-program path, after the first, with layers 4 to 7 on
//                 stages 0 to 3, alpha-blended, the same pixel program telling the two apart by
//                 the pass row c40; its alpha maps on sampler 9, layers 4 and 5's extra textures on
//                 10 and 11, every layer's coordinates in c41 + k.
//
// Main thread only, as the client draws.

#pragma once

#include "wxl/services/terrain.h"

#include <cstdint>
#include <vector>

namespace wxl::core::services::terrain_layers
{
    /// The calling mod's provider; null takes it off.
    wxl_result SetProvider(uint32_t mod, const wxl_terrain_layer_provider* provider);
    void       ForgetMod(uint32_t mod);
    /// The map's WDT was read (seam Map.LoadWdt After): kept for the providers until the map goes;
    /// MPHD 0x80 without 0x4 is given 0x4. Answers the flags as the client now holds them.
    uint32_t WdtLoaded();
    /// The WDT kept (empty before a map loads).
    const std::vector<uint8_t>& Wdt();

    /// The map goes: what was kept per render chunk is forgotten.
    void MapUnloading();
    /// The client shuts down.
    void Clear();

    /// The client holds at most 4 texture layers a chunk (CMapRenderChunk::m_layers[4], one alpha
    /// map texture of three alphas and the shadow, programs for 1 to 4 layers) and writes past them
    /// for an MCNK naming more. A tile's bytes just read (seam MapArea.AsyncCallback Before): each
    /// MCNK naming more than 4 is set to its first 4, the rest left in the file for a mod. Answers
    /// how many chunks were cut.
    uint32_t TileLoading(void* data, uint32_t size);

    /// A layer was appended to @p chunk (CMapRenderChunk*) by CreateLayer(@p area, @p mcly).
    void LayerCreated(void* chunk, void* area, const void* mcly);

    enum class Path : uint8_t
    {
        vertex_single, ///< RenderSinglePassVertexPixelShader: every layer in one draw, c18 + k and c13 + k
        vertex_multi,  ///< RenderMultiPassVertexPixelShader(BigAlpha): a layer a draw, c18 and c13
        fixed_single,  ///< RenderSinglePassPixelShader: every layer in one draw, stage k's matrix
        fixed_multi,   ///< RenderMultiPass{Additive,Alpha,PixelShader,PixelShaderBigAlpha}: a layer a draw, stage 0's matrix
    };
    void Enter(Path path, const void* chunk);
    void Leave(const void* chunk);

    void BeforeDraw(const void* batch);
    void AfterDraw(const void* batch);

    /// What it did, for the tests and the session's lines.
    struct Counters
    {
        uint64_t layers_asked = 0;  ///< layers shown to providers
        uint64_t layers_scaled = 0; ///< of those, answered other than 1
        uint64_t chunks_drawn = 0;  ///< chunks drawn with a scaled layer
        uint64_t draws_scaled = 0;  ///< draws whose texture coordinates were scaled
        uint64_t layers_extra = 0;  ///< layers answered an extra texture
        uint64_t draws_extra = 0;   ///< single-pass draws that bound an extra texture
        uint32_t chunks_kept = 0;   ///< render chunks holding a scaled layer now
        uint64_t chunks_cut = 0;    ///< MCNKs that named more than 4 layers, cut to 4
        uint64_t chunks_extra = 0;  ///< render chunks answered layers past their four
        uint64_t chunks_half = 0;   ///< chunks drawn with a neighbour where only one half has layers past four
        uint64_t chunks_trimmed = 0; ///< chunks drawn with a neighbour whose halves carry more than 4 extra layers together: the 4 covering most drawn
        uint64_t second_draws = 0;  ///< second draws of a chunk's layers past its four
        uint64_t maps_widened = 0;  ///< maps flagged 0x80 without 0x4, given 0x4
    };
    Counters Count();
}
