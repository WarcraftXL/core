// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A listfile: what file each FileDataID names (8.1 and later name textures and models by id), as
// a CSV of `id;path` lines (the layout wow.tools and the community listfile publish). Read once
// into a sorted index: 8 bytes an entry (id, offset of its name) and the names, searched by halves;
// the index is kept on disk beside the CSV's reader, by the CSV's hash, so a listfile of a million
// and a half lines is parsed once, not at every launch.
//
//   index file  "WXLF", version 1, the CSV's hash (FNV-1a 64), the count, the names' offset,
//               the entries sorted by id, the names (NUL-terminated, '\\' between folders)
//
// Portable: no runtime, no Windows.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wxl::modern_adt
{
    class Listfile
    {
    public:
        /// FNV-1a, 64 bits.
        static uint64_t Hash(const uint8_t* data, size_t size);

        /// The index of a CSV: its lines `id;path` (a header, blank lines and lines not of that
        /// shape skipped; an id named twice keeps its first name).
        static std::vector<uint8_t> BuildIndex(const uint8_t* csv, size_t size, uint64_t hash);

        /// Takes an index; false (and nothing taken) when it is not one, or not of @p hash.
        bool Use(std::vector<uint8_t> index, uint64_t hash);

        /// The name of @p id; false when the listfile has none.
        bool Name(uint32_t id, std::string& out) const;

        uint32_t Count() const { return count_; }
        bool     Empty() const { return count_ == 0; }

    private:
        std::vector<uint8_t> index_;
        uint32_t             count_ = 0, names_ = 0;
    };
}
