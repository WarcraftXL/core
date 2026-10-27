// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Every tile of a folder of maps laid out as the client's World\Maps (<map>/<map>.wdt and
// <map>_<x>_<y>.adt with its _tex0, _tex1, _obj0, _obj1 and _lod), surveyed as wxl.modern-adt reads
// them (adt::SurveyTile): per era, per file, the chunks it does not know, and what does not hold.
// The corpus is the user's own (extracted from their archives); it never enters a repository.
// Each tile whose root has no MCIN is also read as the mod's TileSet reads it and indexed as the
// runtime indexes it to load it natively (services/SplitTile): the 256 chunks, the layers the
// client can draw, the references it can follow; the maps split by name are listed, smallest first.
//
// The client's own query paths are run over what the runtime would hand it: every chunk's layers
// as the ground-type query reads them (CMap::QueryGroundTypeTerrain: m_layers[k] for the k the
// low-quality map names), its liquids after the runtime's fixes (formats, types, bounds), each
// map's WDL as the far terrain reads it. With a listfile, ids are named as the mod names them.
//
//   wxl-core-modern-adt-corpus <maps folder> [--listfile <id;path CSV>]
//
// Exits 1 when any tile has an error.

#include "adt/Reader.hpp"
#include "adt/Survey.hpp"
#include "services/SplitTile.hpp"
#include "services/TerrainLowDetail.hpp"
#include "store/Listfile.hpp"
#include "store/TileSet.hpp"

#include <algorithm>
#include <map>

#include <cctype>
#include <cstring>
#include <tuple>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

namespace fs = std::filesystem;
using namespace wxl::modern_adt;

