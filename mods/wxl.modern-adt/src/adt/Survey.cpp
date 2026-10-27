// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A tile read as later clients write it (see Survey.hpp).

#include "adt/Survey.hpp"

#include "adt/Reader.hpp"

#include <cstring>
#include <set>

namespace wxl::modern_adt::adt
{
    namespace
    {
        std::string IdText(uint32_t id)
        {
            std::string s(4, ' ');
            for (int i = 0; i < 4; ++i) s[i] = static_cast<char>((id >> (24 - 8 * i)) & 0xFF);
            return s;
        }

        const std::set<uint32_t>& KnownTop()
        {
            static const std::set<uint32_t> known = [] {
                std::set<uint32_t> k;
                for (const char* n : {"MVER", "MHDR", "MCIN", "MTEX", "MMDX", "MMID", "MWMO", "MWID", "MDDF", "MODF", "MH2O", "MCNK", "MFBO",
                                      "MTXF", "MTXP", "MAMP", "MDID", "MHID", "MBMH", "MBBB", "MBNV", "MBMI", "MLHD", "MLVH", "MLVI", "MLLL",
                                      "MLND", "MLSI", "MLLD", "MLLN", "MLLI", "MLLV", "MLMD", "MLMX", "MLDD", "MLDX", "MLDL", "MLFD", "MWDR",
                                      "MWDS", "MLMB", "MLDB", "MNID", "MSID", "MLID", "MLWS"})
                    k.insert(Id(*reinterpret_cast<const char(*)[5]>(n)));
                return k;
            }();
            return known;
        }

        const std::set<uint32_t>& KnownSub()
        {
            static const std::set<uint32_t> known = [] {
                std::set<uint32_t> k;
                for (const char* n : {"MCVT", "MCNR", "MCLY", "MCRF", "MCRD", "MCRW", "MCSH", "MCAL", "MCLQ", "MCSE", "MCCV", "MCLV", "MCBB",
                                      "MCMT", "MCDD"})
                    k.insert(Id(*reinterpret_cast<const char(*)[5]>(n)));
                return k;
            }();
            return known;
        }

        uint32_t U32(const uint8_t* p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }

        struct Ctx
        {
            const std::string& tile;
            SurveyStats&       stats;
            void Error(const std::string& what)
            {
                ++stats.errors;
                if (stats.firstErrors.size() < 50) stats.firstErrors.push_back(tile + ": " + what);
            }
        };

        /// The top-level chunks of a file, unknown ones counted; false when one runs past the end.
        bool Walk(const std::vector<uint8_t>& file, const char* kind, Ctx& ctx, std::vector<Chunk>& out)
        {
            size_t at = 0;
            while (at + 8 <= file.size())
            {
                const uint32_t id = U32(&file[at]), size = U32(&file[at + 4]);
                if (size > file.size() - at - 8)
                {
                    ctx.Error(std::string(kind) + ": " + IdText(id) + " runs past the end");
                    return false;
                }
                out.push_back(Chunk{id, &file[at + 8], size, static_cast<uint32_t>(at)});
                if (!KnownTop().count(id)) ++ctx.stats.unknown[std::string(kind) + ":" + IdText(id)];
                at += 8 + static_cast<size_t>(size);
            }
            if (at != file.size()) ctx.Error(std::string(kind) + ": " + std::to_string(file.size() - at) + " stray bytes at the end");
            return true;
        }

        const Chunk* First(const std::vector<Chunk>& chunks, uint32_t id)
        {
            for (const Chunk& c : chunks)
                if (c.id == id) return &c;
            return nullptr;
        }

        uint32_t Count(const std::vector<Chunk>& chunks, uint32_t id)
        {
            uint32_t n = 0;
            for (const Chunk& c : chunks) n += c.id == id;
            return n;
        }

        /// An MCNK's sub-chunks from @p from; unknown ones counted.
        void Subs(const Chunk& mcnk, size_t from, const char* kind, Ctx& ctx, std::vector<Chunk>& out)
        {
            size_t at = from;
            while (at + 8 <= mcnk.size)
            {
                const uint32_t id = U32(mcnk.data + at);
                uint32_t       size = U32(mcnk.data + at + 4);
                // MCAL's and MCLQ's own sizes are unreliable in roots of 3.3.5a: what is left.
                if (size > mcnk.size - at - 8) size = static_cast<uint32_t>(mcnk.size - at - 8);
                out.push_back(Chunk{id, mcnk.data + at + 8, size, static_cast<uint32_t>(at)});
                if (!KnownSub().count(id)) ++ctx.stats.unknown[std::string(kind) + ":MCNK/" + IdText(id)];
                at += 8 + static_cast<size_t>(size);
            }
        }

