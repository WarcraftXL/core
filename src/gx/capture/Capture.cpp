// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A frame capture read back.

#include "gx/capture/Capture.hpp"

#include <cstdio>
#include <cstring>

namespace wxl::gx::capture
{
    bool Read32(const Chunk& chunk, uint32_t at, uint32_t& out)
    {
        if (static_cast<uint64_t>(at) + 4 > chunk.size) return false;
        std::memcpy(&out, chunk.body + at, 4);
        return true;
    }

    bool Read64(const Chunk& chunk, uint32_t at, uint64_t& out)
    {
        if (static_cast<uint64_t>(at) + 8 > chunk.size) return false;
        std::memcpy(&out, chunk.body + at, 8);
        return true;
    }

    bool Reader::Open(const uint8_t* data, size_t size, std::string& why)
    {
        data_   = data;
        size_   = size;
        at_     = 0;
        deltas_ = 0;
        for (auto& region : regions_) region.clear();
        if (size < sizeof(wxl_gx_capture_header))
        {
            why = "shorter than a capture's header";
            return false;
        }
        std::memcpy(&header_, data, sizeof header_);
        if (header_.magic != WXL_GX_CAPTURE_MAGIC)
        {
            why = "not a frame capture (no XCAP at its start)";
            return false;
        }
        if (header_.version != WXL_GX_CAPTURE_VERSION)
        {
            char text[96];
            std::snprintf(text, sizeof text, "a capture of version %u; this reader reads version %u", header_.version,
                          WXL_GX_CAPTURE_VERSION);
            why = text;
            return false;
        }
        if (header_.header_size < sizeof(wxl_gx_capture_header) || header_.header_size > size)
        {
            why = "a header whose size does not fit the file";
            return false;
        }
        at_ = header_.header_size;
        return true;
    }

    const std::vector<uint8_t>& Reader::Region(wxl_gx_capture_region region) const
    {
        static const std::vector<uint8_t> kNone;
        const unsigned index = static_cast<unsigned>(region);
        return index < 5 ? regions_[index] : kNone;
    }

    bool Reader::Apply(const Chunk& chunk, std::string& why)
    {
        uint32_t region = 0, total = 0, runs = 0;
        if (!Read32(chunk, 0, region) || !Read32(chunk, 4, total) || !Read32(chunk, 8, runs) || region == 0 || region > 4)
        {
            why = "a MEM chunk without a known region";
            return false;
        }
        std::vector<uint8_t>& bytes = regions_[region];
        if (bytes.size() != total) bytes.assign(total, 0);
        uint32_t at = 12;
        for (uint32_t i = 0; i < runs; ++i)
        {
            uint32_t offset = 0, length = 0;
            if (!Read32(chunk, at, offset) || !Read32(chunk, at + 4, length) || static_cast<uint64_t>(at) + 8 + length > chunk.size ||
                static_cast<uint64_t>(offset) + length > total)
            {
                why = "a MEM run outside its chunk or its region";
                return false;
            }
            std::memcpy(bytes.data() + offset, chunk.body + at + 8, length);
            at += 8 + length;
        }
        ++deltas_;
        return true;
    }

    bool Reader::Next(Chunk& out, std::string& why)
    {
        why.clear();
        while (at_ < size_)
        {
            if (size_ - at_ < sizeof(wxl_gx_capture_chunk))
            {
                why = "the file ends inside a chunk's header";
                return false;
            }
            wxl_gx_capture_chunk head{};
            std::memcpy(&head, data_ + at_, sizeof head);
            const size_t body = at_ + sizeof head;
            if (head.size > size_ - body)
            {
                why = "the file ends inside a chunk";
                return false;
            }
            Chunk chunk{head.id, data_ + body, head.size};
            at_ = body + ((static_cast<size_t>(head.size) + 3) & ~static_cast<size_t>(3));
            if (head.id == WXL_GX_CAPTURE_MEM)
            {
                if (!Apply(chunk, why)) return false;
                continue;
            }
            out = chunk;
            return true;
        }
        return false;
    }
}
