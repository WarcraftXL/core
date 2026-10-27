// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A tile split by file name, indexed (see SplitTile.hpp).

#include "services/SplitTile.hpp"

#include <algorithm>
#include <cstring>

namespace wxl::core::services::split_tile
{
    namespace
    {
        constexpr uint32_t Id(const char (&four)[5])
        {
            return static_cast<uint32_t>(static_cast<uint8_t>(four[0])) << 24 | static_cast<uint32_t>(static_cast<uint8_t>(four[1])) << 16 |
                   static_cast<uint32_t>(static_cast<uint8_t>(four[2])) << 8 | static_cast<uint32_t>(static_cast<uint8_t>(four[3]));
        }

        uint32_t U32(const uint8_t* at)
        {
            uint32_t v;
            std::memcpy(&v, at, sizeof v);
            return v;
        }

        uint16_t U16(const uint8_t* at)
        {
            uint16_t v;
            std::memcpy(&v, at, sizeof v);
            return v;
        }

        /// Walks the chunks of [data, data + size); @p visit(id, payload, size, offset of the header)
        /// answers false to stop. Answers false when a chunk runs past the end.
        template <class F>
        bool Walk(uint8_t* data, uint32_t size, F&& visit)
        {
            uint32_t at = 0;
            while (at + 8 <= size)
            {
                const uint32_t id = U32(data + at), len = U32(data + at + 4);
                if (len > size - at - 8) return false;
                if (!visit(id, data + at + 8, len, at)) return true;
                at += 8 + len;
            }
            return at == size;
        }

        /// The top-level chunks of a split file (_tex0, _obj0): the named ones kept, MCNKs in order.
        struct SplitFile
        {
            std::vector<Span> mcnk;
        };

        bool ReadSplit(uint8_t* data, uint32_t size, Tile& tile, bool tex, SplitFile& out)
        {
            return Walk(data, size, [&](uint32_t id, uint8_t* payload, uint32_t len, uint32_t) {
                const Span span{payload, len};
                if (id == Id("MCNK")) out.mcnk.push_back(span);
                else if (tex && id == Id("MTEX")) tile.mtex = span;
                else if (tex && id == Id("MTXF")) tile.mtxf = span;
                else if (tex && id == Id("MTXP")) tile.mtxp = span;
                else if (tex && id == Id("MAMP")) tile.mamp = span;
                else if (tex && id == Id("MDID")) tile.mdid = span;
                else if (tex && id == Id("MHID")) tile.mhid = span;
                else if (!tex && id == Id("MMDX")) tile.mmdx = span;
                else if (!tex && id == Id("MMID")) tile.mmid = span;
                else if (!tex && id == Id("MWMO")) tile.mwmo = span;
                else if (!tex && id == Id("MWID")) tile.mwid = span;
                else if (!tex && id == Id("MDDF")) tile.mddf = span;
                else if (!tex && id == Id("MODF")) tile.modf = span;
                return true;
            });
        }

        uint32_t CountNames(const Span& names)
        {
            uint32_t count = 0;
            for (uint32_t at = 0; at < names.size;)
            {
                const void* end = std::memchr(names.data + at, 0, names.size - at);
                if (!end) return count; // a name the table does not end: the client would stop there too
                ++count;
                at = static_cast<uint32_t>(static_cast<const uint8_t*>(end) - names.data) + 1;
            }
            return count;
        }

        /// Whether the name a placement's nameId leads to (through MMID or MWID) lies in its table.
        bool NameInTable(const Span& offsets, const Span& names, uint32_t nameId)
        {
            if (static_cast<uint64_t>(nameId) * 4 + 4 > offsets.size) return false;
            const uint32_t at = U32(offsets.data + 4 * nameId);
            return at < names.size && std::memchr(names.data + at, 0, names.size - at) != nullptr;
        }

