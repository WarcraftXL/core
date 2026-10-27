// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.terrain's layers (see TerrainLayers.hpp).

#include "services/TerrainLayers.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "services/StorageService.hpp"
#include "services/TextureService.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
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
    /// The single-pass pixel programs' extra samplers and numbers (wxl_terrain_layer_look, abi 0.1.43).
    constexpr uint32_t kExtraStage = WXL_TERRAIN_LAYER_SAMPLER;
    constexpr uint32_t kParamRow   = WXL_TERRAIN_LAYER_PARAM_ROW;
    /// Layers past the client's four (abi 0.1.44).
    constexpr uint32_t kExtraAlphaStage = WXL_TERRAIN_EXTRA_ALPHA_SAMPLER;
    constexpr uint32_t kExtraHeightStage = WXL_TERRAIN_EXTRA_SAMPLER;
    constexpr uint32_t kPassRow = WXL_TERRAIN_LAYER_PASS_ROW;
    constexpr uint32_t kCoordRow = WXL_TERRAIN_LAYER_COORD_ROW;
    /// c32 to c48: the eight layers' numbers, the pass row, the eight coordinate rows.
    constexpr uint32_t kRows = kCoordRow + 8 - kParamRow;
    constexpr uint32_t kRsBlending = 6, kRsDepthWrite = 15; ///< EGxRenderState
    constexpr int32_t  kBlendAlpha = 2;                     ///< GxBlend_Alpha

    struct Provider
    {
        uint32_t             mod;
        wxl_terrain_layer_fn fn;
        void*                user;
        uint32_t             flags;
        wxl_terrain_chunk_fn chunk_fn;
    };
    std::vector<Provider> g_providers;
    /// Some provider wants the layers' numbers written at every single-pass draw.
    bool g_everyDraw = false;

    /// A render chunk's layers past the client's four, as its providers answered them.
    struct Extras
    {
        uint32_t    count = 0;
        void*       texture[4] = {};  ///< their textures (texture_open handles), owned by g_textures
        void*       height[2]  = {};  ///< layers 4 and 5's extra textures
        float       scale[4]   = {1.0f, 1.0f, 1.0f, 1.0f};
        float       params[4][4] = {};
        void*       alpha = nullptr;  ///< their alpha maps, served by the texture service
        std::string alphaPath;
    };

    /// A render chunk's layers' looks; only chunks with one other than the client's are kept.
    struct Scales
    {
        float scale[4]     = {1.0f, 1.0f, 1.0f, 1.0f};
        void* extra[4]     = {};  ///< the extra textures (texture_open handles), owned by g_textures
        float params[4][4] = {};
        std::shared_ptr<Extras> extras; ///< layers 4 to 7, when some provider answered any
    };
    std::unordered_map<const void*, Scales> g_chunks;

    /// The extra textures opened, by path, until the map goes.
    std::unordered_map<std::string, void*> g_textures;

    /// The chunk being drawn, while one of the eight layer functions runs.
    struct Context
    {
        bool        active = false;
        Path        path = Path::vertex_single;
        const void* chunk = nullptr;
        const void* batch = nullptr;
        uint32_t    layers = 0;
        bool        specular = false; ///< layer 0 is a reflection map: its stage has no layer matrix
        bool        params = false;   ///< the single-pass draw writes the layers' numbers and extra textures
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
        uint32_t samplers[8] = {};
        void*    bound[8]    = {};
        uint32_t sampler_count = 0;
        bool     params = false;
        bool     second = false; ///< the chunk's layers past its four are drawn after this draw
    } g_restore;

    /// The second draw of a chunk with extra layers is under way: the device's draw seams leave it.
    bool g_secondDraw = false;
    /// The map's WDT, until the map goes.
    std::vector<uint8_t> g_wdt;
    /// Alpha maps served for extra layers, until the map goes.
    std::vector<std::pair<std::string, void*>> g_alphas;

    services::terrain_layers::Counters g_counters;
    bool                              g_saidRange = false;
    bool                              g_saidCut   = false;

    bool Scaled(const Scales& s)
    {
        for (int k = 0; k < 4; ++k)
        {
            if (s.scale[k] != 1.0f || s.extra[k] || s.extras) return true;
            for (float p : s.params[k])
                if (p != 0.0f) return true;
        }
        return false;
    }

    bool Single(Path path) { return path == Path::vertex_single || path == Path::fixed_single; }

    void* OpenExtra(const char* path)
    {
        if (!path || !*path) return nullptr;
        const auto at = g_textures.find(path);
        if (at != g_textures.end()) return at->second;
        void* texture = binding::texture_open(path);
        g_textures.emplace(path, texture);
        return texture;
    }

    void CloseExtras()
    {
        for (auto& t : g_textures)
            if (t.second) binding::texture_close(t.second);
        g_textures.clear();
        for (auto& a : g_alphas)
        {
            if (a.second) binding::texture_close(a.second);
            services::texture::Unserve(a.first);
        }
        g_alphas.clear();
    }

    /// One map chunk's answer from the providers with a chunk_fn; false when none answered layers.
    bool AskChunk(const void* mapChunk, const wxl_core_area_file& file, const wxl_core_render_chunk& rc, wxl_terrain_extra_layers& out)
    {
        wxl_terrain_chunk info{};
        info.struct_size = sizeof info;
        info.tile_x      = file.tile_x;
        info.tile_y      = file.tile_y;
        info.chunk_x     = -1;
        info.chunk_y     = -1;
        binding::terrain_chunk_place(mapChunk, &info.chunk_x, &info.chunk_y);
        info.layers = rc.layers;
        for (uint32_t i = 0; i < 4; ++i) info.texture[i] = rc.layer_texture[i];
        info.file          = file.data;
        info.file_size     = file.size;
        info.map_file      = g_wdt.empty() ? nullptr : g_wdt.data();
        info.map_file_size = static_cast<uint32_t>(g_wdt.size());
        std::memset(&out, 0, sizeof out);
        out.struct_size = sizeof out;
        for (const Provider& p : std::vector<Provider>(g_providers))
            if (p.chunk_fn) p.chunk_fn(p.user, &info, &out);
        if (out.count > WXL_TERRAIN_EXTRA_LAYERS) out.count = WXL_TERRAIN_EXTRA_LAYERS;
        for (uint32_t k = 0; k < out.count; ++k)
            if (!out.layer[k].texture || !*out.layer[k].texture) return false; // a layer without its texture: none drawn
        return out.count > 0;
    }

    bool SameLayers(const wxl_terrain_extra_layers& a, const wxl_terrain_extra_layers& b)
    {
        if (a.count != b.count) return false;
        for (uint32_t k = 0; k < a.count; ++k)
            if (std::strcmp(a.layer[k].texture, b.layer[k].texture) != 0) return false;
        return true;
    }

    /// The layers past a render chunk's four, from its one or two map chunks; null for none.
    std::shared_ptr<Extras> AskExtras(const void* area, const wxl_core_render_chunk& rc)
    {
        if (rc.layers != 4 || !rc.map_chunk || (rc.render_flags & 0x4)) return nullptr;
        bool any = false;
        for (const Provider& p : g_providers) any = any || p.chunk_fn;
        if (!any) return nullptr;
        wxl_core_area_file file{};
        binding::terrain_area_file(area, &file);
        static wxl_terrain_extra_layers first, second; // 16 KiB each: kept off the stack
        if (!AskChunk(rc.map_chunk, file, rc, first)) return nullptr;
        // Drawn with its neighbour: both answer the same layers, their alpha maps side by side.
        const bool pair = rc.map_chunk2 && rc.pair;
        if (pair && (!AskChunk(rc.map_chunk2, file, rc, second) || !SameLayers(first, second)))
        {
            ++g_counters.chunks_unpaired;
            return nullptr;
        }
        constexpr uint32_t n = WXL_TERRAIN_ALPHA_SIZE;
        const uint32_t     w = pair && rc.pair == 2 ? 2 * n : n, h = pair && rc.pair == 1 ? 2 * n : n;
        std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4u, 0);
        for (uint32_t y = 0; y < n; ++y)
        {
            std::memcpy(&rgba[static_cast<size_t>(y) * w * 4u], &first.alpha[static_cast<size_t>(y) * n * 4u], n * 4u);
            if (pair && rc.pair == 2) std::memcpy(&rgba[(static_cast<size_t>(y) * w + n) * 4u], &second.alpha[static_cast<size_t>(y) * n * 4u], n * 4u);
            if (pair && rc.pair == 1) std::memcpy(&rgba[static_cast<size_t>(y + n) * w * 4u], &second.alpha[static_cast<size_t>(y) * n * 4u], n * 4u);
        }
        auto extras   = std::make_shared<Extras>();
        extras->count = first.count;
        for (uint32_t k = 0; k < first.count; ++k)
        {
            const wxl_terrain_extra_layer& l = first.layer[k];
            extras->texture[k] = OpenExtra(l.texture);
            if (k < 2) extras->height[k] = OpenExtra(l.extra_texture);
            const float scale  = l.texture_scale;
            extras->scale[k]   = std::isfinite(scale) && scale >= kLeast && scale <= kMost ? scale : 1.0f;
            for (int i = 0; i < 4; ++i) extras->params[k][i] = std::isfinite(l.params[i]) && (k < 2 || i != 2) ? l.params[i] : 0.0f;
            if (!extras->texture[k]) return nullptr;
        }
        extras->alphaPath = services::texture::ServeData("terrain", rgba.data(), w, h);
        if (extras->alphaPath.empty()) return nullptr;
        extras->alpha = binding::texture_open(extras->alphaPath.c_str());
        g_alphas.push_back({extras->alphaPath, extras->alpha});
        if (!extras->alpha) return nullptr;
        ++g_counters.chunks_extra;
        return extras;
    }

    void EveryDraw()
    {
        g_everyDraw = false;
        for (const Provider& p : g_providers) g_everyDraw = g_everyDraw || (p.flags & WXL_TERRAIN_LAYER_PARAMS_EVERY_DRAW);
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
        // A provider of abi 0.1.42 ends before its flags.
        constexpr uint32_t kFirst = offsetof(wxl_terrain_layer_provider, flags);
        if (provider && (provider->struct_size < kFirst || !provider->fn)) return WXL_E_INVALID;
        // abi 0.1.43 ends before chunk_fn.
        const uint32_t flags = provider && provider->struct_size >= offsetof(wxl_terrain_layer_provider, chunk_fn) ? provider->flags : 0u;
        const wxl_terrain_chunk_fn chunkFn =
            provider && provider->struct_size >= sizeof(wxl_terrain_layer_provider) ? provider->chunk_fn : nullptr;
        for (Provider& p : g_providers)
            if (p.mod == mod)
            {
                if (provider)
                {
                    p.fn    = provider->fn;
                    p.user  = provider->user;
                    p.flags    = flags;
                    p.chunk_fn = chunkFn;
                    EveryDraw();
                }
                else
                    ForgetMod(mod);
                return WXL_OK;
            }
        if (provider) g_providers.push_back(Provider{mod, provider->fn, provider->user, flags, chunkFn});
        EveryDraw();
        return WXL_OK;
    }

    void ForgetMod(uint32_t mod)
    {
        for (size_t i = g_providers.size(); i-- > 0;)
            if (g_providers[i].mod == mod) g_providers.erase(g_providers.begin() + static_cast<std::ptrdiff_t>(i));
        EveryDraw();
    }

    void MapUnloading()
    {
        g_chunks.clear();
        g_ctx = Context{};
        CloseExtras();
    }

    void Clear()
    {
        g_providers.clear();
        g_everyDraw = false;
        g_chunks.clear();
        CloseExtras();
        g_wdt.clear();
        g_ctx      = Context{};
        g_restore  = Restore{};
        g_counters = Counters{};
    }

    uint32_t WdtLoaded()
    {
        g_wdt.clear();
        wxl_core_wdt wdt{};
        if (binding::map_wdt(&wdt) != WXL_TRUE || !wdt.flags) return 0;
        if (wdt.path && *wdt.path)
        {
            const wxl_storage_api& storage = StorageApi();
            const wxl_str          path{wdt.path, static_cast<uint32_t>(std::strlen(wdt.path))};
            uint64_t               size = 0, read = 0;
            if (storage.size(path, &size) == WXL_OK && size && size < (16u << 20))
            {
                g_wdt.resize(static_cast<size_t>(size));
                if (storage.read(path, g_wdt.data(), size, &read) != WXL_OK || read != size) g_wdt.clear();
            }
        }
        // Retail reads uncompressed alpha maps of 4096 bytes under 0x4 or 0x80; this client under 0x4 alone.
        if ((*wdt.flags & 0x80u) && !(*wdt.flags & 0x4u))
        {
            *wdt.flags |= 0x4u;
            binding::map_validate_shaders();
            ++g_counters.maps_widened;
            WLOG_INFO(kTag, "%s is flagged for height texturing (MPHD 0x80) without big alpha (0x4): read with 0x4, as retail reads it",
                      wdt.path ? wdt.path : "the map");
        }
        return *wdt.flags;
    }

    const std::vector<uint8_t>& Wdt() { return g_wdt; }

    uint32_t TileLoading(void* data, uint32_t size)
    {
        // The top-level chunks: id and size, then the data. MCNK's header holds nLayers at 0x0C.
        constexpr uint32_t kMcnk = 0x4D434E4Bu; // "MCNK", read as the file stores it ("KNCM")
        auto*              bytes = static_cast<uint8_t*>(data);
        uint32_t           cut   = 0, layers = 0;
        for (size_t at = 0; bytes && at + 8 <= size;)
        {
            uint32_t id = 0, length = 0;
            std::memcpy(&id, bytes + at, 4);
            std::memcpy(&length, bytes + at + 4, 4);
            if (length > size - at - 8) break;
            if (id == kMcnk && length >= 0x10)
            {
                std::memcpy(&layers, bytes + at + 8 + 0x0C, 4);
                if (layers > 4)
                {
                    const uint32_t four = 4;
                    std::memcpy(bytes + at + 8 + 0x0C, &four, 4);
                    ++cut;
                }
            }
            at += 8 + static_cast<size_t>(length);
        }
        if (cut)
        {
            g_counters.chunks_cut += cut;
            if (!g_saidCut)
                WLOG_INFO(kTag, "a tile names more than 4 texture layers in %u chunk(s): the client draws their first 4 (it holds no more), the rest stay in the file for a mod",
                          cut);
            g_saidCut = true;
        }
        return cut;
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
            info.map_file      = g_wdt.empty() ? nullptr : g_wdt.data();
            info.map_file_size = static_cast<uint32_t>(g_wdt.size());
            wxl_terrain_layer_look look{};
            look.struct_size   = sizeof look;
            look.texture_scale = 1.0f;
            for (const Provider& p : std::vector<Provider>(g_providers)) p.fn(p.user, &info, &look);
            ++g_counters.layers_asked;
            scale = look.texture_scale;
            scales.extra[layer] = OpenExtra(look.extra_texture);
            for (int i = 0; i < 4; ++i) scales.params[layer][i] = std::isfinite(look.params[i]) ? look.params[i] : 0.0f;
            if (scales.extra[layer]) ++g_counters.layers_extra;
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
        // The chunk's fourth layer made: its layers past the four, asked once a making.
        if (rc.layers == 4 && layer == 3 && !scales.extras) scales.extras = AskExtras(area, rc);
        for (uint32_t i = rc.layers; i < 4; ++i)
        {
            scales.scale[i] = 1.0f;
            scales.extra[i] = nullptr;
            std::memset(scales.params[i], 0, sizeof scales.params[i]);
        }
        if (Scaled(scales)) g_chunks[chunk] = scales;
        else if (kept != g_chunks.end()) g_chunks.erase(kept);
    }

    void Enter(Path path, const void* chunk)
    {
        g_ctx.active = false;
        // A chunk of the client's own looks is entered only to have its numbers written as zeros
        // (WXL_TERRAIN_LAYER_PARAMS_EVERY_DRAW) on the single-pass paths.
        const bool every = g_everyDraw && Single(path);
        if (g_chunks.empty() && !every) return;
        const auto at = g_chunks.find(chunk);
        if (at == g_chunks.end() && !every) return;
        wxl_core_render_chunk rc{};
        if (binding::terrain_render_chunk(chunk, &rc) != WXL_TRUE) return;
        g_ctx.active   = true;
        g_ctx.path     = path;
        g_ctx.chunk    = chunk;
        g_ctx.batch    = rc.batch;
        g_ctx.layers   = rc.layers;
        g_ctx.specular = (rc.render_flags & 0x4) != 0;
        g_ctx.draws    = 0;
        g_ctx.scales   = at != g_chunks.end() ? at->second : Scales{};
        g_ctx.params   = every || at != g_chunks.end();
        if (at != g_chunks.end()) ++g_counters.chunks_drawn;
    }

    void Leave(const void* chunk)
    {
        if (g_ctx.chunk == chunk) g_ctx = Context{};
    }

    void BeforeDraw(const void* batch)
    {
        if (!g_ctx.active || batch != g_ctx.batch || g_secondDraw) return;
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
        if (draw || !g_ctx.params || !Single(g_ctx.path)) return;
        // The single-pass pixel programs' extras: layer k's extra texture on sampler 12 + k (layers 4
        // and 5 on 10 and 11), wrapped as the layer's own; its numbers in c(32 + k), zeros for a
        // layer whose texture is not loaded yet, so a program never reads numbers without their
        // texture; the pass row; each layer's coordinates as a function of the alpha maps'.
        float rows[kRows][4] = {};
        const auto bind = [](uint32_t stage, void* texture, bool wrap) {
            void* gx = texture ? binding::gx_texture_ready(texture) : nullptr;
            if (!gx) return false;
            g_restore.samplers[g_restore.sampler_count] = stage;
            g_restore.bound[g_restore.sampler_count++] = binding::gx_texture_stage(stage);
            if (wrap) binding::gx_texture_wrap(gx);
            binding::gx_texture_stage_set(stage, gx);
            return true;
        };
        for (uint32_t k = 0; k < g_ctx.layers && k < 4; ++k)
            if (!s.extra[k] || bind(kExtraStage + k, s.extra[k], true)) std::memcpy(rows[k], s.params[k], sizeof rows[k]);
        // Layers past the four: on the vertex-program path, once their textures and alpha maps are loaded.
        const Extras* e = g_ctx.path == Path::vertex_single ? s.extras.get() : nullptr;
        bool          ready = e && binding::gx_texture_ready(e->alpha);
        for (uint32_t k = 0; ready && k < e->count; ++k) ready = binding::gx_texture_ready(e->texture[k]) != nullptr;
        if (ready)
        {
            bind(kExtraAlphaStage, e->alpha, false);
            for (uint32_t k = 0; k < e->count; ++k)
                if (k >= 2 || !e->height[k] || bind(kExtraHeightStage + k, e->height[k], true)) std::memcpy(rows[4 + k], e->params[k], sizeof rows[4 + k]);
            rows[kPassRow - kParamRow][1] = 1.0f;
            g_restore.second = true;
        }
        // tc_k = (p - c23).yx * c(18 + k).yx + c(13 + k).yx for every layer and for the alpha maps
        // (c(18 + L), c(13 + L)): layer k's coordinates are the alpha maps' times the ratio of their
        // scales, plus what is left of the offsets.
        if (const float* vc = g_ctx.path == Path::vertex_single ? binding::terrain_vertex_constants() : nullptr)
        {
            const float* sa = vc + (kScaleRow + g_ctx.layers) * 4;
            const float* oa = vc + (kOffsetRow + g_ctx.layers) * 4;
            for (uint32_t k = 0; k < 8; ++k)
            {
                float sk[2] = {}, ok[2] = {};
                if (k < g_ctx.layers)
                    for (int c = 0; c < 2; ++c)
                    {
                        sk[c] = vc[(kScaleRow + k) * 4 + c] / s.scale[k];
                        ok[c] = vc[(kOffsetRow + k) * 4 + c] / s.scale[k];
                    }
                else if (e && k - 4 < e->count && k >= 4)
                    for (int c = 0; c < 2; ++c) sk[c] = vc[kScaleRow * 4 + c] / e->scale[k - 4];
                float* row = rows[kCoordRow - kParamRow + k];
                for (int c = 0; c < 2; ++c)
                {
                    const int   swz = 1 - c; // u from the y components, v from the x ones
                    const float m   = sa[swz] != 0.0f ? sk[swz] / sa[swz] : 0.0f;
                    row[c]          = m;
                    row[2 + c]      = ok[swz] - oa[swz] * m;
                }
            }
        }
        binding::gx_pixel_constants_set(kParamRow, &rows[0][0], kRows);
        g_restore.params = true;
        if (g_restore.sampler_count) ++g_counters.draws_extra;
    }

    namespace
    {
        /// The second draw of a chunk with layers past its four: layers 4 to 7 on stages 0 to 3, at
        /// texture coordinates 0 to 3, alpha-blended over the first draw, depth not written.
        void SecondDraw(const void* batch)
        {
            const Extras& e  = *g_ctx.scales.extras;
            const float*  vc = binding::terrain_vertex_constants();
            if (!vc) return;
            g_secondDraw = true;
            void* before[4];
            for (uint32_t k = 0; k < 4; ++k) before[k] = binding::gx_texture_stage(k);
            for (uint32_t k = 0; k < e.count; ++k)
            {
                void* gx = binding::gx_texture_ready(e.texture[k]);
                binding::gx_texture_wrap(gx);
                binding::gx_texture_stage_set(k, gx);
            }
            float scales[4][4], offsets[4][4] = {};
            std::memcpy(scales, vc + kScaleRow * 4, sizeof scales);
            for (uint32_t k = 0; k < e.count; ++k)
                for (int c = 0; c < 2; ++c) scales[k][c] = vc[kScaleRow * 4 + c] / e.scale[k];
            binding::gx_vertex_constants_set(kScaleRow, &scales[0][0], 4);
            binding::gx_vertex_constants_set(kOffsetRow, &offsets[0][0], 4);
            const float second[4] = {1.0f, 1.0f, 0.0f, 0.0f};
            binding::gx_pixel_constants_set(kPassRow, second, 1);
            const int32_t blending = binding::gx_render_state(kRsBlending), depthWrite = binding::gx_render_state(kRsDepthWrite);
            binding::gx_render_state_set(kRsBlending, kBlendAlpha);
            binding::gx_render_state_set(kRsDepthWrite, 0);

            binding::gx_draw(batch, 1);

            binding::gx_render_state_set(kRsBlending, blending);
            binding::gx_render_state_set(kRsDepthWrite, depthWrite);
            const float first[4] = {0.0f, 1.0f, 0.0f, 0.0f};
            binding::gx_pixel_constants_set(kPassRow, first, 1);
            binding::gx_vertex_constants_set(kScaleRow, vc + kScaleRow * 4, 4);
            binding::gx_vertex_constants_set(kOffsetRow, vc + kOffsetRow * 4, 4);
            for (uint32_t k = 0; k < 4; ++k) binding::gx_texture_stage_set(k, before[k]);
            g_secondDraw = false;
            ++g_counters.second_draws;
        }
    }

    void AfterDraw(const void* batch)
    {
        if (!g_ctx.active || batch != g_ctx.batch || g_secondDraw) return;
        if (g_restore.second) SecondDraw(batch);
        if (g_restore.row_count)
        {
            // The client's own rows, which it keeps as it sent them.
            const float* rows = binding::terrain_vertex_constants();
            for (uint32_t i = 0; rows && i < g_restore.row_count; ++i)
                binding::gx_vertex_constants_set(g_restore.rows[i], rows + g_restore.rows[i] * 4, 1);
        }
        for (uint32_t i = 0; i < g_restore.stage_count; ++i) binding::gx_xform_set(g_restore.stages[i], g_restore.matrices[i]);
        for (uint32_t i = g_restore.sampler_count; i-- > 0;) binding::gx_texture_stage_set(g_restore.samplers[i], g_restore.bound[i]);
        if (g_restore.params)
        {
            const float zeros[kRows][4] = {};
            binding::gx_pixel_constants_set(kParamRow, &zeros[0][0], kRows);
        }
        g_restore = Restore{};
    }

    Counters Count()
    {
        Counters c   = g_counters;
        c.chunks_kept = static_cast<uint32_t>(g_chunks.size());
        return c;
    }
}
