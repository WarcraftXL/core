// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.terrain's tiles split by file name (see TerrainTiles.hpp).

#include "services/TerrainTiles.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Core.hpp"
#include "services/Assets.hpp"
#include "services/ModCall.hpp"
#include "services/ModelGate.hpp"
#include "services/SplitTile.hpp"
#include "services/TerrainLayers.hpp"

#include "wxl/host/mod_host.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace wxl::core::services::terrain_tiles
{
    namespace
    {
        constexpr const char* kTag = "terrain";

        struct Provider
        {
            uint32_t            mod = 0;
            wxl_terrain_tile_fn fn = nullptr;
            void*               user = nullptr;
        };

        /// A chunk's references the client can follow, when some of the file's could not be.
        struct ChunkRefs
        {
            bool                  allDoodads = true, allMapObjs = true;
            std::vector<uint32_t> doodads, mapObjs;
        };

        /// What the runtime keeps for a split tile until the client frees it.
        struct SplitArea
        {
            void*                     area = nullptr;
            uint8_t*                  root = nullptr; ///< the client's buffer (m_fileBuffer)
            uint32_t                  rootSize = 0;
            std::vector<uint8_t>      files[WXL_TERRAIN_TILE_FILES];
            split_tile::Tile          tile;
            std::vector<uint8_t>      chunkInfo;      ///< 256 SMChunkInfo: offset, size, flags, padding
            std::vector<int32_t>      textureFlags;   ///< MTXF as the client reads it, when MTXF is absent or short
            std::array<ChunkRefs, 256> refs;
        };

        std::vector<Provider>                   g_providers;
        std::vector<std::unique_ptr<SplitArea>> g_areas;
        Counters                                g_counters;
        bool                                    g_inRefs = false;
        bool                                    g_toldUnprovided = false;

        void Put32(uint8_t* at, uint32_t value) { std::memcpy(at, &value, sizeof value); }
        uint32_t Get32(const uint8_t* at)
        {
            uint32_t value;
            std::memcpy(&value, at, sizeof value);
            return value;
        }

        SplitArea* Of(const void* area)
        {
            for (const auto& a : g_areas)
                if (a->area == area) return a.get();
            return nullptr;
        }

        /// The split tile whose root holds @p at (a chunk's MCNK).
        SplitArea* Holding(const void* at)
        {
            const uint8_t* p = static_cast<const uint8_t*>(at);
            for (const auto& a : g_areas)
                if (p >= a->root && p < a->root + a->rootSize) return a.get();
            return nullptr;
        }

        void Drop(const void* area)
        {
            for (size_t i = 0; i < g_areas.size(); ++i)
                if (g_areas[i]->area == area)
                {
                    g_areas.erase(g_areas.begin() + static_cast<std::ptrdiff_t>(i));
                    return;
                }
        }
    }

    wxl_result SetProvider(uint32_t mod, const wxl_terrain_tile_provider* provider)
    {
        if (provider && (provider->struct_size < sizeof(wxl_terrain_tile_provider) || !provider->fn)) return WXL_E_INVALID;
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

    void AreaReading(void* area)
    {
        Drop(area);
        int32_t     x = 0, y = 0;
        void*       file = nullptr;
        uint32_t    size = 0;
        const void* data = area ? wxl::core::binding::map_area_bytes(area, &x, &y, &file, &size) : nullptr;
        if (!split_tile::IsSplitRoot(static_cast<const uint8_t*>(data), size)) return;

        auto split      = std::make_unique<SplitArea>();
        split->area     = area;
        split->root     = static_cast<uint8_t*>(const_cast<void*>(data));
        split->rootSize = size;

        // The name the tile was opened by, without the runtime's own prefix.
        char        name[260] = {};
        const char* path      = name;
        if (file && wxl::core::binding::file_name(file, name, sizeof name))
        {
            const char* inner = nullptr;
            if (assets::IsLayerPath(name, nullptr, &inner)) path = inner;
        }
        const std::vector<uint8_t>& wdt = terrain_layers::Wdt();
        wxl_terrain_tile tile{};
        tile.struct_size   = sizeof tile;
        tile.tile_x        = x;
        tile.tile_y        = y;
        tile.path          = wxl_str{path, static_cast<uint32_t>(std::strlen(path))};
        tile.file          = data;
        tile.file_size     = size;
        tile.map_file      = wdt.empty() ? nullptr : wdt.data();
        tile.map_file_size = static_cast<uint32_t>(wdt.size());
        bool answered = false;
        for (const Provider& p : std::vector<Provider>(g_providers))
        {
            wxl_terrain_tile_files files{};
            files.struct_size = sizeof files;
            CallMod(p.mod, kTag, "a tile provider", [&] { p.fn(p.user, &tile, &files); });
            for (uint32_t k = 0; k < WXL_TERRAIN_TILE_FILES; ++k)
                if (split->files[k].empty() && files.data[k] && files.size[k])
                {
                    const uint8_t* bytes = static_cast<const uint8_t*>(files.data[k]);
                    split->files[k].assign(bytes, bytes + files.size[k]);
                    answered = true;
                }
        }
        if (!answered)
        {
            ++g_counters.unprovided;
            if (!g_toldUnprovided)
                WLOG_WARN(kTag, "%s has no MCIN (a tile split into _tex0 and _obj0, as Cataclysm and later write it) and no tile "
                                "provider gave its files: it loads untextured and without placements (wxl.modern-adt provides them)",
                          path);
            g_toldUnprovided = true;
        }

        std::vector<uint8_t>& tex0 = split->files[WXL_TERRAIN_TILE_TEX0];
        std::vector<uint8_t>& obj0 = split->files[WXL_TERRAIN_TILE_OBJ0];
        std::string           why;
        if (!split_tile::Index(split->root, size, tex0.empty() ? nullptr : tex0.data(), static_cast<uint32_t>(tex0.size()),
                               obj0.empty() ? nullptr : obj0.data(), static_cast<uint32_t>(obj0.size()), split->tile, why))
        {
            WLOG_ERROR(kTag, "%s has no MCIN and cannot be read as a split tile (%s): the client reads it as it is", path, why.c_str());
            return;
        }
        if (!why.empty()) WLOG_WARN(kTag, "%s: %sthat file is not used", path, why.c_str());
        g_areas.push_back(std::move(split));
    }

    void AreaRead(void* area)
    {
        SplitArea* split = Of(area);
        if (!split) return;
        split_tile::Tile& t = split->tile;

        split->chunkInfo.assign(256 * 16, 0);
        for (uint32_t i = 0; i < 256; ++i)
        {
            Put32(&split->chunkInfo[16 * i], t.chunkOffset[i]);
            Put32(&split->chunkInfo[16 * i + 4], t.chunkSize[i]);
        }

        // The client reads one int32 of MTXF a texture (CMapArea::LoadTerrainTexture): MTXF when it
        // covers them all, else MTXP's flags (the same bits) laid out as MTXF.
        int32_t* flags = nullptr;
        if (t.textures && t.mtxf.size / 4 >= t.textures)
            flags = reinterpret_cast<int32_t*>(t.mtxf.data);
        else if (t.textures && (t.mtxp || t.mtxf))
        {
            split->textureFlags = split_tile::FlagsFromMtxp(t);
            for (uint32_t i = 0; t.mtxf && i < t.mtxf.size / 4 && i < t.textures; ++i)
                split->textureFlags[i] = static_cast<int32_t>(Get32(t.mtxf.data + 4 * i));
            flags = split->textureFlags.data();
        }

        wxl_core_area_split fields{};
        fields.chunk_info         = split->chunkInfo.data();
        fields.doodad_defs        = t.mddf.data;
        fields.map_obj_defs       = t.modf.data;
        fields.doodad_count       = t.mddf.size / split_tile::kDoodadDefSize;
        fields.map_obj_count      = t.modf.size / split_tile::kMapObjDefSize;
        fields.model_names        = reinterpret_cast<char*>(t.mmdx.data);
        fields.map_obj_names      = reinterpret_cast<char*>(t.mwmo.data);
        fields.model_offsets      = reinterpret_cast<uint32_t*>(t.mmid.data);
        fields.map_obj_offsets    = reinterpret_cast<uint32_t*>(t.mwid.data);
        fields.texture_flags      = flags;
        fields.texture_names      = t.textures ? reinterpret_cast<char*>(t.mtex.data) : nullptr;
        fields.texture_names_size = t.textures ? t.mtex.size : 0;

        // The alpha maps' size (64 >> MAMP), which MHDR holds in a single file: _tex0's MAMP.
        if (t.mamp.size >= 1)
        {
            const uint32_t mhdr = 8 + Get32(split->root + 4) + 8;
            split->root[mhdr + split_tile::kMhdrMampValue] = t.mamp.data[0];
        }
        if (!wxl::core::binding::map_area_split_set(area, &fields))
        {
            Drop(area);
            return;
        }
        if (!g_counters.tiles)
            WLOG_INFO(kTag, "a tile without MCIN loads from its split files (%u texture(s), %u doodad(s), %u map object(s)): "
                            "the client reads them in place",
                      t.textures, fields.doodad_count, fields.map_obj_count);
        ++g_counters.tiles;
    }

    void ChunkRead(void* chunk)
    {
        if (g_areas.empty() || !chunk) return;
        wxl_core_map_chunk at{};
        if (!wxl::core::binding::map_chunk_parts(chunk, &at) || !at.header) return;
        SplitArea* split = Holding(at.begin);
        if (!split || at.index_x < 0 || at.index_x > 15 || at.index_y < 0 || at.index_y > 15) return;
        const uint32_t                  i = static_cast<uint32_t>(at.index_x + 16 * at.index_y);
        const split_tile::Tile&         t = split->tile;
        const split_tile::ChunkParts&   p = t.parts[i];
        uint8_t*                        header = static_cast<uint8_t*>(at.header);

        const uint32_t layers = split_tile::UsableLayers(t, i);
        if (layers < p.mcly.size / 16) ++g_counters.layersCut;

        ChunkRefs& refs = split->refs[i];
        refs.allDoodads = split_tile::UsableDoodadRefs(t, i, refs.doodads);
        refs.allMapObjs = split_tile::UsableMapObjRefs(t, i, refs.mapObjs);
        const uint32_t doodads = refs.allDoodads ? p.mcrd.size / 4 : static_cast<uint32_t>(refs.doodads.size());
        const uint32_t mapObjs = refs.allMapObjs ? p.mcrw.size / 4 : static_cast<uint32_t>(refs.mapObjs.size());
        g_counters.refsLeftOut += (p.mcrd.size / 4 - doodads) + (p.mcrw.size / 4 - mapObjs);

        Put32(header + split_tile::kHeaderLayers, layers);
        Put32(header + split_tile::kHeaderSizeAlpha, p.mcal ? p.mcal.size + 8 : 0);
        Put32(header + split_tile::kHeaderDoodadRefs, doodads);
        Put32(header + split_tile::kHeaderMapObjRefs, mapObjs);
        if (Get32(header + split_tile::kHeaderFlags) & split_tile::kHighResHoles)
        {
            const uint16_t holes = split_tile::FoldHoles(header + split_tile::kHeaderHolesHigh);
            std::memcpy(header + split_tile::kHeaderHoles, &holes, sizeof holes);
        }

        wxl_core_chunk_split parts{};
        parts.layers = layers ? p.mcly.data : nullptr;
        parts.alpha  = p.mcal.data;
        parts.shadow = p.mcsh.data;
        parts.refs   = refs.allDoodads ? p.mcrd.data : reinterpret_cast<uint8_t*>(refs.doodads.data());
        wxl::core::binding::map_chunk_split_set(chunk, &parts);
        ++g_counters.chunks;
    }

    namespace
    {
        /// The references of @p list whose placement names a model the client can load; @p kept
        /// filled only when some are not (answers whether all are).
        bool Loadable(const uint32_t* list, uint32_t count, const uint8_t* defs, uint32_t defCount, uint32_t defSize, const char* names,
                      const uint32_t* offsets, bool mapObject, std::vector<uint32_t>& kept)
        {
            kept.clear();
            if (!list || !defs || !names || !offsets) return true;
            bool all = true;
            for (uint32_t i = 0; i < count; ++i)
            {
                // A reference past the placements is the client's to read, as it always was.
                bool ok = list[i] >= defCount;
                if (!ok) ok = model_gate::Loadable(names + offsets[Get32(defs + static_cast<size_t>(list[i]) * defSize)], mapObject);
                if (ok) kept.push_back(list[i]);
                else all = false;
            }
            if (all) kept.clear();
            return all;
        }
    }

    bool CreateRefs(void* chunk, void* area, const uint32_t* refs, uint32_t doodadRefs, uint32_t mapObjRefs)
    {
        if (g_inRefs || !chunk || !area) return false;
        wxl_core_map_chunk at{};
        SplitArea*         split = nullptr;
        if (!g_areas.empty() && wxl::core::binding::map_chunk_parts(chunk, &at)) split = Holding(at.begin);

        // The lists the client would make: a split chunk's MCRW and MCRD (what it can follow), or the
        // one list it was handed.
        const uint32_t *mapObjs = nullptr, *doodads = nullptr;
        uint32_t        nMapObj = 0, nDoodad = 0;
        wxl_core_area_split placements{};
        if (split)
        {
            if (at.index_x < 0 || at.index_x > 15 || at.index_y < 0 || at.index_y > 15) return false;
            const uint32_t                i = static_cast<uint32_t>(at.index_x + 16 * at.index_y);
            const split_tile::ChunkParts& p = split->tile.parts[i];
            const ChunkRefs&              r = split->refs[i];
            mapObjs = r.allMapObjs ? reinterpret_cast<const uint32_t*>(p.mcrw.data) : r.mapObjs.data();
            nMapObj = r.allMapObjs ? p.mcrw.size / 4 : static_cast<uint32_t>(r.mapObjs.size());
            doodads = r.allDoodads ? reinterpret_cast<const uint32_t*>(p.mcrd.data) : r.doodads.data();
            nDoodad = r.allDoodads ? p.mcrd.size / 4 : static_cast<uint32_t>(r.doodads.size());
            const split_tile::Tile& t = split->tile;
            placements.doodad_defs     = t.mddf.data;
            placements.map_obj_defs    = t.modf.data;
            placements.doodad_count    = t.mddf.size / split_tile::kDoodadDefSize;
            placements.map_obj_count   = t.modf.size / split_tile::kMapObjDefSize;
            placements.model_names     = reinterpret_cast<char*>(t.mmdx.data);
            placements.map_obj_names   = reinterpret_cast<char*>(t.mwmo.data);
            placements.model_offsets   = reinterpret_cast<uint32_t*>(t.mmid.data);
            placements.map_obj_offsets = reinterpret_cast<uint32_t*>(t.mwid.data);
        }
        else
        {
            if (!refs || !(doodadRefs + mapObjRefs) || !wxl::core::binding::map_area_placements(area, &placements)) return false;
            doodads = refs;
            nDoodad = doodadRefs;
            mapObjs = refs + doodadRefs;
            nMapObj = mapObjRefs;
        }

        // Models the client cannot load are left out.
        std::vector<uint32_t> keptMapObjs, keptDoodads;
        const bool allMapObjs = Loadable(mapObjs, nMapObj, static_cast<const uint8_t*>(placements.map_obj_defs), placements.map_obj_count,
                                         split_tile::kMapObjDefSize, placements.map_obj_names, placements.map_obj_offsets, true, keptMapObjs);
        const bool allDoodads = Loadable(doodads, nDoodad, static_cast<const uint8_t*>(placements.doodad_defs), placements.doodad_count,
                                         split_tile::kDoodadDefSize, placements.model_names, placements.model_offsets, false, keptDoodads);
        if (!allMapObjs)
        {
            model_gate::CountLeftOut(nMapObj - static_cast<uint32_t>(keptMapObjs.size()));
            mapObjs = keptMapObjs.data();
            nMapObj = static_cast<uint32_t>(keptMapObjs.size());
        }
        if (!allDoodads)
        {
            model_gate::CountLeftOut(nDoodad - static_cast<uint32_t>(keptDoodads.size()));
            doodads = keptDoodads.data();
            nDoodad = static_cast<uint32_t>(keptDoodads.size());
        }
        if (!split && allMapObjs && allDoodads) return false; // a tile in one file, every model loadable: the client's own call

        // One list in a single file: map objects, then doodads. Both the client's own call.
        g_inRefs = true;
        if (nMapObj) wxl::core::binding::map_chunk_create_refs(chunk, area, mapObjs, 0, nMapObj);
        if (nDoodad) wxl::core::binding::map_chunk_create_refs(chunk, area, doodads, nDoodad, 0);
        g_inRefs = false;
        return true;
    }

    void AreaFreeing(void* area) { Drop(area); }

    void Clear()
    {
        model_gate::Clear();
        g_providers.clear();
        g_areas.clear();
        g_inRefs         = false;
        g_toldUnprovided = false;
    }

    const Counters& Count() { return g_counters; }
    uint32_t        Held() { return static_cast<uint32_t>(g_areas.size()); }
}
