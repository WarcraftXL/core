// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A tile read as later clients write it, every file of its set (wowdev ADT/v18): the root
// (<map>_<x>_<y>.adt), its textures (_tex0, _tex1), its objects (_obj0, _obj1) and its far view
// (_lod). Later clients parse every file of a set alike; so does this. What it checks is what a
// native load would lean on: chunks inside their file, 256 MCNKs, layers naming textures that
// exist, alpha maps that decode, placements naming models that exist, chunk references naming
// placements that exist. What it does not know is counted, not refused.
//
//   era  monolithic   one file with MCIN (3.3.5a and earlier)
//        split        root + _tex + _obj by file name (Cataclysm to Legion)
//        file ids     textures (MDID, MHID) or models (MDDF/MODF flag) by FileDataID (8.1 and later)

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wxl::modern_adt::adt
{
    enum class Era : uint8_t
    {
        monolithic,
        split,
        file_ids,
    };

    /// A tile's files; an empty one is absent.
    struct TileFiles
    {
        std::vector<uint8_t> root, tex0, tex1, obj0, obj1, lod;
    };

    /// What a survey of many tiles found.
    struct SurveyStats
    {
        uint64_t tiles = 0;
        uint64_t era[3] = {};
        uint64_t files[6] = {};                        ///< root, tex0, tex1, obj0, obj1, lod present
        uint64_t chunks = 0;                           ///< MCNKs read in roots
        uint64_t layers = 0, alphaMaps = 0;            ///< MCLY entries, alpha maps decoded
        uint64_t doodads = 0, mapObjects = 0;          ///< MDDF, MODF entries
        uint64_t doodadsById = 0, mapObjectsById = 0;  ///< of those, by FileDataID
        uint64_t highResHoles = 0;                     ///< root chunks with 64-bit holes
        uint64_t morethanFour = 0;                     ///< chunks of more than 4 layers
        std::map<std::string, uint64_t> unknown;       ///< "file:CHNK" -> count
        uint64_t misnamed = 0;                         ///< "roots" holding a _lod file's chunks (MLHD, no MHDR): an extraction's naming
        uint64_t errors = 0;
        std::vector<std::string> firstErrors;          ///< up to 50, "<tile>: what"
    };

    /// Surveys one tile; @p mapFlags the map's MPHD flags (0x4 or 0x80: 4096-byte alpha maps).
    Era SurveyTile(const std::string& name, const TileFiles& files, uint32_t mapFlags, SurveyStats& stats);
}