        bool UsableRefs(const Span& refs, const Span& defs, uint32_t defSize, uint32_t flagsAt, uint16_t byFileId,
                        const Span& offsets, const Span& names, std::vector<uint32_t>& kept)
        {
            kept.clear();
            const uint32_t count = refs.size / 4, defCount = defs.size / defSize;
            bool           all   = true;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t ref = U32(refs.data + 4 * i);
                bool           ok  = ref < defCount;
                if (ok)
                {
                    const uint8_t* def = defs.data + static_cast<size_t>(ref) * defSize;
                    ok = !(U16(def + flagsAt) & byFileId) && NameInTable(offsets, names, U32(def));
                }
                if (ok) kept.push_back(ref);
                else all = false;
            }
            if (all) kept.clear();
            return all;
        }
    }

    bool IsSplitRoot(const uint8_t* root, uint32_t size)
    {
        if (!root || size < 8 + 4 + 8 + 0x40 + 8) return false;
        if (U32(root) != Id("MVER")) return false;
        const uint32_t mhdr = 8 + U32(root + 4);
        if (mhdr > size - 8 - 0x40 || U32(root + mhdr) != Id("MHDR") || U32(root + mhdr + 4) < 0x40) return false;
        if (U32(root + mhdr + 8 + kMhdrMcin) != 0) return false;
        const uint32_t next = mhdr + 8 + U32(root + mhdr + 4);
        // MCNKs follow (MH2O, MFBO may come first or after: any top-level MCNK will do).
        for (uint32_t at = next; at + 8 <= size;)
        {
            const uint32_t id = U32(root + at), len = U32(root + at + 4);
            if (id == Id("MCNK")) return true;
            if (len > size - at - 8) return false;
            at += 8 + len;
        }
        return false;
    }

    bool Index(uint8_t* root, uint32_t rootSize, uint8_t* tex0, uint32_t tex0Size, uint8_t* obj0, uint32_t obj0Size, Tile& out,
               std::string& why)
    {
        out = Tile{};
        why.clear();
        if (!IsSplitRoot(root, rootSize))
        {
            why = "not a root without MCIN";
            return false;
        }
        uint32_t chunks = 0;
        bool     shaped = true;
        const bool whole = Walk(root, rootSize, [&](uint32_t id, uint8_t*, uint32_t len, uint32_t at) {
            if (id != Id("MCNK")) return true;
            if (chunks >= 256 || len < kHeaderSize)
            {
                shaped = false;
                return false;
            }
            out.chunkOffset[chunks] = at;
            out.chunkSize[chunks]   = 8 + len;
            ++chunks;
            return true;
        });
        if (!whole || !shaped || chunks != 256)
        {
            why = "a root without 256 whole MCNKs";
            return false;
        }

        if (tex0 && tex0Size)
        {
            Tile      parts;
            SplitFile file;
            if (!ReadSplit(tex0, tex0Size, parts, true, file) || file.mcnk.size() != 256)
                why += "_tex0 has not 256 whole MCNKs; ";
            else
            {
                out.mtex = parts.mtex;
                out.mtxf = parts.mtxf;
                out.mtxp = parts.mtxp;
                out.mamp = parts.mamp;
                out.mdid = parts.mdid;
                out.mhid = parts.mhid;
                out.textures = CountNames(out.mtex);
                for (uint32_t i = 0; i < 256; ++i)
                {
                    ChunkParts& p = out.parts[i];
                    if (!Walk(file.mcnk[i].data, file.mcnk[i].size, [&](uint32_t id, uint8_t* payload, uint32_t len, uint32_t) {
                            if (id == Id("MCLY")) p.mcly = Span{payload, len};
                            else if (id == Id("MCAL")) p.mcal = Span{payload, len};
                            else if (id == Id("MCSH")) p.mcsh = Span{payload, len};
                            return true;
                        }))
                        p.mcly = p.mcal = p.mcsh = Span{};
                }
            }
        }

        if (obj0 && obj0Size)
        {
            Tile      parts;
            SplitFile file;
            if (!ReadSplit(obj0, obj0Size, parts, false, file) || (file.mcnk.size() != 256 && !file.mcnk.empty()))
                why += "_obj0 has not 256 whole MCNKs; ";
            else
            {
                out.mmdx = parts.mmdx;
                out.mmid = parts.mmid;
                out.mwmo = parts.mwmo;
                out.mwid = parts.mwid;
                out.mddf = parts.mddf;
                out.modf = parts.modf;
                for (uint32_t i = 0; i < file.mcnk.size(); ++i)
                {
                    ChunkParts& p = out.parts[i];
                    if (!Walk(file.mcnk[i].data, file.mcnk[i].size, [&](uint32_t id, uint8_t* payload, uint32_t len, uint32_t) {
                            if (id == Id("MCRD")) p.mcrd = Span{payload, len};
                            else if (id == Id("MCRW")) p.mcrw = Span{payload, len};
                            return true;
                        }))
                        p.mcrd = p.mcrw = Span{};
                }
            }
        }
        return true;
    }

    uint32_t UsableLayers(const Tile& tile, uint32_t chunk)
    {
        if (chunk >= 256) return 0;
        const ChunkParts& p     = tile.parts[chunk];
        const uint32_t    count = p.mcly.size / 16;
        uint32_t          k     = 0;
        for (; k < count && k < 4; ++k)
        {
            const uint8_t* layer = p.mcly.data + 16 * k;
            if (U32(layer) >= tile.textures || (U32(layer) < tile.missing.size() && tile.missing[U32(layer)])) break;
            // An alpha map (0x100) must start inside MCAL; how much it reads the client decides by its
            // own flags (the runtime's copy of _tex0 is padded past its end, so a map cut short reads zeros).
            if ((U32(layer + 4) & 0x100) && U32(layer + 8) >= p.mcal.size) break;
        }
        return k;
    }

    bool NeedsPlaceholder(const Tile& tile)
    {
        for (uint32_t i = 0; i < 256; ++i)
            if (!UsableLayers(tile, i)) return true;
        return false;
    }

    ChunkLayers PlanLayers(const Tile& tile, uint32_t chunk, uint32_t placeholder)
    {
        ChunkLayers out;
        if (chunk >= 256) return out;
        const ChunkParts& p     = tile.parts[chunk];
        const uint32_t    usable = UsableLayers(tile, chunk);
        if (usable && p.mcly.size / 16 >= 4)
        {
            out.count = usable;
            return out; // the file's MCLY: four entries the client can read
        }
        out.own = true;
        if (usable)
        {
            std::memcpy(out.table, p.mcly.data, 16 * usable);
            out.count = usable;
        }
        else
        {
            std::memcpy(out.table, &placeholder, 4);
            out.count = 1;
        }
        for (uint32_t k = out.count; k < 4; ++k)
        {
            std::memcpy(out.table + 16 * k, out.table, 4); // the first's texture; no flags, alpha or effect
        }
        return out;
    }

    LiquidFixes FixLiquids(uint8_t* root, uint32_t size, const std::vector<uint8_t>& known)
    {
        LiquidFixes fixes;
        if (!IsSplitRoot(root, size)) return fixes;
        const uint32_t mhdr = 8 + U32(root + 4) + 8;
        const uint32_t slot = U32(root + mhdr + 0x28);
        if (!slot || static_cast<uint64_t>(mhdr) + slot + 8 > size) return fixes;
        uint8_t*       chunk = root + mhdr + slot;
        if (U32(chunk) != Id("MH2O")) return fixes;
        const uint32_t len = std::min<uint32_t>(U32(chunk + 4), size - (mhdr + slot + 8));
        uint8_t*       data = chunk + 8;
        if (len < 256 * 12) return fixes;

        // Every offset MH2O holds, to bound each instance's vertex data by the next one.
        std::vector<uint32_t> offsets{len};
        for (uint32_t c = 0; c < 256; ++c)
        {
            const uint32_t oi = U32(data + 12 * c), n = U32(data + 12 * c + 4), oa = U32(data + 12 * c + 8);
            if (oa) offsets.push_back(oa);
            if (!oi || static_cast<uint64_t>(oi) + 24ull * n > len) continue;
            offsets.push_back(oi);
            for (uint32_t k = 0; k < n; ++k)
            {
                const uint8_t* ins = data + oi + 24 * k;
                if (U32(ins + 16)) offsets.push_back(U32(ins + 16));
                if (U32(ins + 20)) offsets.push_back(U32(ins + 20));
            }
        }
        std::sort(offsets.begin(), offsets.end());

        for (uint32_t c = 0; c < 256; ++c)
        {
            uint8_t*       head = data + 12 * c;
            const uint32_t oi = U32(head), n = U32(head + 4), oa = U32(head + 8);
            if (!n) continue;
            bool ok = oi && static_cast<uint64_t>(oi) + 24ull * n <= len && (!oa || static_cast<uint64_t>(oa) + 16 <= len);
            for (uint32_t k = 0; ok && k < n; ++k)
            {
                uint8_t*       ins = data + oi + 24 * k;
                const uint32_t x = ins[12], y = ins[13], w = ins[14], h = ins[15];
                const uint32_t bitmap = U32(ins + 16), vertex = U32(ins + 20);
                if (!w || !h || x + w > 8 || y + h > 8 || (bitmap && static_cast<uint64_t>(bitmap) + (w * h + 7) / 8 > len))
                {
                    ok = false;
                    break;
                }
                ++fixes.instances;
                const uint32_t vertices = (w + 1) * (h + 1);
                uint16_t       format   = U16(ins + 2);
                const uint16_t before   = format;
                uint32_t       gap      = 0;
                if (vertex)
                {
                    const auto next = std::upper_bound(offsets.begin(), offsets.end(), vertex);
                    gap = (next == offsets.end() ? len : *next) - vertex;
                }
                if (format > 3)
                    format = !vertex ? 2 : gap >= 8 * vertices ? 1 : gap >= 5 * vertices ? 0 : gap >= vertices ? 2 : 2;
                if (format == 3) format = 1;
                // The vertex data the format reads must lie in MH2O; else none (flat, at the instance's heights).
                const uint32_t need = format == 0 ? 5 * vertices : format == 1 ? 8 * vertices : vertices;
                if (vertex && static_cast<uint64_t>(vertex) + need > len)
                {
                    const uint32_t none = 0;
                    std::memcpy(ins + 20, &none, 4);
                    format = 2;
                }
                if (format != before)
                {
                    std::memcpy(ins + 2, &format, 2);
                    ++fixes.formats;
                }
                const uint16_t type = U16(ins);
                if (type >= known.size() || !known[type])
                {
                    const uint16_t safe = format == 2 ? 2 : 1;
                    std::memcpy(ins, &safe, 2);
                    ++fixes.types;
                }
            }
            if (!ok)
            {
                const uint32_t none = 0;
                std::memcpy(head + 4, &none, 4);
                ++fixes.dropped;
            }
        }
        return fixes;
    }

    bool UsableDoodadRefs(const Tile& tile, uint32_t chunk, std::vector<uint32_t>& kept)
    {
        if (chunk >= 256) return true;
        return UsableRefs(tile.parts[chunk].mcrd, tile.mddf, kDoodadDefSize, 34, kDoodadByFileId, tile.mmid, tile.mmdx, kept);
    }

    bool UsableMapObjRefs(const Tile& tile, uint32_t chunk, std::vector<uint32_t>& kept)
    {
        if (chunk >= 256) return true;
        return UsableRefs(tile.parts[chunk].mcrw, tile.modf, kMapObjDefSize, 56, kMapObjByFileId, tile.mwid, tile.mwmo, kept);
    }

    void NameByIndex(Span defs, uint32_t defSize, uint32_t index, uint32_t nameId)
    {
        if (static_cast<uint64_t>(index + 1) * defSize > defs.size) return;
        uint8_t* def = defs.data + static_cast<size_t>(index) * defSize;
        std::memcpy(def, &nameId, 4);
        const uint32_t flagsAt = defSize == kDoodadDefSize ? 34 : 56;
        const uint16_t byId    = defSize == kDoodadDefSize ? kDoodadByFileId : kMapObjByFileId;
        const uint16_t flags   = static_cast<uint16_t>(U16(def + flagsAt) & ~byId);
        std::memcpy(def + flagsAt, &flags, 2);
    }

    std::vector<int32_t> FlagsFromMtxp(const Tile& tile)
    {
        std::vector<int32_t> flags(tile.textures, 0);
        for (uint32_t i = 0; i < tile.textures && 16 * i + 16 <= tile.mtxp.size; ++i)
            flags[i] = static_cast<int32_t>(U32(tile.mtxp.data + 16 * i));
        return flags;
    }

    uint16_t FoldHoles(const uint8_t high[8])
    {
        uint16_t low = 0;
        for (uint32_t row = 0; row < 4; ++row)
            for (uint32_t col = 0; col < 4; ++col)
            {
                const uint32_t pair = (high[2 * row] | high[2 * row + 1]) >> (2 * col);
                if (pair & 3) low = static_cast<uint16_t>(low | (1u << (4 * row + col)));
            }
        return low;
    }
}
