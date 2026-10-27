// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Reading an ADT's bytes: its top-level chunks walked, its MHDR's offsets followed, and what the
// features need taken out of them. Portable and bounds-checked: no runtime, no Windows; the
// native tests read made-up files with it.
//
//   Reader       one file (a root ADT now; a split file -- _tex0, _obj0 -- read the same way later):
//                its chunks in order, a chunk by id, the MHDR and the chunks it points to
//   TextureInfo  what the file says of each MTEX texture: its MTXF flags and its MTXP entry
//
// Chunk ids are compared as the file stores them, read little-endian: "MTXP" is the bytes
// "PXTM", which read as a uint32 give 'MTXP'.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace wxl::modern_adt::adt
{
    constexpr uint32_t Id(const char (&four)[5])
    {
        return static_cast<uint32_t>(static_cast<uint8_t>(four[0])) << 24 | static_cast<uint32_t>(static_cast<uint8_t>(four[1])) << 16 |
               static_cast<uint32_t>(static_cast<uint8_t>(four[2])) << 8 | static_cast<uint32_t>(static_cast<uint8_t>(four[3]));
    }

    constexpr uint32_t kMVER = Id("MVER"), kMHDR = Id("MHDR"), kMTEX = Id("MTEX"), kMTXF = Id("MTXF"), kMTXP = Id("MTXP");

    struct Chunk
    {
        uint32_t       id = 0;
        const uint8_t* data = nullptr;
        uint32_t       size = 0;
        uint32_t       offset = 0; ///< of its header, in the file
        explicit operator bool() const { return data != nullptr; }
    };

    /// The MHDR's offsets (relative to the start of its data), as 3.3.5a and later write them.
    enum MhdrSlot : uint32_t
    {
        kSlotFlags = 0x00, kSlotMcin = 0x04, kSlotMtex = 0x08, kSlotMmdx = 0x0C, kSlotMmid = 0x10, kSlotMwmo = 0x14,
        kSlotMwid = 0x18, kSlotMddf = 0x1C, kSlotModf = 0x20, kSlotMfbo = 0x24, kSlotMh2o = 0x28, kSlotMtxf = 0x2C,
    };

    class Reader
    {
    public:
        Reader(const void* data, size_t size) : data_(static_cast<const uint8_t*>(data)), size_(data ? size : 0) {}

        /// The top-level chunks in order; @p visit answers false to stop.
        template <class F>
        void Chunks(F&& visit) const
        {
            for (size_t at = 0; at + 8 <= size_;)
            {
                const uint32_t id = U32(at), len = U32(at + 4);
                if (len > size_ - at - 8) return; // cut short: what follows is not read
                if (!visit(Chunk{id, data_ + at + 8, len, static_cast<uint32_t>(at)})) return;
                at += 8 + static_cast<size_t>(len);
            }
        }

        /// The first top-level chunk of @p id.
        Chunk Find(uint32_t id) const;

        /// The MHDR chunk, the second of a root ADT (after MVER).
        Chunk Mhdr() const;

        /// The chunk an MHDR slot points to; none for a slot of 0, past the file, or not of @p id.
        Chunk FromMhdr(MhdrSlot slot, uint32_t id) const;

        /// The MTEX names, in order.
        std::vector<std::string_view> TextureNames() const;

    private:
        uint32_t U32(size_t at) const
        {
            return static_cast<uint32_t>(data_[at]) | static_cast<uint32_t>(data_[at + 1]) << 8 | static_cast<uint32_t>(data_[at + 2]) << 16 |
                   static_cast<uint32_t>(data_[at + 3]) << 24;
        }
        Chunk At(size_t offset, uint32_t id) const;

        const uint8_t* data_;
        size_t         size_;
    };

    /// Where a texture's scale came from.
    enum class Source : uint8_t
    {
        none, ///< the file says nothing of it
        mtxf, ///< MTXF's flags (bits 4 to 7)
        mtxp, ///< MTXP's entry
    };

    /// What a file says of one MTEX texture.
    struct TextureInfo
    {
        uint32_t flags = 0;          ///< MTXF's or MTXP's flags, as the source has them
        uint8_t  scale = 0;          ///< (flags >> 4) & 0xF: the texture drawn 2^scale times larger
        float    height_scale = 0.0f;  ///< MTXP: for the height blend; 0 without MTXP
        float    height_offset = 1.0f; ///< MTXP
        Source   source = Source::none;
    };

    /// Which chunk a file's scales are taken from.
    enum class Prefer : uint8_t
    {
        automatic, ///< MTXP when the file has it, MTXF otherwise (retail's order: MTXP is the later chunk)
        mtxp,      ///< MTXP only
        mtxf,      ///< MTXF only
    };

    /// Every MTEX texture's info, from the file's MTXF and MTXP; as many entries as MTEX names (at
    /// least as many as either chunk has).
    std::vector<TextureInfo> ReadTextures(const Reader& file, Prefer prefer);

    /// 2^scale, the texture_scale wxl.terrain takes.
    inline float ScaleOf(uint8_t scale) { return static_cast<float>(1u << (scale & 0xF)); }
}
