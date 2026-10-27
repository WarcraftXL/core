// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Placements of models the client of 3.3.5a cannot load, left out before it makes them. Retail
// data names models this client never read: an M2 in Legion's chunked form (magic MD21), an M2
// of a later version than its 264 (MD20, Cataclysm's 265 and on), a map object root (WMO) of a
// later version than its 17. A tile's placement of one is not handed to the client
// (MapChunk.CreateRefs, src/services/TerrainTiles.cpp); each such file is logged once.
//
// A file is read once, its first 12 bytes, through the client's own file layer (so a file a mod
// serves is judged as served); what was found is kept by name until the client shuts down.
// Main thread only.

#pragma once

#include <cstddef>
#include <cstdint>

namespace wxl::core::services::model_gate
{
    enum class Verdict : uint8_t
    {
        loadable, ///< what the client reads, or a file it judges itself (absent, unknown)
        chunked,  ///< an M2 of magic MD21
        newer,    ///< an M2 of a version past 264, or a WMO root past 17
    };

    /// What a file's first bytes say. @p mapObject: a WMO root (MVER first), else an M2.
    Verdict Judge(const uint8_t* head, size_t size, bool mapObject);

    /// Whether the client can load the model @p name names, as a placement names it (".mdx" and
    /// ".mdl" read as ".m2", as the client does). Logs a file it cannot, once.
    bool Loadable(const char* name, bool mapObject);

    /// Placements left out so far.
    uint64_t LeftOut();
    void     CountLeftOut(uint32_t count);

    /// The client shuts down.
    void Clear();
}
