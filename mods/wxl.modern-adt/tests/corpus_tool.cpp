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
//   wxl-core-modern-adt-corpus <maps folder>
//
// Exits 1 when any tile has an error.

#include "adt/Reader.hpp"
#include "adt/Survey.hpp"
#include "services/SplitTile.hpp"
#include "store/TileSet.hpp"

#include <algorithm>
#include <map>

#include <cctype>
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
        std::fprintf(stderr, "usage: wxl-core-modern-adt-corpus <maps folder>\n");
        return 2;
    }
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
            for (uint32_t c = 0; c < 256; ++c)
            {
                ++nativeChunks[e];
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
    for (const std::string& e : nativeFirst) std::printf("  %s\n", e.c_str());
    std::vector<std::pair<uint32_t, std::string>> small;
    for (const auto& m : byName) small.emplace_back(m.second, m.first);
    std::sort(small.begin(), small.end());
    std::printf("maps split by name (%zu), smallest first:", small.size());
    for (size_t i = 0; i < small.size() && i < 25; ++i) std::printf(" %s %u", small[i].second.c_str(), small[i].first);
    std::printf("\n");
    return stats.errors || nativeErrors ? 1 : 0;
}
