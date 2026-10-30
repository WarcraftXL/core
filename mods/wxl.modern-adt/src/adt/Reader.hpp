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
    constexpr uint32_t kMCNK = Id("MCNK"), kMCLY = Id("MCLY"), kMCAL = Id("MCAL"), kMPHD = Id("MPHD");

    /// MPHD's flags (a WDT), as retail names them.
    enum MapFlag : uint32_t
    {
        kMapGlobalObject = 0x1,     ///< wdt_uses_global_map_obj
        kMapVertexColors = 0x2,     ///< adt_has_mccv
        kMapBigAlpha = 0x4,         ///< adt_has_big_alpha: 4096-byte alpha maps
        kMapSortedDoodads = 0x8,    ///< adt_has_doodadrefs_sorted_by_size_cat
        kMapLightVertices = 0x10,   ///< MCLV (Cataclysm and later)
        kMapHeightTexturing = 0x80, ///< adt_has_height_texturing: the height blend, and 4096-byte alpha maps
        kMapLod = 0x100,            ///< _lod.adt (Legion and later)
        kMapFileIds = 0x200,        ///< MAID (8.1 and later)
    };


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

    /// A layer of a chunk, as MCLY has it.
    struct LayerEntry
    {
        uint32_t texture = 0; ///< its MTEX index
        uint32_t flags = 0;   ///< 0x100 has an alpha map, 0x200 compressed
        uint32_t offset = 0;  ///< of its alpha map in MCAL
    };

    /// A chunk of a split texture file (_tex0.adt): its layers and its alpha maps.
    struct SplitChunk
    {
        std::vector<LayerEntry> layers;
        const uint8_t*          mcal = nullptr;
        uint32_t                mcalSize = 0;
    };

    /// The MCNKs of a split texture file, in file order (chunk x + 16 * chunk y): no header, their
    /// sub-chunks from their start, as retail writes them. Fewer than 256 when the file is cut short.
    std::vector<SplitChunk> ReadSplitChunks(const Reader& file);

    /// Chunk @p index (x + 16 * y) of a root ADT, its layers and alpha maps as the split form holds
    /// them: MCLY counted by its own size (the MCNK header's count may have been cut to the
    /// client's four), MCAL from the header's offset and size. False past the file's chunks.
    bool ReadRootChunk(const Reader& file, uint32_t index, SplitChunk& out);

    /// A WDT's MPHD flags; false for a file without MPHD.
    bool MapFlags(const Reader& wdt, uint32_t* flags);

    /// The side of an alpha map.
    constexpr uint32_t kAlphaSide = 64;

    /// Layer @p layer's alpha map (64 x 64, row by row from the north-west) from its chunk's MCAL:
    /// run-length coded when its flags say 0x200, else 4096 bytes, else 2048 of four bits (low
    /// nibble first). False for a layer without one, or one running past MCAL.
    /// @p big: the map reads uncompressed maps as 4096 bytes (MPHD 0x4 or 0x80), 2048 of four bits
    /// otherwise; unknown (-1): 4096 when MCAL holds that many past the map.
    bool DecodeAlpha(const SplitChunk& chunk, uint32_t layer, uint8_t* out, int big = -1);

    /// 2^scale, the texture_scale wxl.terrain takes.
    inline float ScaleOf(uint8_t scale) { return static_cast<float>(1u << (scale & 0xF)); }
}