namespace
{
    std::vector<uint8_t> Read(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return {};
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: wxl-core-modern-adt-corpus <maps folder> [--listfile <csv>]\n");
        return 2;
    }
    // A listfile, as the mod reads it.
    Listfile listfile;
    for (int i = 2; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--listfile")
        {
            const auto csv = Read(argv[i + 1]);
            const uint64_t hash = Listfile::Hash(csv.data(), csv.size());
            listfile.Use(Listfile::BuildIndex(csv.data(), csv.size(), hash), hash);
            std::printf("listfile %s: %u name(s)\n", argv[i + 1], listfile.Count());
        }
    // The liquid types of 3.3.5a's LiquidType.dbc.
    std::vector<uint8_t> knownLiquids(182, 0);
    for (int id : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17, 19, 20, 21, 41, 61, 81, 100, 121, 141, 181}) knownLiquids[id] = 1;
    uint64_t queries = 0, unqueryable = 0, placeholders = 0, liquidInstances = 0, liquidFormats = 0, liquidTypes = 0, liquidDry = 0,
             liquidUnsafe = 0, texturesNamed = 0, texturesUnnamed = 0, modelsNamed = 0, modelsUnnamed = 0;
    uint64_t wdlStock = 0, wdlRetail = 0, wdlAreas = 0, wdlSkipped = 0, wdlNoMaof = 0;
    adt::SurveyStats stats;
    const std::regex root(R"((.+)_(\d+)_(\d+)\.adt)", std::regex::icase);
    uint32_t         maps = 0, mapsWithMaid = 0, mapsHeight = 0;
    // The native load's own reading of each split set.
    // [0] split by name, [1] by FileDataID.
    uint64_t nativeTiles[2] = {}, nativeErrors = 0, nativeChunks[2] = {}, layersKept[2] = {}, layersCut[2] = {}, refsKept[2] = {},
             refsLeftOut[2] = {}, noMtex[2] = {}, moreThanFour[2] = {};
    std::vector<std::string>        nativeFirst;
    std::map<std::string, uint32_t> byName; ///< maps split by name: tiles
    namespace st = wxl::core::services::split_tile;
    for (const auto& dir : fs::directory_iterator(argv[1]))
    {
        if (!dir.is_directory()) continue;
        ++maps;
        const std::string mapName = dir.path().filename().string();
        uint32_t          flags = 0;
        const auto        wdt = Read(dir.path() / (mapName + ".wdt"));
        adt::MapFlags(adt::Reader(wdt.data(), wdt.size()), &flags);
        mapsWithMaid += adt::Reader(wdt.data(), wdt.size()).Find(adt::Id("MAID")) ? 1u : 0u;
        mapsHeight += (flags & 0x80) ? 1u : 0u;
        // Its far terrain, as CMapLowDetail::Load reads it (or the runtime for a later client's WDL).
        const auto wdl = Read(dir.path() / (mapName + ".wdl"));
        if (!wdl.empty())
        {
            const wxl::core::services::low_detail::Plan plan = wxl::core::services::low_detail::Read(wdl.data(), static_cast<uint32_t>(wdl.size()));
            if (plan.clientReads) ++wdlStock;
            else if (!plan.maof) ++wdlNoMaof;
            else
            {
                ++wdlRetail;
                wdlAreas += plan.areas.size();
                wdlSkipped += plan.skipped;
            }
        }
        for (const auto& file : fs::directory_iterator(dir.path()))
        {
            const std::string name = file.path().filename().string();
            std::smatch       m;
            if (!std::regex_match(name, m, root)) continue;
            const std::string stem = (dir.path() / m[1].str()).string() + "_" + m[2].str() + "_" + m[3].str();
            adt::TileFiles    files;
            files.root = Read(file.path());
            files.tex0 = Read(stem + "_tex0.adt");
            files.tex1 = Read(stem + "_tex1.adt");
            files.obj0 = Read(stem + "_obj0.adt");
            files.obj1 = Read(stem + "_obj1.adt");
            files.lod  = Read(stem + "_lod.adt");
            const adt::Era era = adt::SurveyTile(mapName + "/" + name, files, flags, stats);
            if (!TileSet::IsSplitRoot(files.root.data(), static_cast<uint32_t>(files.root.size()))) continue;
            if (era == adt::Era::split) ++byName[mapName];
            const int e = era == adt::Era::split ? 0 : 1;
            ++nativeTiles[e];
            st::Tile    tile;
            std::string why;
            if (!st::Index(files.root.data(), static_cast<uint32_t>(files.root.size()), files.tex0.empty() ? nullptr : files.tex0.data(),
                           static_cast<uint32_t>(files.tex0.size()), files.obj0.empty() ? nullptr : files.obj0.data(),
                           static_cast<uint32_t>(files.obj0.size()), tile, why) ||
                !why.empty())
            {
                ++nativeErrors;
                if (nativeFirst.size() < 20) nativeFirst.push_back(mapName + "/" + name + ": " + why);
                continue;
            }
            if (!tile.textures) ++noMtex[e];
            // Textures and models by FileDataID named from the listfile, as the runtime asks the mod.
            if (!tile.textures && tile.mdid.size >= 4)
            {
                tile.textures = tile.mdid.size / 4;
                tile.missing.assign(tile.textures, 0);
                for (uint32_t i = 0; i < tile.textures; ++i)
                {
                    uint32_t id;
                    std::memcpy(&id, tile.mdid.data + 4 * i, 4);
                    std::string n;
                    if (!listfile.Empty() && id && listfile.Name(id, n)) ++texturesNamed;
                    else
                    {
                        tile.missing[i] = 1;
                        ++texturesUnnamed;
                    }
                }
            }
            for (const auto& [defs, size, flagAt, flag] : {std::tuple{tile.mddf, 36u, 34u, uint16_t{0x40}}, std::tuple{tile.modf, 64u, 56u, uint16_t{0x8}}})
                for (uint32_t i = 0; i < defs.size / size; ++i)
                {
                    uint16_t f;
                    uint32_t id;
                    std::memcpy(&f, defs.data + i * size + flagAt, 2);
                    std::memcpy(&id, defs.data + i * size, 4);
                    if (!(f & flag)) continue;
                    std::string n;
                    if (!listfile.Empty() && listfile.Name(id, n)) ++modelsNamed;
                    else ++modelsUnnamed;
                }
            // Its liquids after the runtime's fixes, held to what the client reads.
            {
                std::vector<uint8_t> root = files.root;
                const st::LiquidFixes fixes = st::FixLiquids(root.data(), static_cast<uint32_t>(root.size()), knownLiquids);
                liquidInstances += fixes.instances;
                liquidFormats += fixes.formats;
                liquidTypes += fixes.types;
                liquidDry += fixes.dropped;
                // Again: a second pass finds nothing left to fix (every instance in bounds, of a format and type the client has).
                const st::LiquidFixes again = st::FixLiquids(root.data(), static_cast<uint32_t>(root.size()), knownLiquids);
                liquidUnsafe += again.formats + again.types + again.dropped;
            }
            // The layers the client is handed, read as the ground-type query reads them.
            const bool     needs       = st::NeedsPlaceholder(tile);
            const uint32_t placeholder = tile.textures;
            if (needs) tile.textures += 1;
            for (uint32_t c = 0; c < 256; ++c)
            {
                ++nativeChunks[e];
                const st::ChunkLayers plan = st::PlanLayers(tile, c, placeholder);
                const uint8_t* entries = plan.own ? plan.table : tile.parts[c].mcly.data;
                bool ok = plan.count >= 1 && plan.count <= 4 && entries && (plan.own || tile.parts[c].mcly.size >= 64);
                for (uint32_t k = 0; ok && k < plan.count; ++k)
                {
                    uint32_t texture, layerFlags, offset;
                    std::memcpy(&texture, entries + 16 * k, 4);
                    std::memcpy(&layerFlags, entries + 16 * k + 4, 4);
                    std::memcpy(&offset, entries + 16 * k + 8, 4);
                    ok = texture < tile.textures && (!(layerFlags & 0x100) || offset < tile.parts[c].mcal.size);
                }
                // CMap::QueryGroundTypeTerrain: for each of the 64 cells, m_layers[k], k from the low-quality map.
                const uint8_t* header = files.root.data() + tile.chunkOffset[c] + 8;
                for (uint32_t row = 0; row < 8; ++row)
                {
                    uint16_t bits;
                    std::memcpy(&bits, header + 0x40 + 2 * row, 2);
                    for (uint32_t col = 0; col < 8; ++col)
                    {
                        ++queries;
                        const uint32_t k = (bits >> (2 * col)) & 3;
                        if (!ok || (!plan.own && 16 * (k + 1) > tile.parts[c].mcly.size)) ++unqueryable;
                    }
                }
                if (plan.own && !st::UsableLayers(tile, c)) ++placeholders;
                const uint32_t kept = st::UsableLayers(tile, c), named = tile.parts[c].mcly.size / 16;
                layersKept[e] += kept;
                layersCut[e] += named - kept;
                moreThanFour[e] += named > 4 ? 1u : 0u;
                std::vector<uint32_t> d, w;
                const uint32_t nd = tile.parts[c].mcrd.size / 4, nw = tile.parts[c].mcrw.size / 4;
                const uint32_t kd = st::UsableDoodadRefs(tile, c, d) ? nd : static_cast<uint32_t>(d.size());
                const uint32_t kw = st::UsableMapObjRefs(tile, c, w) ? nw : static_cast<uint32_t>(w.size());
                refsKept[e] += kd + kw;
                refsLeftOut[e] += (nd - kd) + (nw - kw);
            }
        }
    }
    std::printf("%u maps (%u with MAID, %u flagged 0x80), %llu tiles: %llu monolithic, %llu split by name, %llu by FileDataID\n", maps,
                mapsWithMaid, mapsHeight, static_cast<unsigned long long>(stats.tiles), static_cast<unsigned long long>(stats.era[0]),
                static_cast<unsigned long long>(stats.era[1]), static_cast<unsigned long long>(stats.era[2]));
    std::printf("files: root %llu, tex0 %llu, tex1 %llu, obj0 %llu, obj1 %llu, lod %llu\n", static_cast<unsigned long long>(stats.files[0]),
                static_cast<unsigned long long>(stats.files[1]), static_cast<unsigned long long>(stats.files[2]),
                static_cast<unsigned long long>(stats.files[3]), static_cast<unsigned long long>(stats.files[4]),
                static_cast<unsigned long long>(stats.files[5]));
    std::printf("%llu chunks (%llu with 64-bit holes, %llu of more than 4 layers), %llu layers, %llu alpha maps decoded, %llu doodads (%llu by id), %llu map objects (%llu by id)\n",
                static_cast<unsigned long long>(stats.chunks), static_cast<unsigned long long>(stats.highResHoles),
                static_cast<unsigned long long>(stats.morethanFour), static_cast<unsigned long long>(stats.layers),
                static_cast<unsigned long long>(stats.alphaMaps), static_cast<unsigned long long>(stats.doodads),
                static_cast<unsigned long long>(stats.doodadsById), static_cast<unsigned long long>(stats.mapObjects),
                static_cast<unsigned long long>(stats.mapObjectsById));
    std::printf("%llu root name(s) holding a _lod file's chunks (left out)\n", static_cast<unsigned long long>(stats.misnamed));
    std::printf("chunks not known:");
    for (const auto& u : stats.unknown) std::printf(" %s %llu", u.first.c_str(), static_cast<unsigned long long>(u.second));
    std::printf("\n%llu error(s)\n", static_cast<unsigned long long>(stats.errors));
    for (const std::string& e : stats.firstErrors) std::printf("  %s\n", e.c_str());
    for (int e = 0; e < 2; ++e)
        std::printf("native load, %s: %llu root(s) without MCIN indexed, %llu chunks (%llu naming more than 4 layers); layers kept %llu, "
                    "past 4 or not drawable %llu; references kept %llu, left out %llu; %llu tile(s) without MTEX\n",
                    e ? "by FileDataID" : "split by name", static_cast<unsigned long long>(nativeTiles[e]),
                    static_cast<unsigned long long>(nativeChunks[e]), static_cast<unsigned long long>(moreThanFour[e]),
                    static_cast<unsigned long long>(layersKept[e]), static_cast<unsigned long long>(layersCut[e]),
                    static_cast<unsigned long long>(refsKept[e]), static_cast<unsigned long long>(refsLeftOut[e]),
                    static_cast<unsigned long long>(noMtex[e]));
    std::printf("native load: %llu error(s)\n", static_cast<unsigned long long>(nativeErrors));
    std::printf("client queries: %llu ground-type queries, %llu unsafe; %llu chunk(s) given the placeholder layer\n",
                static_cast<unsigned long long>(queries), static_cast<unsigned long long>(unqueryable), static_cast<unsigned long long>(placeholders));
    std::printf("liquids: %llu instance(s), %llu given a format the client reads, %llu a type it has, %llu chunk(s) left dry; %llu unsafe after the fixes\n",
                static_cast<unsigned long long>(liquidInstances), static_cast<unsigned long long>(liquidFormats),
                static_cast<unsigned long long>(liquidTypes), static_cast<unsigned long long>(liquidDry), static_cast<unsigned long long>(liquidUnsafe));
    std::printf("FileDataIDs: textures %llu named, %llu not; models %llu named, %llu not\n", static_cast<unsigned long long>(texturesNamed),
                static_cast<unsigned long long>(texturesUnnamed), static_cast<unsigned long long>(modelsNamed),
                static_cast<unsigned long long>(modelsUnnamed));
    std::printf("WDLs: %llu read by the client, %llu of later clients (%llu areas, %llu offsets past the file), %llu without MAOF\n",
                static_cast<unsigned long long>(wdlStock), static_cast<unsigned long long>(wdlRetail), static_cast<unsigned long long>(wdlAreas),
                static_cast<unsigned long long>(wdlSkipped), static_cast<unsigned long long>(wdlNoMaof));
    for (const std::string& e : nativeFirst) std::printf("  %s\n", e.c_str());
    std::vector<std::pair<uint32_t, std::string>> small;
    for (const auto& m : byName) small.emplace_back(m.second, m.first);
    std::sort(small.begin(), small.end());
    std::printf("maps split by name (%zu), smallest first:", small.size());
    for (size_t i = 0; i < small.size() && i < 25; ++i) std::printf(" %s %u", small[i].second.c_str(), small[i].first);
    std::printf("\n");
    return stats.errors || nativeErrors || unqueryable || liquidUnsafe ? 1 : 0;
}
