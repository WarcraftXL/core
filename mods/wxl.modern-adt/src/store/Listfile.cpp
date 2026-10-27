// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A listfile and its index (see Listfile.hpp).

#include "store/Listfile.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace wxl::modern_adt
{
    namespace
    {
        constexpr uint32_t kMagic = 0x464C5857; // "WXLF"
        constexpr uint32_t kVersion = 1;
        constexpr size_t   kHeader = 24;       // magic, version, hash (8), count, names offset

        uint32_t U32(const uint8_t* at)
        {
            uint32_t v;
            std::memcpy(&v, at, 4);
            return v;
        }

        void Put32(std::vector<uint8_t>& out, uint32_t v)
        {
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
    }

    uint64_t Listfile::Hash(const uint8_t* data, size_t size)
    {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i)
        {
            h ^= data[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    std::vector<uint8_t> Listfile::BuildIndex(const uint8_t* csv, size_t size, uint64_t hash)
    {
        struct Entry
        {
            uint32_t id;
            uint32_t at;  ///< the name's start in the CSV
            uint32_t len;
        };
        std::vector<Entry> entries;
        for (size_t line = 0; line < size;)
        {
            const uint8_t* end  = static_cast<const uint8_t*>(std::memchr(csv + line, '\n', size - line));
            const size_t   stop = end ? static_cast<size_t>(end - csv) : size;
            size_t         at   = line;
            uint64_t       id   = 0;
            while (at < stop && csv[at] >= '0' && csv[at] <= '9' && id <= 0xFFFFFFFFull) id = id * 10 + (csv[at++] - '0');
            if (at > line && at < stop && csv[at] == ';' && id && id <= 0xFFFFFFFFull)
            {
                size_t from = at + 1, to = stop;
                while (to > from && (csv[to - 1] == '\r' || csv[to - 1] == ' ')) --to;
                if (to > from) entries.push_back(Entry{static_cast<uint32_t>(id), static_cast<uint32_t>(from), static_cast<uint32_t>(to - from)});
            }
            line = stop + 1;
        }
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.id < b.id; });
        entries.erase(std::unique(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.id == b.id; }), entries.end());

        std::vector<uint8_t> out;
        out.reserve(kHeader + entries.size() * 8);
        Put32(out, kMagic);
        Put32(out, kVersion);
        Put32(out, static_cast<uint32_t>(hash));
        Put32(out, static_cast<uint32_t>(hash >> 32));
        Put32(out, static_cast<uint32_t>(entries.size()));
        Put32(out, static_cast<uint32_t>(kHeader + entries.size() * 8));
        uint32_t name = 0;
        for (const Entry& e : entries)
        {
            Put32(out, e.id);
            Put32(out, name);
            name += e.len + 1;
        }
        for (const Entry& e : entries)
        {
            for (uint32_t i = 0; i < e.len; ++i)
            {
                const uint8_t c = csv[e.at + i];
                out.push_back(c == '/' ? static_cast<uint8_t>('\\') : c);
            }
            out.push_back(0);
        }
        return out;
    }

    bool Listfile::Use(std::vector<uint8_t> index, uint64_t hash)
    {
        if (index.size() < kHeader || U32(index.data()) != kMagic || U32(index.data() + 4) != kVersion) return false;
        const uint64_t theirs = static_cast<uint64_t>(U32(index.data() + 8)) | static_cast<uint64_t>(U32(index.data() + 12)) << 32;
        const uint32_t count = U32(index.data() + 16), names = U32(index.data() + 20);
        if (theirs != hash || names != kHeader + static_cast<uint64_t>(count) * 8 || names > index.size() ||
            (count && index.back() != 0))
            return false;
        index_ = std::move(index);
        count_ = count;
        names_ = names;
        return true;
    }

    bool Listfile::Name(uint32_t id, std::string& out) const
    {
        uint32_t lo = 0, hi = count_;
        while (lo < hi)
        {
            const uint32_t mid = lo + (hi - lo) / 2;
            const uint32_t at  = U32(index_.data() + kHeader + 8 * static_cast<size_t>(mid));
            if (at < id) lo = mid + 1;
            else hi = mid;
        }
        if (lo >= count_ || U32(index_.data() + kHeader + 8 * static_cast<size_t>(lo)) != id) return false;
        const size_t name = names_ + static_cast<size_t>(U32(index_.data() + kHeader + 8 * static_cast<size_t>(lo) + 4));
        if (name >= index_.size()) return false;
        out.assign(reinterpret_cast<const char*>(index_.data() + name));
        return true;
    }
}
