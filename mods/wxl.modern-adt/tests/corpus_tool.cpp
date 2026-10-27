// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Every tile of a folder of maps laid out as the client's World\Maps (<map>/<map>.wdt and
// <map>_<x>_<y>.adt with its _tex0, _tex1, _obj0, _obj1 and _lod), surveyed as wxl.modern-adt reads
// them (adt::SurveyTile): per era, per file, the chunks it does not know, and what does not hold.
// The corpus is the user's own (extracted from their archives); it never enters a repository.
//
//   wxl-core-modern-adt-corpus <maps folder>
//
// Exits 1 when any tile has an error.

#include "adt/Reader.hpp"
#include "adt/Survey.hpp"

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
            adt::SurveyTile(mapName + "/" + name, files, flags, stats);
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
    return stats.errors ? 1 : 0;
}
