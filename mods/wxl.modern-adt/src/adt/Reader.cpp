// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Reading an ADT's bytes (see Reader.hpp).

#include "adt/Reader.hpp"

#include <algorithm>
#include <cstring>

namespace wxl::modern_adt::adt
{
    Chunk Reader::At(size_t offset, uint32_t id) const
    {
        if (offset + 8 > size_ || offset + 8 < offset) return Chunk{};
        const uint32_t len = U32(offset + 4);
        if (U32(offset) != id || len > size_ - offset - 8) return Chunk{};
        return Chunk{id, data_ + offset + 8, len, static_cast<uint32_t>(offset)};
    }

    Chunk Reader::Find(uint32_t id) const
    {
        Chunk found;
        Chunks([&](const Chunk& c) {
            if (c.id != id) return true;
            found = c;
            return false;
        });
        return found;
    }

    Chunk Reader::Mhdr() const
    {
        // MVER first, MHDR right after it; a file laid out otherwise is searched.
        Chunk mver = At(0, kMVER);
        if (mver)
            if (Chunk mhdr = At(8 + mver.size, kMHDR)) return mhdr;
        return Find(kMHDR);
    }

    Chunk Reader::FromMhdr(MhdrSlot slot, uint32_t id) const
    {
        const Chunk mhdr = Mhdr();
        if (!mhdr || mhdr.size < slot + 4u) return Chunk{};
        uint32_t offset = 0;
        std::memcpy(&offset, mhdr.data + slot, sizeof offset);
        if (!offset) return Chunk{};
        // Relative to the start of MHDR's data, as the client reads them (CMapArea::Create).
        return At(static_cast<size_t>(mhdr.offset) + 8 + offset, id);
    }

    std::vector<std::string_view> Reader::TextureNames() const
    {
        std::vector<std::string_view> names;
        Chunk mtex = FromMhdr(kSlotMtex, kMTEX);
        if (!mtex) mtex = Find(kMTEX);
        const char* p   = reinterpret_cast<const char*>(mtex.data);
        const char* end = p + mtex.size;
        while (p && p < end)
        {
            const char* zero = static_cast<const char*>(std::memchr(p, 0, static_cast<size_t>(end - p)));
            if (!zero) break;
            if (zero > p) names.emplace_back(p, static_cast<size_t>(zero - p));
            p = zero + 1;
        }
        return names;
    }

    std::vector<TextureInfo> ReadTextures(const Reader& file, Prefer prefer)
    {
        Chunk mtxf = file.FromMhdr(kSlotMtxf, kMTXF);
        if (!mtxf) mtxf = file.Find(kMTXF);
        const Chunk mtxp = file.Find(kMTXP); // no MHDR slot points to it
        const size_t fromMtxf = mtxf.size / 4, fromMtxp = mtxp.size / 16;
        std::vector<TextureInfo> out(std::max({file.TextureNames().size(), fromMtxf, fromMtxp}));
        const bool useMtxp = prefer != Prefer::mtxf && mtxp;
        const bool useMtxf = prefer != Prefer::mtxp && mtxf && !useMtxp;
        for (size_t i = 0; i < out.size(); ++i)
        {
            TextureInfo& t = out[i];
            if (useMtxp && i < fromMtxp)
            {
                std::memcpy(&t.flags, mtxp.data + i * 16, 4);
                std::memcpy(&t.height_scale, mtxp.data + i * 16 + 4, 4);
                std::memcpy(&t.height_offset, mtxp.data + i * 16 + 8, 4);
                t.source = Source::mtxp;
            }
            else if (useMtxf && i < fromMtxf)
            {
                std::memcpy(&t.flags, mtxf.data + i * 4, 4);
                t.source = Source::mtxf;
            }
            else
                continue;
            t.scale = static_cast<uint8_t>((t.flags >> 4) & 0xF);
        }
        return out;
    }
}