        /// The names of a string block (MTEX, MMDX, MWMO), by their offsets.
        std::vector<uint32_t> Offsets(const Chunk* names)
        {
            std::vector<uint32_t> out;
            if (!names) return out;
            uint32_t start = 0;
            for (uint32_t i = 0; i < names->size; ++i)
                if (!names->data[i])
                {
                    if (i > start) out.push_back(start);
                    start = i + 1;
                }
            return out;
        }
    }

    Era SurveyTile(const std::string& name, const TileFiles& files, uint32_t mapFlags, SurveyStats& stats)
    {
        Ctx ctx{name, stats};
        ++stats.tiles;
        const std::vector<uint8_t>* list[6] = {&files.root, &files.tex0, &files.tex1, &files.obj0, &files.obj1, &files.lod};
        const char*                 kinds[6] = {"root", "tex0", "tex1", "obj0", "obj1", "lod"};
        std::vector<Chunk>          top[6];
        for (int f = 0; f < 6; ++f)
            if (!list[f]->empty())
            {
                ++stats.files[f];
                Walk(*list[f], kinds[f], ctx, top[f]);
            }
        constexpr uint32_t kMCIN = Id("MCIN"), kMDID = Id("MDID"), kMHID = Id("MHID"), kMMDX = Id("MMDX"), kMMID = Id("MMID"),
                           kMWMO = Id("MWMO"), kMWID = Id("MWID"), kMDDF = Id("MDDF"), kMODF = Id("MODF"), kMCRD = Id("MCRD"),
                           kMCRW = Id("MCRW"), kMCRF = Id("MCRF");
        const bool monolithic = First(top[0], kMCIN) != nullptr;
        // The textures and objects of the set: in the root (monolithic) or their own files.
        const std::vector<Chunk>& tex = monolithic ? top[0] : top[1];
        const std::vector<Chunk>& obj = monolithic ? top[0] : top[3];
        const Chunk*              mdid = First(tex, kMDID);
        bool                      byId = mdid || First(tex, kMHID);

        // A root name holding a far-view file (MLHD, no MHDR): the extraction's naming, not a tile.
        if (!files.root.empty() && !First(top[0], kMHDR) && First(top[0], Id("MLHD")))
        {
            ++stats.misnamed;
            return Era::split;
        }
        // Root: 256 MCNKs with their 128-byte header.
        if (!files.root.empty())
        {
            const uint32_t n = Count(top[0], kMCNK);
            if (n != 256) ctx.Error("root: " + std::to_string(n) + " MCNKs");
            for (const Chunk& c : top[0])
            {
                if (c.id != kMCNK) continue;
                ++stats.chunks;
                if (c.size < 128)
                {
                    ctx.Error("root: an MCNK without its header");
                    continue;
                }
                if (U32(c.data) & 0x10000) ++stats.highResHoles;
                std::vector<Chunk> subs;
                Subs(c, 128, "root", ctx, subs);
            }
        }

        // Textures: names or ids, then each chunk's layers and alpha maps.
        const uint32_t textures = mdid ? mdid->size / 4 : static_cast<uint32_t>(Offsets(First(tex, kMTEX)).size());
        if (!files.tex0.empty() || monolithic)
        {
            uint32_t index = 0;
            for (const Chunk& c : tex)
            {
                if (c.id != kMCNK) continue;
                std::vector<Chunk> subs;
                Subs(c, monolithic ? 128 : 0, monolithic ? "root" : "tex0", ctx, subs);
                SplitChunk chunk;
                for (const Chunk& s : subs)
                    if (s.id == kMCLY)
                        for (uint32_t at = 0; at + 16 <= s.size; at += 16)
                            chunk.layers.push_back(LayerEntry{U32(s.data + at), U32(s.data + at + 4), U32(s.data + at + 8)});
                    else if (s.id == kMCAL)
                    {
                        chunk.mcal     = s.data;
                        chunk.mcalSize = s.size;
                    }
                if (monolithic && c.size >= 0x2C)
                {
                    // A root's MCAL ends where its header says.
                    const uint32_t ofs = U32(c.data + 0x24), size = U32(c.data + 0x28);
                    if (ofs >= 8 && size > 8 && ofs - 8 + size <= c.size)
                    {
                        chunk.mcal     = c.data + ofs;
                        chunk.mcalSize = size - 8;
                    }
                }
                stats.layers += chunk.layers.size();
                if (chunk.layers.size() > 4) ++stats.morethanFour;
                uint8_t alpha[4096];
                for (uint32_t k = 0; k < chunk.layers.size(); ++k)
                {
                    if (chunk.layers[k].texture >= textures)
                        ctx.Error("chunk " + std::to_string(index) + " layer " + std::to_string(k) + " names texture " +
                                  std::to_string(chunk.layers[k].texture) + " of " + std::to_string(textures));
                    if (!(chunk.layers[k].flags & 0x100)) continue;
                    if (DecodeAlpha(chunk, k, alpha, (mapFlags & 0x84) ? 1 : 0)) ++stats.alphaMaps;
                    else ctx.Error("chunk " + std::to_string(index) + " layer " + std::to_string(k) + ": its alpha map does not decode");
                }
                ++index;
            }
            if (!monolithic && index != 256) ctx.Error("tex0: " + std::to_string(index) + " MCNKs");
        }

        // Objects: placements naming models that exist, chunk references naming placements that exist.
        const Chunk*   mddf = First(obj, kMDDF);
        const Chunk*   modf = First(obj, kMODF);
        const uint32_t m2Count = First(obj, kMMID) ? First(obj, kMMID)->size / 4 : 0;
        const uint32_t wmoCount = First(obj, kMWID) ? First(obj, kMWID)->size / 4 : 0;
        const uint32_t doodads = mddf ? mddf->size / 36 : 0, mapObjects = modf ? modf->size / 64 : 0;
        if (mddf && mddf->size % 36) ctx.Error("MDDF of " + std::to_string(mddf->size) + " bytes");
        if (modf && modf->size % 64) ctx.Error("MODF of " + std::to_string(modf->size) + " bytes");
        if (const Chunk* mmid = First(obj, kMMID); mmid && First(obj, kMMDX))
            for (uint32_t i = 0; i + 4 <= mmid->size; i += 4)
                if (U32(mmid->data + i) >= First(obj, kMMDX)->size) ctx.Error("MMID past MMDX");
        if (const Chunk* mwid = First(obj, kMWID); mwid && First(obj, kMWMO))
            for (uint32_t i = 0; i + 4 <= mwid->size; i += 4)
                if (U32(mwid->data + i) >= First(obj, kMWMO)->size) ctx.Error("MWID past MWMO");
        for (uint32_t i = 0; i < doodads; ++i)
        {
            const uint8_t* d = mddf->data + i * 36;
            const uint16_t flags = static_cast<uint16_t>(d[34] | d[35] << 8);
            ++stats.doodads;
            if (flags & 0x40) ++stats.doodadsById, byId = true;
            else if (U32(d) >= m2Count) ctx.Error("doodad " + std::to_string(i) + " names model " + std::to_string(U32(d)) + " of " + std::to_string(m2Count));
        }
        for (uint32_t i = 0; i < mapObjects; ++i)
        {
            const uint8_t* d = modf->data + i * 64;
            const uint16_t flags = static_cast<uint16_t>(d[56] | d[57] << 8);
            ++stats.mapObjects;
            if (flags & 0x8) ++stats.mapObjectsById, byId = true;
            else if (U32(d) >= wmoCount) ctx.Error("map object " + std::to_string(i) + " names model " + std::to_string(U32(d)) + " of " + std::to_string(wmoCount));
        }
        for (const Chunk& c : obj)
        {
            if (c.id != kMCNK) continue;
            std::vector<Chunk> subs;
            Subs(c, monolithic ? 128 : 0, monolithic ? "root" : "obj0", ctx, subs);
            for (const Chunk& s : subs)
            {
                const uint32_t limit = s.id == kMCRD ? doodads : s.id == kMCRW ? mapObjects : 0;
                if (s.id != kMCRD && s.id != kMCRW) continue;
                for (uint32_t i = 0; i + 4 <= s.size; i += 4)
                    if (U32(s.data + i) >= limit) ctx.Error(IdText(s.id) + " names placement " + std::to_string(U32(s.data + i)) + " of " + std::to_string(limit));
            }
            (void)kMCRF;
        }
        const Era era = monolithic ? Era::monolithic : byId ? Era::file_ids : Era::split;
        ++stats.era[static_cast<int>(era)];
        return era;
    }
}
