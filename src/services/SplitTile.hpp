// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A tile as Cataclysm and later write it, split by file name (wowdev ADT/v18): its root
// (<map>_<x>_<y>.adt: MVER, MHDR without MCIN, 256 MCNKs with their header: MCVT, MCNR, MCCV, MCSE,
// ...; MH2O, MFBO), _tex0 (MTEX, MTXF or MTXP, MAMP; 256 MCNKs without header: MCLY, MCAL, MCSH) and
// _obj0 (MMDX, MMID, MWMO, MWID, MDDF, MODF; 256 MCNKs without header: MCRD doodad references,
// MCRW map object references). Index finds where each part the client of 3.3.5a reads lies in
// these files, checked against their bounds; nothing is copied or rewritten here. Portable: no
// client, no Windows, so the harness checks it on made-up files.
//
// The client walks a chunk's sub-chunks itself (CMapChunk::ProcessIffChunks), from the offsets of
// an index of the file's MCNKs (MCIN); a split root has no MCIN, and its MCNKs carry no layers,
// alpha maps, shadow or references: those are the _tex0's and _obj0's MCNK of the same place.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wxl::core::services::split_tile
{
    /// A chunk's payload in a file; empty when the file has none.
    struct Span
    {
        uint8_t* data = nullptr;
        uint32_t size = 0;
        explicit operator bool() const { return data != nullptr; }
    };

    /// The MCNK header's fields (wowdev SMChunk), as offsets into it: the client's own layout.
    enum HeaderField : uint32_t
    {
        kHeaderFlags = 0x00,
        kHeaderLayers = 0x0C,
        kHeaderDoodadRefs = 0x10,
        kHeaderHolesHigh = 0x14, ///< 8 bytes (in place of ofsHeight, ofsNormal) when flags say 0x10000
        kHeaderSizeAlpha = 0x28,
        kHeaderMapObjRefs = 0x38,
        kHeaderHoles = 0x3C,     ///< 16 bits, 4 x 4
        kHeaderSize = 0x80,
    };
    constexpr uint32_t kHighResHoles = 0x10000; ///< MCNK flag: 64-bit holes at kHeaderHolesHigh

    /// MHDR's slots (offsets into its data) and the byte of the alpha maps' size.
    constexpr uint32_t kMhdrMcin = 0x04, kMhdrMampValue = 0x30;

    /// The sizes of a placement: MDDF (SMDoodadDef) and MODF (SMMapObjDef).
    constexpr uint32_t kDoodadDefSize = 36, kMapObjDefSize = 64;
    /// Placement flags naming their model by FileDataID (8.1 and later): MDDF 0x40, MODF 0x8.
    constexpr uint16_t kDoodadByFileId = 0x40, kMapObjByFileId = 0x8;

    /// What a chunk's own parts are, from _tex0 and _obj0.
    struct ChunkParts
    {
        Span mcly, mcal, mcsh; ///< _tex0
        Span mcrd, mcrw;       ///< _obj0
    };

    /// A split tile's parts, pointing into the files Index was given.
    struct Tile
    {
        uint32_t   chunkOffset[256] = {}; ///< of each root MCNK's header (its 'MCNK'), from the root's start
        uint32_t   chunkSize[256] = {};   ///< 8 + its size, as MCIN counts it
        Span       mtex, mtxf, mtxp, mamp;
        Span       mdid, mhid;            ///< 8.1 and later: textures (and their heights) by FileDataID, in place of MTEX
        Span       mmdx, mmid, mwmo, mwid, mddf, modf;
        uint32_t   textures = 0;          ///< MTEX's names (or, set by the caller, the names it gave MDID's ids)
        ChunkParts parts[256];
        std::vector<uint8_t> missing;     ///< by texture: 1 for one the tile names by an id no one named (set by the caller)
    };

    /// Whether @p root is a root ADT without MCIN: MVER, then MHDR whose MCIN slot is 0, and MCNKs.
    bool IsSplitRoot(const uint8_t* root, uint32_t size);

    /// Indexes a split tile: the root's 256 MCNKs, and what _tex0 and _obj0 (either may be absent)
    /// hold. False, with @p why, for a root that is not split or has not 256 well-formed MCNKs;
    /// a _tex0 or _obj0 that is cut short or of the wrong shape is set aside (its parts left
    /// empty) and said in @p why with a true answer.
    bool Index(uint8_t* root, uint32_t rootSize, uint8_t* tex0, uint32_t tex0Size, uint8_t* obj0, uint32_t obj0Size, Tile& out,
               std::string& why);

    /// How many of a chunk's MCLY layers the client can draw from: at most 4, up to the first that
    /// names a texture the tile has not or an alpha map past MCAL.
    uint32_t UsableLayers(const Tile& tile, uint32_t chunk);

    /// A chunk's references the client can follow: each naming a placement that exists, whose
    /// model is named (MMID, MWID) inside the name table, not by FileDataID. Answers whether every
    /// reference is (then @p kept is left empty and the file's own list is used as it is).
    bool UsableDoodadRefs(const Tile& tile, uint32_t chunk, std::vector<uint32_t>& kept);
    bool UsableMapObjRefs(const Tile& tile, uint32_t chunk, std::vector<uint32_t>& kept);

    /// A placement's model named by FileDataID (MDDF flag 0x40, MODF flag 0x8), given the name it
    /// has: @p defs's entry @p index gets nameId @p nameId and loses the flag, as a placement by name.
    void NameByIndex(Span defs, uint32_t defSize, uint32_t index, uint32_t nameId);

    /// The texture a chunk is given when none of its layers can be drawn: one every 3.3.5a client
    /// has (a plain dirt), so the chunk keeps a layer the client can draw and query.
    constexpr const char* kPlaceholderTexture = "Tileset\\Elwynn\\ElwynnDirtBase.blp";

    /// What the client is given for a chunk's layers (CMapChunk::m_layers, nLayers). The client
    /// assumes at least one layer it can draw and four entries it can read: CMap::QueryGroundTypeTerrain
    /// reads m_layers[k] for the k (0 to 3) the chunk's low-quality map (MCNK +0x40) names, whatever
    /// nLayers says. So: the layers it can draw (UsableLayers), at least one (the placeholder
    /// texture when none can), and four readable entries: the file's MCLY when it holds four, else a
    /// table of the runtime's: the drawable entries, then copies of the first with no alpha map and
    /// no ground effect.
    struct ChunkLayers
    {
        uint32_t count = 0;       ///< nLayers: 1 to 4
        bool     own = false;     ///< the client reads `table`, not MCLY
        uint8_t  table[4 * 16] = {};
    };
    /// @p placeholder: the index the tile's texture list gives kPlaceholderTexture (needed only when
    /// NeedsPlaceholder).
    ChunkLayers PlanLayers(const Tile& tile, uint32_t chunk, uint32_t placeholder);
    /// Whether some chunk of the tile has no layer it can draw.
    bool NeedsPlaceholder(const Tile& tile);

    /// Liquids (MH2O) of a root as later clients write them, made safe for the client of 3.3.5a in
    /// place, before it reads them (a root without MCIN only):
    ///   - an instance's vertex format past 3 is a LiquidObject id (WoD and later): its format is
    ///     taken from the size of its vertex data (heights and depths 5 bytes a vertex, heights and
    ///     texture coordinates 8, all three 9, depths 1); format 3 (heights, texture coordinates,
    ///     depths) is read as 1, whose first two parts it holds, since this client draws 3 flat;
    ///   - a liquid type its LiquidType.dbc has not (@p known, by id) becomes Water (1), or Ocean
    ///     (2) for one without heights: the client looks types up without a check in several places;
    ///   - a chunk whose instances or tables lie outside MH2O, or cover more than 8 x 8, has none.
    struct LiquidFixes
    {
        uint32_t instances = 0, formats = 0, types = 0, dropped = 0;
    };
    LiquidFixes FixLiquids(uint8_t* root, uint32_t size, const std::vector<uint8_t>& known);

    /// MTXP's flags, one int32 a texture as MTXF lays them out (the client reads MTXF only).
    std::vector<int32_t> FlagsFromMtxp(const Tile& tile);

    /// Retail's 8 x 8 holes (byte = row, bit = column) folded into the client's 4 x 4 (bit 4 * row
    /// + column): a hole wherever one of the four it covers is one.
    uint16_t FoldHoles(const uint8_t high[8]);
}
