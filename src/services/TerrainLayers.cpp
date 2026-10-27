// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.terrain's layers (see TerrainLayers.hpp).

#include "services/TerrainLayers.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace wxl::core;
    using Path = services::terrain_layers::Path;

    constexpr const char* kTag = "terrain";

    /// The vertex program's rows (CMapRenderChunk::SVertexShaderConstants): c13 + k a layer's
    /// animation offset, c18 + k its texture scale (gx/terrain Terrain.bls: texcoord k =
    /// (position - c23).yx * c(18 + k).yx + c(13 + k).yx, in every profile).
    constexpr uint32_t kOffsetRow = 13;
    constexpr uint32_t kScaleRow  = 18;
    constexpr float    kLeast = 1.0f / 64.0f, kMost = 1024.0f;

    struct Provider
    {
        uint32_t             mod;
        wxl_terrain_layer_fn fn;
        void*                user;
    };
    std::vector<Provider> g_providers;

    /// A render chunk's layers' texture scales; only chunks with one other than 1 are kept.
    struct Scales
    {
        float scale[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    };
    std::unordered_map<const void*, Scales> g_chunks;

    /// The chunk being drawn, while one of the eight layer functions runs.
    struct Context
    {
        bool        active = false;
        Path        path = Path::vertex_single;
        const void* chunk = nullptr;
        const void* batch = nullptr;
        uint32_t    layers = 0;
        bool        specular = false; ///< layer 0 is a reflection map: its stage has no layer matrix
        uint32_t    draws = 0;
        Scales      scales;
    } g_ctx;

    /// What BeforeDraw changed, given back by AfterDraw.
    struct Restore
    {
        uint32_t rows[8]  = {};
        uint32_t row_count = 0;
        uint32_t stages[4] = {};
        float    matrices[4][16] = {};
        uint32_t stage_count = 0;
    } g_restore;

    services::terrain_layers::Counters g_counters;
    bool                              g_saidRange = false;

    bool Scaled(const Scales& s)
    {
        for (float f : s.scale)
            if (f != 1.0f) return true;
        return false;
    }

    /// Row @p row of the client's constants, its x and y divided by @p scale, sent through the
    /// engine; noted for AfterDraw.
    void ScaleRow(const float* rows, uint32_t row, float scale)
    {
        float r[4];
        std::memcpy(r, rows + row * 4, sizeof r);
        r[0] /= scale;
        r[1] /= scale;
        binding::gx_vertex_constants_set(row, r, 1);
        if (g_restore.row_count < 8) g_restore.rows[g_restore.row_count++] = row;
    }

    /// Texture stage @p stage's matrix with its u and v columns divided by @p scale: the texture
    /// coordinates it makes, its translation (an animated layer's move) included.
    void ScaleStage(uint32_t stage, float scale)
    {
        float m[16];
        if (binding::gx_xform_get(stage, m) != WXL_TRUE || g_restore.stage_count >= 4) return;
        std::memcpy(g_restore.matrices[g_restore.stage_count], m, sizeof m);
        g_restore.stages[g_restore.stage_count++] = stage;
        for (int r = 0; r < 4; ++r)
        {
            m[4 * r + 0] /= scale;
            m[4 * r + 1] /= scale;
        }
        binding::gx_xform_set(stage, m);
    }
}

namespace wxl::core::services::terrain_layers
{
    wxl_result SetProvider(uint32_t mod, const wxl_terrain_layer_provider* provider)
    {
        if (provider && (provider->struct_size < sizeof(wxl_terrain_layer_provider) || !provider->fn)) return WXL_E_INVALID;
        for (Provider& p : g_providers)
            if (p.mod == mod)
            {
                if (provider)
                {
                    p.fn   = provider->fn;
                    p.user = provider->user;
                }
                else
                    ForgetMod(mod);
                return WXL_OK;
            }
        if (provider) g_providers.push_back(Provider{mod, provider->fn, provider->user});
        return WXL_OK;
    }

    void ForgetMod(uint32_t mod)
    {
        for (size_t i = g_providers.size(); i-- > 0;)
            if (g_providers[i].mod == mod) g_providers.erase(g_providers.begin() + static_cast<std::ptrdiff_t>(i));
    }

    void MapUnloading()
    {
        g_chunks.clear();
        g_ctx = Context{};
    }

    void Clear()
    {
        g_providers.clear();
        g_chunks.clear();
        g_ctx      = Context{};
        g_restore  = Restore{};
        g_counters = Counters{};
    }

