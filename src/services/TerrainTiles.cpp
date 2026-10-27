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
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace wxl::core::services::terrain_tiles
{
    namespace
    {
        constexpr const char* kTag = "terrain";
        /// Zeros kept past each file the runtime copies: the client reads an alpha map by its own
        /// flags (4096 bytes, or 2048, or a run-length map), so one cut short at the end of _tex0
        /// reads zeros, not past the buffer.
        constexpr size_t kPadding = 8192;

        struct Provider
        {
            uint32_t            mod = 0;
            wxl_terrain_tile_fn fn = nullptr;
            void*               user = nullptr;
            wxl_terrain_name_fn name_fn = nullptr; ///< abi 0.1.46
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
            std::vector<uint8_t>      files[WXL_TERRAIN_TILE_FILES]; ///< each padded with kPadding zeros past its end
            uint32_t                  sizes[WXL_TERRAIN_TILE_FILES] = {};
            std::array<std::array<uint8_t, 64>, 256> layerTables{}; ///< layers the runtime gives a chunk (PlanLayers' own)
            split_tile::Tile          tile;
            std::vector<uint8_t>      chunkInfo;      ///< 256 SMChunkInfo: offset, size, flags, padding
            std::vector<int32_t>      textureFlags;   ///< MTXF as the client reads it, when MTXF is absent or short
            // Names given to FileDataIDs (8.1 and later), laid out as MTEX, MMDX/MMID, MWMO/MWID.
            std::vector<char>         textureNames, modelNames, mapObjNames;
            std::vector<uint32_t>     modelOffsets, mapObjOffsets;
            uint32_t                  placeholder = UINT32_MAX; ///< kPlaceholderTexture's index, when given
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

    namespace
    {
        bool g_toldUnnamed = false;
        bool g_toldLiquids = false;
        bool g_toldPlaceholder = false;

        /// The liquid types the client's LiquidType.dbc has, by id (read once through its file layer);
        /// Water, Ocean, Magma and Slime when it cannot be read.
        const std::vector<uint8_t>& KnownLiquids()
        {
            static std::vector<uint8_t> known;
            if (!known.empty()) return known;
            std::vector<uint8_t> dbc;
            if (void* file = wxl::core::binding::file_open("DBFilesClient\\LiquidType.dbc"))
            {
                const uint64_t size = wxl::core::binding::file_size(file);
                if (size >= 20 && size < (16u << 20))
                {
                    dbc.resize(static_cast<size_t>(size));
                    if (wxl::core::binding::file_read(file, dbc.data(), static_cast<uint32_t>(size)) != size) dbc.clear();
                }
                wxl::core::binding::file_close(file);
            }
            if (dbc.size() >= 20 && std::memcmp(dbc.data(), "WDBC", 4) == 0)
            {
                const uint32_t records = Get32(dbc.data() + 4), recordSize = Get32(dbc.data() + 12);
                for (uint32_t i = 0; recordSize >= 4 && 20 + static_cast<uint64_t>(i + 1) * recordSize <= dbc.size() && i < records; ++i)
                {
                    const uint32_t id = Get32(dbc.data() + 20 + static_cast<size_t>(i) * recordSize);
                    if (id < 65536)
                    {
                        if (known.size() <= id) known.resize(id + 1, 0);
                        known[id] = 1;
                    }
                }
            }
            if (known.empty()) known = {0, 1, 1, 1, 1};
            return known;
        }

        /// The name the providers give a FileDataID; false when none does.
        bool NameOf(uint32_t id, std::string& out)
        {
            if (!id) return false;
            for (const Provider& p : std::vector<Provider>(g_providers))
            {
                if (!p.name_fn) continue;
                char     name[260] = {};
                wxl_bool named     = WXL_FALSE;
                CallMod(p.mod, kTag, "a tile provider's name function", [&] { named = p.name_fn(p.user, id, name, sizeof name); });
                name[sizeof name - 1] = 0;
                if (named == WXL_TRUE && name[0])
                {
                    out = name;
                    return true;
                }
            }
            return false;
        }

        /// Placements by FileDataID of one table (MDDF or MODF): each named one given its name in a
        /// table laid out as MMDX/MMID (the file's own names first), the rest left flagged.
        uint32_t NamePlacements(split_tile::Span defs, uint32_t defSize, uint16_t byId, uint32_t flagsAt, split_tile::Span& names,
                                split_tile::Span& offsets, std::vector<char>& nameTable, std::vector<uint32_t>& offsetTable,
                                uint32_t& unnamed)
        {
            std::vector<uint32_t> flagged;
            for (uint32_t i = 0; i < defs.size / defSize; ++i)
            {
                uint16_t flags;
                std::memcpy(&flags, defs.data + static_cast<size_t>(i) * defSize + flagsAt, 2);
                if (flags & byId) flagged.push_back(i);
            }
            if (flagged.empty()) return 0;
            nameTable.assign(names.data, names.data + names.size);
            offsetTable.assign(offsets.size / 4, 0);
            if (offsets.size) std::memcpy(offsetTable.data(), offsets.data, offsetTable.size() * 4);
            std::vector<std::pair<uint32_t, uint32_t>> known; // FileDataID -> its nameId
            std::vector<std::pair<uint32_t, uint32_t>> rename; // entry -> nameId
            uint32_t                                   named = 0;
            for (uint32_t i : flagged)
            {
                const uint32_t id = Get32(defs.data + static_cast<size_t>(i) * defSize);
                uint32_t       nameId = UINT32_MAX;
                for (const auto& k : known)
                    if (k.first == id) nameId = k.second;
                std::string name;
                if (nameId == UINT32_MAX && NameOf(id, name))
                {
                    nameId = static_cast<uint32_t>(offsetTable.size());
                    offsetTable.push_back(static_cast<uint32_t>(nameTable.size()));
                    nameTable.insert(nameTable.end(), name.begin(), name.end());
                    nameTable.push_back(0);
                    known.emplace_back(id, nameId);
                }
                if (nameId == UINT32_MAX)
                {
                    ++unnamed;
                    continue;
                }
                rename.emplace_back(i, nameId);
                ++named;
            }
            for (const auto& r : rename) split_tile::NameByIndex(defs, defSize, r.first, r.second);
            names   = split_tile::Span{reinterpret_cast<uint8_t*>(nameTable.data()), static_cast<uint32_t>(nameTable.size())};
            offsets = split_tile::Span{reinterpret_cast<uint8_t*>(offsetTable.data()), static_cast<uint32_t>(offsetTable.size() * 4)};
            return named;
        }
    }

    /// A tile with a chunk none of whose layers can be drawn gets kPlaceholderTexture after its own
    /// textures, for those chunks' one layer (PlanLayers).
    static void GivePlaceholder(SplitArea& split)
    {
        split_tile::Tile& t = split.tile;
        if (!split_tile::NeedsPlaceholder(t)) return;
        if (split.textureNames.empty() && t.mtex.size)
            split.textureNames.assign(reinterpret_cast<const char*>(t.mtex.data), reinterpret_cast<const char*>(t.mtex.data) + t.mtex.size);
        // MTEX's names end with a NUL; a table cut short is ended before the placeholder.
        if (!split.textureNames.empty() && split.textureNames.back() != 0) split.textureNames.push_back(0);
        const std::string name = split_tile::kPlaceholderTexture;
        split.textureNames.insert(split.textureNames.end(), name.begin(), name.end());
        split.textureNames.push_back(0);
        split.placeholder = t.textures;
        t.textures += 1;
        t.mtex = split_tile::Span{reinterpret_cast<uint8_t*>(split.textureNames.data()), static_cast<uint32_t>(split.textureNames.size())};
        if (!g_toldPlaceholder)
            WLOG_INFO(kTag, "chunks of a tile without MCIN have no layer the client can draw (textures not named, or none): they are "
                            "drawn with %s", split_tile::kPlaceholderTexture);
        g_toldPlaceholder = true;
    }

    static void NameFileIds(SplitArea& split, const char* path)
    {
        split_tile::Tile& t       = split.tile;
        uint32_t          unnamed = 0, named = 0;
        // Textures by FileDataID (MDID, no MTEX): their names in MDID's order, a placeholder for
        // one no one names, whose layers are then left out.
        if (!t.textures && t.mdid.size >= 4)
        {
            const uint32_t count = t.mdid.size / 4;
            t.missing.assign(count, 0);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t id = Get32(t.mdid.data + 4 * i);
                std::string    name;
                if (NameOf(id, name)) ++named;
                else
                {
                    name         = "FileDataID_" + std::to_string(id) + ".blp";
                    t.missing[i] = 1;
                    ++unnamed;
                }
                split.textureNames.insert(split.textureNames.end(), name.begin(), name.end());
                split.textureNames.push_back(0);
            }
            t.mtex     = split_tile::Span{reinterpret_cast<uint8_t*>(split.textureNames.data()), static_cast<uint32_t>(split.textureNames.size())};
            t.textures = count;
        }
        named += NamePlacements(t.mddf, split_tile::kDoodadDefSize, split_tile::kDoodadByFileId, 34, t.mmdx, t.mmid, split.modelNames,
                                split.modelOffsets, unnamed);
        named += NamePlacements(t.modf, split_tile::kMapObjDefSize, split_tile::kMapObjByFileId, 56, t.mwmo, t.mwid, split.mapObjNames,
                                split.mapObjOffsets, unnamed);
        g_counters.named += named;
        g_counters.unnamed += unnamed;
        if (unnamed && !g_toldUnnamed)
            WLOG_WARN(kTag, "%s names %u file(s) by FileDataID that no tile provider named (a listfile for wxl.modern-adt): their layers "
                            "and placements are left out",
                      path, unnamed);
        if (unnamed) g_toldUnnamed = true;
    }

    wxl_result SetProvider(uint32_t mod, const wxl_terrain_tile_provider* provider)
    {
        // A provider of abi 0.1.45 ends before name_fn.
        if (provider && (provider->struct_size < offsetof(wxl_terrain_tile_provider, name_fn) || !provider->fn)) return WXL_E_INVALID;
        const wxl_terrain_name_fn nameFn =
            provider && provider->struct_size >= sizeof(wxl_terrain_tile_provider) ? provider->name_fn : nullptr;
        for (Provider& p : g_providers)
            if (p.mod == mod)
            {
                if (provider)
                {
                    p.fn      = provider->fn;
                    p.user    = provider->user;
                    p.name_fn = nameFn;
                }
                else
                    ForgetMod(mod);
                return WXL_OK;
            }
        if (provider) g_providers.push_back(Provider{mod, provider->fn, provider->user, nameFn});
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

        // Its liquids, before the client reads them: formats and types this client knows.
        const split_tile::LiquidFixes fixed =
            split_tile::FixLiquids(static_cast<uint8_t*>(const_cast<void*>(data)), size, KnownLiquids());
        g_counters.liquidFormats += fixed.formats;
        g_counters.liquidTypes += fixed.types;
        g_counters.liquidsDropped += fixed.dropped;
        if ((fixed.formats || fixed.types || fixed.dropped) && !g_toldLiquids)
        {
            WLOG_INFO(kTag, "a tile without MCIN has liquids of later clients: %u of %u instance(s) given a vertex format this client reads "
                            "(LiquidObject ids, format 3), %u a liquid type its LiquidType.dbc has, %u chunk(s) of liquid out of bounds left dry",
                      fixed.formats, fixed.instances, fixed.types, fixed.dropped);
            g_toldLiquids = true;
        }

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
                    split->files[k].resize(static_cast<size_t>(files.size[k]) + kPadding, 0);
                    split->sizes[k] = files.size[k];
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
        if (!split_tile::Index(split->root, size, tex0.empty() ? nullptr : tex0.data(), split->sizes[WXL_TERRAIN_TILE_TEX0],
                               obj0.empty() ? nullptr : obj0.data(), split->sizes[WXL_TERRAIN_TILE_OBJ0], split->tile, why))
        {
            WLOG_ERROR(kTag, "%s has no MCIN and cannot be read as a split tile (%s): the client reads it as it is", path, why.c_str());
            return;
        }
        if (!why.empty()) WLOG_WARN(kTag, "%s: %sthat file is not used", path, why.c_str());
        NameFileIds(*split, path);
        GivePlaceholder(*split);
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

        const split_tile::ChunkLayers plan = split_tile::PlanLayers(t, i, split->placeholder);
        const uint32_t                layers = plan.count;
        if (split_tile::UsableLayers(t, i) < p.mcly.size / 16) ++g_counters.layersCut;
        if (plan.own) std::memcpy(split->layerTables[i].data(), plan.table, sizeof plan.table);

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
        parts.layers = plan.own ? split->layerTables[i].data() : p.mcly.data;
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

    uint32_t WdtLoaded()
    {
        wxl_core_wdt wdt{};
        if (wxl::core::binding::map_wdt(&wdt) != WXL_TRUE || !wdt.flags || !wdt.tiles || !wdt.map_path || !wdt.map_name) return 0;
        // A WDT of later clients: height texturing (0x80), MAID (0x200, or the chunk itself).
        const std::vector<uint8_t>& bytes = terrain_layers::Wdt();
        bool                        later = (*wdt.flags & (0x80u | 0x200u)) != 0;
        for (size_t at = 0; !later && at + 8 <= bytes.size();)
        {
            later = std::memcmp(bytes.data() + at, "DIAM", 4) == 0;
            at += 8 + static_cast<size_t>(Get32(bytes.data() + at + 4));
        }
        if (!later) return 0;
        uint32_t    missing = 0, present = 0;
        std::string listed;
        char        name[300];
        for (uint32_t i = 0; i < 4096; ++i)
        {
            if (!(wdt.tiles[2 * i] & 1u)) continue;
            ++present;
            std::snprintf(name, sizeof name, "%s\\%s_%u_%u.adt", wdt.map_path, wdt.map_name, i % 64, i / 64);
            if (wxl::core::binding::file_exists(name) == WXL_TRUE) continue;
            wdt.tiles[2 * i] &= ~1u;
            ++missing;
            if (listed.size() < 600) listed += (listed.empty() ? "" : " ") + std::to_string(i % 64) + "_" + std::to_string(i / 64);
        }
        g_counters.tilesAbsent += missing;
        if (missing)
            WLOG_WARN(kTag, "%s marks %u of its %u tile(s) present that its files do not have (x_y: %s%s): they are not loaded, the ground there is empty",
                      wdt.path ? wdt.path : "the map's WDT", missing, present, listed.c_str(), listed.size() >= 600 ? " ..." : "");
        return missing;
    }

    void Clear()
    {
        model_gate::Clear();
        g_providers.clear();
        g_areas.clear();
        g_inRefs         = false;
        g_toldUnprovided = false;
        g_toldUnnamed    = false;
        g_toldLiquids    = false;
        g_toldPlaceholder = false;
    }

    const Counters& Count() { return g_counters; }
    uint32_t        Held() { return static_cast<uint32_t>(g_areas.size()); }
}
