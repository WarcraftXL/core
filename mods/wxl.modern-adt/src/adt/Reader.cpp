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

    std::vector<SplitChunk> ReadSplitChunks(const Reader& file)
    {
        std::vector<SplitChunk> chunks;
        file.Chunks([&chunks](const Chunk& c) {
            if (c.id != kMCNK) return true;
            SplitChunk chunk;
            Reader(c.data, c.size).Chunks([&chunk](const Chunk& sub) {
                if (sub.id == kMCLY)
                    for (uint32_t at = 0; at + 16 <= sub.size; at += 16)
                    {
                        LayerEntry e;
                        std::memcpy(&e.texture, sub.data + at, 4);
                        std::memcpy(&e.flags, sub.data + at + 4, 4);
                        std::memcpy(&e.offset, sub.data + at + 8, 4);
                        chunk.layers.push_back(e);
                    }
                else if (sub.id == kMCAL)
                {
                    chunk.mcal     = sub.data;
                    chunk.mcalSize = sub.size;
                }
                return true;
            });
            chunks.push_back(std::move(chunk));
            return chunks.size() < 256;
        });
        return chunks;
    }

    bool ReadRootChunk(const Reader& file, uint32_t index, SplitChunk& out)
    {
        out = SplitChunk{};
        uint32_t seen = 0;
        bool     found = false;
        file.Chunks([&](const Chunk& c) {
            if (c.id != kMCNK) return true;
            if (seen++ != index) return true;
            found = true;
            // Offsets in the 128-byte header count from the MCNK's own header (8 bytes before its data).
            const uint8_t* base = c.data - 8;
            const size_t   total = static_cast<size_t>(c.size) + 8;
            if (c.size < 0x2C) return false;
            uint32_t ofsLayer = 0, ofsAlpha = 0, sizeAlpha = 0;
            std::memcpy(&ofsLayer, c.data + 0x1C, 4);
            std::memcpy(&ofsAlpha, c.data + 0x24, 4);
            std::memcpy(&sizeAlpha, c.data + 0x28, 4);
            if (ofsLayer && ofsLayer + 8 <= total)
            {
                uint32_t size = 0;
                std::memcpy(&size, base + ofsLayer + 4, 4);
                size = static_cast<uint32_t>(std::min<size_t>(size, total - ofsLayer - 8));
                for (uint32_t at = 0; at + 16 <= size; at += 16)
                {
                    LayerEntry e;
                    std::memcpy(&e.texture, base + ofsLayer + 8 + at, 4);
                    std::memcpy(&e.flags, base + ofsLayer + 8 + at + 4, 4);
                    std::memcpy(&e.offset, base + ofsLayer + 8 + at + 8, 4);
                    out.layers.push_back(e);
                }
            }
            // MCAL's own size is unreliable in root files; the header's sizeAlpha counts its 8-byte header.
            if (ofsAlpha && sizeAlpha > 8 && ofsAlpha + 8 <= total)
            {
                out.mcal     = base + ofsAlpha + 8;
                out.mcalSize = static_cast<uint32_t>(std::min<size_t>(sizeAlpha - 8, total - ofsAlpha - 8));
            }
            return false;
        });
        return found;
    }

    bool MapFlags(const Reader& wdt, uint32_t* flags)
    {
        const Chunk mphd = wdt.Find(kMPHD);
        if (!mphd || mphd.size < 4) return false;
        std::memcpy(flags, mphd.data, 4);
        return true;
    }

    bool DecodeAlpha(const SplitChunk& chunk, uint32_t layer, uint8_t* out, int big)
    {
        if (layer >= chunk.layers.size() || !(chunk.layers[layer].flags & 0x100) || !chunk.mcal) return false;
        const LayerEntry& l = chunk.layers[layer];
        if (l.offset >= chunk.mcalSize) return false;
        const uint8_t* p    = chunk.mcal + l.offset;
        const size_t   left = chunk.mcalSize - l.offset;
        constexpr size_t n  = kAlphaSide * kAlphaSide;
        if (l.flags & 0x200)
        {
            // Runs: a byte's top bit fills with the next byte, else copies the next (byte & 0x7F) bytes.
            size_t in = 0, done = 0;
            while (done < n && in < left)
            {
                const uint8_t head = p[in++];
                const size_t  count = std::min<size_t>(head & 0x7F, n - done);
                if (head & 0x80)
                {
                    if (in >= left) return false;
                    std::memset(out + done, p[in++], count);
                }
                else
                {
                    if (in + count > left) return false;
                    std::memcpy(out + done, p + in, count);
                    in += count;
                }
                done += count;
            }
            return done == n;
        }
        if (big != 0 && left >= n)
        {
            std::memcpy(out, p, n);
            return true;
        }
        if (big != 1 && left >= n / 2)
        {
            for (size_t i = 0; i < n / 2; ++i)
            {
                out[2 * i]     = static_cast<uint8_t>((p[i] & 0x0F) * 17);
                out[2 * i + 1] = static_cast<uint8_t>((p[i] >> 4) * 17);
            }
            return true;
        }
        return false;
    }
}