    void LayerCreated(void* chunk, void* area, const void* mcly)
    {
        if (g_providers.empty() && g_chunks.empty()) return; // nothing to ask, nothing kept
        wxl_core_render_chunk rc{};
        if (!mcly || binding::terrain_render_chunk(chunk, &rc) != WXL_TRUE || !rc.layers) return;
        // The layer CreateLayer appended is the last; one it refused as a duplicate is the one it
        // found (asked again: the same answer).
        uint32_t       texture = 0, flags = 0;
        std::memcpy(&texture, mcly, sizeof texture);                                        // SMLayer::m_textureId
        std::memcpy(&flags, static_cast<const char*>(mcly) + sizeof texture, sizeof flags); // SMLayer::m_flags
        uint32_t layer = rc.layers - 1;
        if (rc.layer_texture[layer] != texture)
            for (uint32_t i = 0; i < rc.layers; ++i)
                if (rc.layer_texture[i] == texture) layer = i;

        Scales scales;
        const auto kept = g_chunks.find(chunk);
        if (kept != g_chunks.end() && rc.layers > 1) scales = kept->second; // a chunk's layers are made from the first
        float scale = 1.0f;
        if (!g_providers.empty())
        {
            wxl_core_area_file file{};
            binding::terrain_area_file(area, &file);
            wxl_terrain_layer info{};
            info.struct_size = sizeof info;
            info.tile_x      = file.tile_x;
            info.tile_y      = file.tile_y;
            info.chunk_x     = -1;
            info.chunk_y     = -1;
            if (rc.map_chunk) binding::terrain_chunk_place(rc.map_chunk, &info.chunk_x, &info.chunk_y);
            info.layer       = layer;
            info.texture     = texture;
            info.flags       = flags;
            info.file        = file.data;
            info.file_size   = file.size;
            wxl_terrain_layer_look look{};
            look.struct_size   = sizeof look;
            look.texture_scale = 1.0f;
            for (const Provider& p : std::vector<Provider>(g_providers)) p.fn(p.user, &info, &look);
            ++g_counters.layers_asked;
            scale = look.texture_scale;
            if (!(std::isfinite(scale) && scale >= kLeast && scale <= kMost))
            {
                if (!g_saidRange)
                    WLOG_WARN(kTag, "a layer provider answered a texture scale of %g, past 1/64 to 1024: the layer is drawn as the client draws it",
                              static_cast<double>(scale));
                g_saidRange = true;
                scale       = 1.0f;
            }
            if (scale != 1.0f) ++g_counters.layers_scaled;
        }
        scales.scale[layer] = scale;
        for (uint32_t i = rc.layers; i < 4; ++i) scales.scale[i] = 1.0f;
        if (Scaled(scales)) g_chunks[chunk] = scales;
        else if (kept != g_chunks.end()) g_chunks.erase(kept);
    }

    void Enter(Path path, const void* chunk)
    {
        g_ctx.active = false;
        if (g_chunks.empty()) return;
        const auto at = g_chunks.find(chunk);
        if (at == g_chunks.end()) return;
        wxl_core_render_chunk rc{};
        if (binding::terrain_render_chunk(chunk, &rc) != WXL_TRUE) return;
        g_ctx.active   = true;
        g_ctx.path     = path;
        g_ctx.chunk    = chunk;
        g_ctx.batch    = rc.batch;
        g_ctx.layers   = rc.layers;
        g_ctx.specular = (rc.render_flags & 0x4) != 0;
        g_ctx.draws    = 0;
        g_ctx.scales   = at->second;
        ++g_counters.chunks_drawn;
    }

    void Leave(const void* chunk)
    {
        if (g_ctx.chunk == chunk) g_ctx = Context{};
    }

    void BeforeDraw(const void* batch)
    {
        if (!g_ctx.active || batch != g_ctx.batch) return;
        const uint32_t draw = g_ctx.draws++;
        g_restore           = Restore{};
        const Scales&  s    = g_ctx.scales;
        switch (g_ctx.path)
        {
        case Path::vertex_single:
        {
            const float* rows = binding::terrain_vertex_constants();
            if (draw || !rows) return;
            for (uint32_t k = 0; k < g_ctx.layers; ++k)
                if (s.scale[k] != 1.0f)
                {
                    ScaleRow(rows, kScaleRow + k, s.scale[k]);
                    ScaleRow(rows, kOffsetRow + k, s.scale[k]);
                }
            break;
        }
        case Path::vertex_multi:
        {
            // A layer a draw, each read through c18 and c13; past the layers, the shadow's pass.
            const float* rows = binding::terrain_vertex_constants();
            if (draw >= g_ctx.layers || !rows || s.scale[draw] == 1.0f) return;
            ScaleRow(rows, kScaleRow, s.scale[draw]);
            ScaleRow(rows, kOffsetRow, s.scale[draw]);
            break;
        }
        case Path::fixed_single:
            if (draw) return;
            for (uint32_t k = 0; k < g_ctx.layers; ++k)
                if (s.scale[k] != 1.0f && !(k == 0 && g_ctx.specular)) ScaleStage(k, s.scale[k]);
            break;
        case Path::fixed_multi:
            if (draw >= g_ctx.layers || s.scale[draw] == 1.0f || (draw == 0 && g_ctx.specular)) return;
            ScaleStage(0, s.scale[draw]);
            break;
        }
        if (g_restore.row_count || g_restore.stage_count) ++g_counters.draws_scaled;
    }

    void AfterDraw(const void* batch)
    {
        if (!g_ctx.active || batch != g_ctx.batch) return;
        if (g_restore.row_count)
        {
            // The client's own rows, which it keeps as it sent them.
            const float* rows = binding::terrain_vertex_constants();
            for (uint32_t i = 0; rows && i < g_restore.row_count; ++i)
                binding::gx_vertex_constants_set(g_restore.rows[i], rows + g_restore.rows[i] * 4, 1);
        }
        for (uint32_t i = 0; i < g_restore.stage_count; ++i) binding::gx_xform_set(g_restore.stages[i], g_restore.matrices[i]);
        g_restore = Restore{};
    }

    Counters Count()
    {
        Counters c   = g_counters;
        c.chunks_kept = static_cast<uint32_t>(g_chunks.size());
        return c;
    }
}
