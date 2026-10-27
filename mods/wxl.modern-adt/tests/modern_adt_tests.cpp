// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.modern-adt's reading layer, store, texture scale and options, on ADTs made up byte by byte:
// a root ADT with MVER, MHDR, MTEX, MTXF (its bits 4 to 7) and a trailing MTXP no MHDR slot points
// to, as WorldGen and retail write them; a stock one (MTXF bit 0 only); a file cut short. The
// height blend: its answers (a retail MTXP's numbers, from arakkoadungeon_26_30_tex0.adt), and the
// rewritten pixel program run by a small ps_3_0 interpreter against the blend's formula worked out
// apart: the Direct3D 9 path's CPU reference (the Vulkan renderer's is held on lavapipe). Tiles
// split by file name: the files read beside a root without MCIN, handed to the tile provider, and
// the textures a split tile's layers name, read from its _tex0.

#include "Options.hpp"
#include "adt/Reader.hpp"
#include "adt/Survey.hpp"
#include "features/ExtraLayers.hpp"
#include "features/HeightBlend.hpp"
#include "features/SplitTiles.hpp"
#include "features/TextureScale.hpp"
#include "shader/Ps3.hpp"
#include "store/Store.hpp"
#include "store/TileSet.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace
{
    using namespace wxl::modern_adt;
    int g_failures = 0, g_checks = 0;

    void Check(bool ok, const char* what)
    {
        ++g_checks;
        if (!ok) ++g_failures;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    }

    /// A file made chunk by chunk, MHDR's slots set to the chunks named.
    struct Adt
    {
        std::vector<uint8_t> bytes;

        void U32(uint32_t v)
        {
            for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
        void F32(float f)
        {
            uint32_t v;
            std::memcpy(&v, &f, 4);
            U32(v);
        }
        size_t Chunk(uint32_t id, const std::vector<uint8_t>& data)
        {
            const size_t at = bytes.size();
            U32(id);
            U32(static_cast<uint32_t>(data.size()));
            bytes.insert(bytes.end(), data.begin(), data.end());
            return at;
        }
    };

    std::vector<uint8_t> Words(std::initializer_list<uint32_t> words)
    {
        std::vector<uint8_t> out;
        for (uint32_t w : words)
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(w >> (8 * i)));
        return out;
    }

    /// A root ADT of three textures; MTXF's scales 0, 2, 4 unless @p mtxf_bits is false; MTXP's
    /// 1, 3, 0 (with heights) when @p mtxp.
    std::vector<uint8_t> MakeAdt(bool mtxf_bits, bool mtxp)
    {
        Adt a;
        a.Chunk(adt::kMVER, Words({18}));
        const size_t mhdr = a.Chunk(adt::kMHDR, std::vector<uint8_t>(64, 0));
        const std::string names = std::string("Tileset\\Retail\\a.blp") + '\0' + "Tileset\\Retail\\b.blp" + '\0' + "Tileset\\Retail\\c.blp" + '\0';
        const size_t mtex = a.Chunk(adt::kMTEX, std::vector<uint8_t>(names.begin(), names.end()));
        const size_t mtxf = a.Chunk(adt::kMTXF, mtxf_bits ? Words({0x00, 0x20, 0x41}) : Words({0x0, 0x1, 0x0}));
        if (mtxp)
        {
            Adt p;
            for (uint32_t scale : {1u, 3u, 0u})
            {
                p.U32(scale << 4);
                p.F32(scale ? 1.0f : 0.0f);
                p.F32(scale ? 0.0f : 1.0f);
                p.U32(0);
            }
            a.Chunk(adt::kMTXP, p.bytes);
        }
        // MHDR's slots, relative to the start of its data.
        const size_t data = mhdr + 8;
        uint32_t v = static_cast<uint32_t>(mtex - data);
        std::memcpy(&a.bytes[data + adt::kSlotMtex], &v, 4);
        v = static_cast<uint32_t>(mtxf - data);
        std::memcpy(&a.bytes[data + adt::kSlotMtxf], &v, 4);
        return a.bytes;
    }

    void ReaderCases()
    {
        const std::vector<uint8_t> file = MakeAdt(true, true);
        adt::Reader                r(file.data(), file.size());
        int                        chunks = 0;
        r.Chunks([&](const adt::Chunk&) { return ++chunks, true; });
        Check(chunks == 5, "the top-level chunks walked: MVER, MHDR, MTEX, MTXF, MTXP");
        Check(r.Mhdr() && r.Mhdr().size == 64, "MHDR found after MVER");
        Check(r.FromMhdr(adt::kSlotMtxf, adt::kMTXF).size == 12, "MTXF reached through MHDR + 0x2C");
        Check(!r.FromMhdr(adt::kSlotMh2o, adt::kMTXF), "an empty slot reaches nothing");
        const auto names = r.TextureNames();
        Check(names.size() == 3 && names[1] == "Tileset\\Retail\\b.blp", "MTEX's names");
        Check(r.Find(adt::kMTXP).size == 48, "MTXP found by walking: no MHDR slot points to it");

        const auto viaMtxf = adt::ReadTextures(r, adt::Prefer::mtxf);
        Check(viaMtxf.size() == 3 && viaMtxf[0].scale == 0 && viaMtxf[1].scale == 2 && viaMtxf[2].scale == 4 &&
                  viaMtxf[2].source == adt::Source::mtxf && viaMtxf[2].flags == 0x41,
              "MTXF: the scale is (flags >> 4) & 0xF, bit 0 (no _s) kept");
        const auto viaMtxp = adt::ReadTextures(r, adt::Prefer::automatic);
        Check(viaMtxp[0].scale == 1 && viaMtxp[1].scale == 3 && viaMtxp[2].scale == 0 && viaMtxp[1].source == adt::Source::mtxp &&
                  viaMtxp[1].height_scale == 1.0f && viaMtxp[2].height_offset == 1.0f,
              "auto: MTXP wins when the file has both, its heights kept for the blend");
        Check(adt::ScaleOf(3) == 8.0f && adt::ScaleOf(0) == 1.0f, "2^scale");

        const std::vector<uint8_t> stock = MakeAdt(false, false);
        const auto                 s     = adt::ReadTextures(adt::Reader(stock.data(), stock.size()), adt::Prefer::automatic);
        bool                       none  = s.size() == 3;
        for (const auto& t : s) none = none && t.scale == 0;
        Check(none, "a stock ADT (MTXF bit 0 only): every scale 0");

        std::vector<uint8_t> cut = file;
        cut.resize(cut.size() - 20);
        const auto c = adt::ReadTextures(adt::Reader(cut.data(), cut.size()), adt::Prefer::automatic);
        Check(c.size() == 3 && c[1].source == adt::Source::mtxf, "a file cut short in MTXP: MTXF read, nothing past the end");
        Check(adt::ReadTextures(adt::Reader(nullptr, 0), adt::Prefer::automatic).empty(), "no file: nothing");
    }

    void FeatureCases()
    {
        const std::vector<uint8_t> file = MakeAdt(true, false);
        Store                      store;
        TextureScale               scale(store);
        wxl_terrain_layer          layer{};
        layer.struct_size = sizeof layer;
        layer.tile_x      = 32;
        layer.tile_y      = 48;
        layer.chunk_x     = 512;
        layer.chunk_y     = 768;
        layer.file        = file.data();
        layer.file_size   = static_cast<uint32_t>(file.size());
        float answers[3];
        for (uint32_t t = 0; t < 3; ++t)
        {
            layer.layer   = t;
            layer.texture = t;
            wxl_terrain_layer_look look{sizeof(wxl_terrain_layer_look), 1.0f};
            scale.Answer(layer, look);
            answers[t] = look.texture_scale;
        }
        Check(answers[0] == 1.0f && answers[1] == 4.0f && answers[2] == 16.0f, "each layer answered 2^scale; a texture of scale 0 left at the client's own");
        Check(store.Reads() == 1 && store.Areas() == 1, "an area read once for all its layers");
        const Layer* kept = store.LayerAt(512, 768, 2);
        Check(kept && kept->texture == 2 && kept->info.scale == 4, "each layer kept by chunk and place, for the features to come");
        const std::vector<uint8_t> again = MakeAdt(false, false);
        layer.file      = again.data();
        layer.file_size = static_cast<uint32_t>(again.size());
        layer.texture   = 1;
        wxl_terrain_layer_look look{sizeof(wxl_terrain_layer_look), 1.0f};
        scale.Answer(layer, look);
        Check(store.Reads() == 2 && look.texture_scale == 1.0f, "an area loaded again from other bytes is read again");
        layer.texture = 9;
        look.texture_scale = 1.0f;
        scale.Answer(layer, look);
        Check(look.texture_scale == 1.0f, "a texture past MTEX's: the client's own look");
    }

    /// MTXP of arakkoadungeon_26_30_tex0.adt (retail): flags, heightScale, heightOffset, padding.
    const float kRetailHeights[6][2] = {{3.0f, 1.0f}, {10.0f, 1.0f}, {5.0f, 1.0f}, {10.9f, 1.0f}, {5.0f, 1.0f}, {0.0f, 1.0f}};
    const uint32_t kRetailFlags[6] = {32, 16, 32, 16, 48, 16};

    std::vector<uint8_t> RetailAdt()
    {
        Adt a;
        a.Chunk(adt::kMVER, Words({18}));
        const size_t mhdr = a.Chunk(adt::kMHDR, std::vector<uint8_t>(64, 0));
        std::string  names;
        for (int i = 0; i < 6; ++i) names += "Tileset\\Arakkoa\\t" + std::to_string(i) + ".blp" + '\0';
        const size_t mtex = a.Chunk(adt::kMTEX, std::vector<uint8_t>(names.begin(), names.end()));
        Adt p;
        for (int i = 0; i < 6; ++i)
        {
            p.U32(kRetailFlags[i]);
            p.F32(kRetailHeights[i][0]);
            p.F32(kRetailHeights[i][1]);
            p.U32(0);
        }
        a.Chunk(adt::kMTXP, p.bytes);
        const size_t data = mhdr + 8;
        const uint32_t v  = static_cast<uint32_t>(mtex - data);
        std::memcpy(&a.bytes[data + adt::kSlotMtex], &v, 4);
        return a.bytes;
    }

    void HeightBlendCases()
    {
        Check(HeightBlend::HeightName("Tileset\\a\\Rock.BLP") == "Tileset\\a\\Rock_h.BLP" && HeightBlend::HeightName("x.tga").empty(),
              "a texture's height texture is its name with _h before .blp");
        const std::vector<uint8_t> file = RetailAdt();
        Store                      store;
        TextureScale               scale(store);
        std::vector<std::string>   asked;
        HeightBlend                blend(store, [&](const std::string& name) {
            asked.push_back(name);
            return name.find("t3_h") == std::string::npos; // t3's _h is missing
        }, false);
        wxl_terrain_layer layer{};
        layer.struct_size = sizeof layer;
        layer.chunk_x = 10, layer.chunk_y = 20;
        layer.file      = file.data();
        layer.file_size = static_cast<uint32_t>(file.size());
        wxl_terrain_layer_look looks[6];
        for (uint32_t t = 0; t < 6; ++t)
        {
            layer.layer   = t % 4;
            layer.texture = t;
            looks[t]      = wxl_terrain_layer_look{};
            looks[t].struct_size   = sizeof looks[t];
            looks[t].texture_scale = 1.0f;
            scale.Answer(layer, looks[t]);
            blend.Answer(layer, looks[t]);
        }
        Check(looks[0].texture_scale == 4.0f && looks[4].texture_scale == 8.0f, "the retail file's scales: 2^2, 2^3");
        Check(looks[1].extra_texture && std::string(looks[1].extra_texture) == "Tileset\\Arakkoa\\t1_h.blp" && looks[1].params[0] == 10.0f &&
                  looks[1].params[1] == 1.0f && looks[1].params[2] == 1.0f,
              "a texture with an MTXP height: its _h texture and (heightScale, heightOffset, 1)");
        Check(!looks[3].extra_texture && looks[3].params[2] == 0.0f, "a texture whose _h file is missing: the client's blend");
        Check(!looks[5].extra_texture && looks[5].params[2] == 0.0f && asked.size() == 5, "heightScale 0 and heightOffset 1 (retail's no _h): not even looked for");
        wxl_terrain_layer_look old{};
        old.struct_size = offsetof(wxl_terrain_layer_look, extra_texture);
        layer.texture = 1;
        layer.layer   = 1;
        blend.Answer(layer, old);
        Check(blend.Blended() == 4, "a look of abi 0.1.42 (no room for the texture) is left alone");
        const std::vector<uint8_t> stock = MakeAdt(false, false);
        layer.file      = stock.data();
        layer.file_size = static_cast<uint32_t>(stock.size());
        wxl_terrain_layer_look look{};
        look.struct_size = sizeof look;
        scale.Answer(layer, look);
        blend.Answer(layer, look);
        Check(!look.extra_texture && look.params[2] == 0.0f, "a stock ADT (no MTXP): the client's blend");
    }

    // --- A ps_3_0 program, and a small interpreter for it ---------------------------------------

    using Vec = std::array<float, 4>;

    uint32_t RegTok(uint32_t type, uint32_t num) { return 0x80000000u | ((type & 7) << 28) | (((type >> 3) & 3) << 11) | num; }
    uint32_t D(uint32_t type, uint32_t num, uint32_t mask = 0xF) { return RegTok(type, num) | (mask << 16); }
    uint32_t S(uint32_t type, uint32_t num, uint32_t swz = 0xE4) { return RegTok(type, num) | (swz << 16); }
    uint32_t Fl(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }

    /// The client's Terrain1 for @p layers layers, reduced to its blend: layer k on sampler k at
    /// v(2 + k), the alpha maps on sampler L at v(2 + L), lrp one over the other, out as the colour.
    std::vector<uint32_t> TerrainProgram(uint32_t layers, bool cube0 = false)
    {
        std::vector<uint32_t> w{0xFFFF0300u};
        const auto op = [&](uint32_t o, std::initializer_list<uint32_t> p) {
            w.push_back(o | (static_cast<uint32_t>(p.size()) << 24));
            w.insert(w.end(), p.begin(), p.end());
        };
        op(0x51, {D(2, 0), Fl(0.3f), Fl(0.7f), Fl(1.0f), Fl(0.0f)});
        for (uint32_t k = 0; k <= layers; ++k) op(0x1F, {0x80000005u, D(1, 2 + k)}); // dcl_texcoord
        for (uint32_t k = 0; k <= layers; ++k) op(0x1F, {0x80000000u | ((k == 0 && cube0 ? 3u : 2u) << 27), D(10, k)});
        for (uint32_t k = 0; k <= layers; ++k) op(0x42, {D(0, k), S(1, 2 + k), S(10, k)});
        uint32_t acc = 0;
        for (uint32_t k = 1; k < layers; ++k)
        {
            const uint32_t to = 10 + k;
            op(0x12, {D(0, to), S(0, layers, (k - 1) * 0x55), S(0, k), S(0, acc)}); // lrp
            acc = to;
        }
        op(0x01, {D(8, 0), S(0, acc)}); // mov oC0
        w.push_back(0x0000FFFFu);
        return w;
    }

    /// The client's Terrain1 for maps with big alpha (Terrain1:14 for four layers), reduced to its
    /// blend: layer 0 gets 1 - sat(a1 + a2 + a3), layer k its alpha, summed.
    std::vector<uint32_t> BigAlphaProgram(uint32_t layers)
    {
        std::vector<uint32_t> w{0xFFFF0300u};
        const auto op = [&](uint32_t o, std::initializer_list<uint32_t> p) {
            w.push_back(o | (static_cast<uint32_t>(p.size()) << 24));
            w.insert(w.end(), p.begin(), p.end());
        };
        op(0x51, {D(2, 0), Fl(1.0f), Fl(0.3f), Fl(0.7f), Fl(0.0f)});
        for (uint32_t k = 0; k <= layers; ++k) op(0x1F, {0x80000005u, D(1, 2 + k)});
        for (uint32_t k = 0; k <= layers; ++k) op(0x1F, {0x80000000u | (2u << 27), D(10, k)});
        for (uint32_t k = 0; k <= layers; ++k) op(0x42, {D(0, k), S(1, 2 + k), S(10, k)});
        op(0x08, {D(0, 8, 1) | (1u << 20), S(0, layers), S(2, 0, 0x00)});      // dp3_sat r8.x, alphas, c0.x
        op(0x02, {D(0, 8, 1), S(0, 8, 0x00) | 0x01000000u, S(2, 0, 0x00)});   // add r8.x, -r8.x, c0.x
        op(0x05, {D(0, 9), S(0, 0), S(0, 8, 0x00)});                           // mul r9, t0, r8.x
        for (uint32_t k = 1; k < layers; ++k) op(0x04, {D(0, 9), S(0, k), S(0, layers, (k - 1) * 0x55), S(0, 9)}); // mad r9, tk, ak, r9
        op(0x01, {D(8, 0), S(0, 9)});
        w.push_back(0x0000FFFFu);
        return w;
    }

    /// Mips 5 and 6 of 12elw_riverstones01_1024_h.blp's alpha (retail): a real height map, sampled
    /// bilinearly between two levels, as a device filters it.
    const uint8_t kRiverstonesHeight[32 * 32 + 16 * 16] = {
    // 32x32
        90, 57, 90, 124, 49, 34, 80, 111, 122, 122, 88, 54, 83, 43, 103, 143, 115, 82, 33, 33, 43, 59, 43, 43, 34, 102, 119, 85, 48, 93, 138, 123,
        57, 24, 40, 74, 34, 49, 80, 80, 88, 88, 71, 37, 24, 24, 123, 123, 115, 115, 82, 33, 91, 75, 59, 157, 102, 68, 85, 85, 48, 78, 108, 93,
        40, 57, 24, 24, 34, 49, 96, 127, 122, 105, 54, 20, 24, 43, 163, 163, 115, 99, 99, 49, 75, 43, 59, 157, 153, 85, 51, 51, 48, 48, 93, 78,
        90, 141, 57, 57, 34, 111, 143, 143, 140, 122, 105, 37, 43, 123, 163, 143, 66, 82, 99, 149, 124, 75, 91, 124, 153, 119, 85, 102, 154, 93, 78, 78,
        73, 73, 55, 55, 70, 122, 149, 149, 149, 123, 123, 123, 126, 126, 126, 67, 46, 46, 156, 156, 107, 119, 94, 70, 108, 108, 141, 152, 114, 95, 76, 38,
        91, 55, 37, 37, 70, 122, 122, 135, 136, 136, 175, 188, 185, 165, 67, 48, 68, 90, 179, 156, 82, 107, 119, 144, 119, 86, 75, 97, 95, 114, 95, 76,
        165, 165, 37, 37, 57, 109, 122, 122, 110, 149, 188, 188, 185, 185, 106, 48, 46, 68, 134, 112, 82, 132, 132, 157, 152, 75, 75, 97, 57, 76, 133, 172,
        146, 110, 73, 91, 57, 83, 96, 96, 97, 136, 188, 188, 185, 145, 87, 87, 24, 24, 90, 68, 132, 157, 157, 157, 152, 75, 97, 141, 133, 38, 133, 172,
        81, 61, 41, 81, 75, 55, 55, 75, 116, 103, 141, 166, 154, 102, 68, 68, 73, 113, 100, 100, 155, 155, 155, 155, 116, 57, 77, 96, 81, 43, 81, 119,
        61, 101, 121, 101, 55, 36, 94, 153, 141, 79, 103, 103, 85, 102, 85, 85, 168, 154, 127, 127, 155, 155, 155, 145, 96, 77, 96, 155, 100, 43, 62, 119,
        121, 141, 161, 182, 114, 36, 75, 114, 128, 128, 128, 116, 85, 102, 189, 154, 168, 154, 140, 127, 155, 155, 155, 107, 77, 77, 155, 175, 176, 119, 176, 176,
        141, 161, 161, 182, 114, 55, 114, 173, 166, 166, 166, 153, 102, 154, 154, 119, 154, 154, 140, 100, 126, 145, 135, 88, 38, 38, 135, 175, 176, 157, 119, 100,
        153, 173, 173, 153, 98, 56, 141, 184, 188, 171, 154, 121, 78, 113, 78, 101, 137, 137, 93, 49, 64, 102, 102, 64, 24, 24, 89, 139, 155, 155, 77, 93,
        134, 173, 153, 114, 77, 35, 98, 120, 121, 121, 104, 87, 101, 113, 113, 113, 93, 115, 137, 93, 141, 180, 141, 45, 56, 89, 56, 106, 124, 124, 62, 62,
        95, 114, 95, 56, 35, 56, 77, 77, 71, 71, 71, 137, 160, 160, 160, 148, 137, 204, 181, 115, 160, 180, 141, 45, 106, 106, 89, 56, 77, 77, 62, 62,
        75, 56, 37, 56, 98, 120, 120, 141, 121, 87, 87, 154, 160, 160, 160, 148, 137, 159, 137, 93, 141, 122, 64, 64, 106, 106, 106, 89, 47, 93, 108, 93,
        128, 128, 39, 56, 114, 131, 131, 114, 89, 72, 89, 157, 155, 155, 155, 155, 131, 72, 116, 72, 82, 100, 117, 82, 113, 113, 113, 93, 47, 103, 117, 103,
        92, 39, 74, 92, 96, 79, 79, 62, 157, 191, 123, 106, 139, 155, 155, 139, 101, 116, 116, 57, 65, 188, 188, 100, 103, 113, 103, 83, 47, 89, 131, 117,
        92, 56, 128, 164, 149, 149, 131, 96, 157, 191, 174, 123, 91, 107, 123, 107, 43, 72, 72, 87, 135, 135, 135, 65, 53, 83, 73, 63, 103, 103, 103, 117,
        39, 39, 110, 164, 184, 184, 166, 131, 106, 174, 174, 140, 107, 59, 59, 43, 57, 57, 43, 146, 170, 188, 188, 152, 43, 43, 43, 93, 145, 131, 75, 61,
        52, 67, 67, 125, 169, 169, 130, 92, 48, 122, 152, 137, 114, 134, 194, 134, 86, 86, 66, 186, 204, 204, 204, 164, 63, 77, 121, 121, 124, 124, 124, 87,
        67, 96, 81, 52, 92, 92, 92, 54, 92, 107, 107, 122, 114, 154, 194, 154, 47, 47, 66, 146, 204, 204, 204, 144, 77, 121, 151, 166, 181, 181, 162, 106,
        67, 96, 67, 24, 35, 149, 169, 149, 92, 77, 48, 77, 74, 55, 114, 94, 66, 47, 47, 66, 124, 144, 144, 104, 166, 136, 121, 151, 162, 162, 143, 87,
        38, 52, 24, 38, 111, 169, 169, 169, 137, 62, 92, 92, 74, 55, 55, 55, 126, 106, 66, 186, 164, 84, 64, 144, 121, 63, 77, 107, 124, 124, 106, 50,
        33, 51, 51, 51, 136, 166, 159, 159, 142, 90, 125, 125, 62, 45, 79, 62, 118, 136, 99, 99, 157, 129, 72, 100, 64, 36, 50, 50, 57, 57, 102, 72,
        144, 107, 88, 88, 151, 166, 159, 159, 142, 73, 108, 108, 45, 114, 148, 131, 44, 62, 62, 62, 114, 129, 129, 129, 92, 106, 64, 50, 133, 133, 57, 87,
        163, 144, 107, 70, 166, 174, 174, 166, 160, 56, 39, 90, 96, 131, 166, 148, 44, 44, 44, 136, 157, 157, 157, 143, 106, 135, 50, 106, 164, 148, 102, 148,
        107, 107, 51, 33, 129, 159, 182, 174, 125, 39, 73, 160, 166, 131, 131, 79, 44, 62, 81, 174, 172, 172, 172, 157, 106, 135, 64, 64, 118, 118, 72, 118,
        92, 56, 44, 68, 40, 111, 129, 93, 81, 100, 174, 174, 129, 71, 56, 28, 29, 29, 80, 150, 177, 177, 177, 156, 91, 64, 64, 50, 90, 106, 45, 60,
        80, 80, 56, 56, 40, 93, 93, 57, 45, 155, 137, 100, 71, 42, 28, 28, 29, 46, 46, 80, 114, 135, 135, 114, 77, 37, 118, 132, 75, 45, 60, 60,
        92, 104, 104, 56, 165, 165, 111, 40, 63, 81, 45, 45, 114, 71, 42, 71, 63, 115, 80, 46, 72, 72, 72, 51, 37, 77, 132, 118, 75, 90, 152, 121,
        128, 104, 128, 128, 93, 75, 57, 75, 118, 118, 81, 45, 100, 42, 28, 28, 46, 98, 80, 63, 30, 51, 72, 72, 37, 104, 118, 91, 60, 106, 136, 121,
    // 16x16
        58, 86, 44, 86, 100, 61, 42, 120, 110, 44, 77, 77, 73, 95, 73, 118, 86, 44, 58, 128, 120, 61, 61, 159, 94, 94, 77, 110, 130, 73, 84, 84,
        72, 44, 100, 143, 139, 159, 159, 81, 60, 161, 110, 110, 107, 118, 107, 73, 143, 58, 72, 114, 120, 179, 179, 81, 44, 94, 127, 144, 107, 107, 73, 153,
        78, 78, 62, 94, 109, 131, 109, 76, 127, 117, 157, 157, 82, 111, 67, 97, 142, 174, 78, 126, 142, 142, 120, 154, 157, 127, 147, 117, 53, 156, 156, 141,
        158, 142, 62, 142, 154, 120, 109, 98, 117, 97, 117, 87, 53, 97, 141, 82, 78, 62, 78, 110, 87, 109, 154, 154, 157, 137, 147, 87, 111, 82, 67, 82,
        98, 65, 98, 98, 124, 124, 154, 154, 105, 86, 105, 124, 110, 97, 72, 122, 49, 147, 164, 131, 154, 154, 94, 79, 67, 86, 162, 143, 60, 72, 122, 85,
        65, 81, 131, 98, 94, 139, 124, 169, 67, 124, 201, 181, 85, 135, 148, 122, 65, 49, 114, 164, 94, 79, 65, 79, 86, 86, 124, 105, 122, 122, 148, 97,
        90, 73, 156, 156, 114, 114, 67, 98, 90, 71, 129, 110, 67, 56, 100, 78, 139, 57, 156, 173, 98, 98, 129, 129, 52, 110, 168, 148, 122, 78, 134, 111,
        73, 57, 73, 90, 98, 145, 83, 37, 33, 90, 148, 148, 67, 89, 78, 56, 106, 106, 123, 73, 98, 52, 83, 37, 90, 71, 52, 71, 67, 111, 78, 134,
    };

    float HeightAt(float u, float v, float lod)
    {
        const auto level = [](const uint8_t* texels, int side, float u, float v) {
            const float x = u * side - 0.5f, y = v * side - 0.5f;
            const int   x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
            const float fx = x - x0, fy = y - y0;
            const auto  at = [&](int i, int j) { return texels[((j % side + side) % side) * side + ((i % side + side) % side)] / 255.0f; };
            return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
        };
        return level(kRiverstonesHeight, 32, u, v) * (1 - lod) + level(kRiverstonesHeight + 32 * 32, 16, u, v) * lod;
    }

    struct Machine
    {
        Vec r[32]{}, v[16]{}, c[256]{}, o{};
        std::function<Vec(uint32_t sampler, const Vec& at)> sample;

        static uint32_t Type(uint32_t t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); }
        Vec* Reg(uint32_t t)
        {
            const uint32_t n = t & 0x7FF;
            switch (Type(t))
            {
            case 0: return &r[n];
            case 1: return &v[n];
            case 2: return &c[n];
            case 8: return &o;
            default: return nullptr;
            }
        }
        Vec Src(uint32_t t)
        {
            const Vec* p = Reg(t);
            Vec        out{};
            if (!p) return out;
            const uint32_t swz = (t >> 16) & 0xFF, mod = (t >> 24) & 0xF;
            for (int i = 0; i < 4; ++i)
            {
                float f = (*p)[(swz >> (2 * i)) & 3];
                if (mod == 0xB || mod == 0xC) f = std::fabs(f);
                if (mod == 1 || mod == 0xC) f = -f;
                out[i] = f;
            }
            return out;
        }
        void Dst(uint32_t t, const Vec& value)
        {
            Vec* p = Reg(t);
            const uint32_t mask = (t >> 16) & 0xF;
            const bool     sat  = (t >> 20) & 1;
            for (int i = 0; i < 4; ++i)
                if (mask & (1u << i)) (*p)[i] = sat ? std::min(1.0f, std::max(0.0f, value[i])) : value[i];
        }
        /// Runs @p w; false on an opcode it does not know.
        bool Run(const std::vector<uint32_t>& w)
        {
            for (size_t i = 1; i < w.size();)
            {
                const uint32_t tok = w[i];
                if (tok == 0x0000FFFFu) return true;
                const uint32_t op = tok & 0xFFFF, len = op == 0xFFFE ? (tok >> 16) & 0x7FFF : (tok >> 24) & 0xF;
                const uint32_t* p = &w[i + 1];
                Vec a{}, b{}, cc{}, out{};
                switch (op)
                {
                case 0xFFFE: case 0x1F: break;
                case 0x51: for (int k = 0; k < 4; ++k) std::memcpy(&c[p[0] & 0x7FF][k], &p[1 + k], 4); break;
                case 0x01: Dst(p[0], Src(p[1])); break;
                case 0x02: a = Src(p[1]), b = Src(p[2]); for (int k = 0; k < 4; ++k) out[k] = a[k] + b[k]; Dst(p[0], out); break;
                case 0x04: a = Src(p[1]), b = Src(p[2]), cc = Src(p[3]); for (int k = 0; k < 4; ++k) out[k] = a[k] * b[k] + cc[k]; Dst(p[0], out); break;
                case 0x05: a = Src(p[1]), b = Src(p[2]); for (int k = 0; k < 4; ++k) out[k] = a[k] * b[k]; Dst(p[0], out); break;
                case 0x08: a = Src(p[1]), b = Src(p[2]); out.fill(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]); Dst(p[0], out); break;
                case 0x06: a = Src(p[1]); out.fill(1.0f / a[3]); Dst(p[0], out); break; // replicate swizzle: any component
                case 0x09: a = Src(p[1]), b = Src(p[2]); out.fill(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]); Dst(p[0], out); break;
                case 0x0B: a = Src(p[1]), b = Src(p[2]); for (int k = 0; k < 4; ++k) out[k] = std::max(a[k], b[k]); Dst(p[0], out); break;
                case 0x12: a = Src(p[1]), b = Src(p[2]), cc = Src(p[3]); for (int k = 0; k < 4; ++k) out[k] = a[k] * (b[k] - cc[k]) + cc[k]; Dst(p[0], out); break;
                case 0x58: a = Src(p[1]), b = Src(p[2]), cc = Src(p[3]); for (int k = 0; k < 4; ++k) out[k] = a[k] >= 0.0f ? b[k] : cc[k]; Dst(p[0], out); break;
                case 0x42: Dst(p[0], sample(p[2] & 0x7FF, Src(p[1]))); break;
                default: return false;
                }
                i += 1 + len;
            }
            return false;
        }
    };

    /// The blend worked out apart (wowdev's retail excerpt, the weights from the client's lerps).
    Vec Reference(uint32_t layers, const float* a, const float* h, const float (*params)[4], const Vec* t)
    {
        float wgt[4] = {}, rem = 1.0f;
        for (int k = static_cast<int>(layers) - 1; k >= 1; --k)
        {
            wgt[k] = a[k - 1] * rem;
            rem *= 1.0f - a[k - 1];
        }
        wgt[0] = rem;
        bool any = false;
        float pct[4] = {}, top = 0.0f, sum = 0.0f;
        for (uint32_t k = 0; k < layers; ++k)
        {
            any    = any || params[k][2] != 0.0f;
            pct[k] = wgt[k] * (params[k][2] != 0.0f ? h[k] * params[k][0] + params[k][1] : 1.0f);
            top    = std::max(top, pct[k]);
        }
        Vec out{};
        if (!any)
        {
            for (uint32_t k = 0; k < layers; ++k)
                for (int i = 0; i < 4; ++i) out[i] += wgt[k] * t[k][i];
            return out;
        }
        for (uint32_t k = 0; k < layers; ++k)
        {
            pct[k] *= 1.0f - std::min(1.0f, std::max(0.0f, top - pct[k]));
            sum += pct[k];
        }
        for (uint32_t k = 0; k < layers; ++k)
            for (int i = 0; i < 4; ++i) out[i] += pct[k] / std::max(sum, 1e-6f) * t[k][i];
        return out;
    }

    void ShaderCases()
    {
        using shader::BlendByHeight;
        std::vector<uint32_t> out;
        shader::Found         found;
        std::string           why;
        std::vector<uint32_t> one = TerrainProgram(1);
        Check(!BlendByHeight(one.data(), one.size() * 4, out, found, why) && why == "fewer than two layers", "one layer: left as it is");
        std::vector<uint32_t> vs = TerrainProgram(4);
        vs[0]                    = 0xFFFE0300u;
        Check(!BlendByHeight(vs.data(), vs.size() * 4, out, found, why) && why == "not ps_3_0", "another profile: left");

        uint32_t seed = 12345;
        const auto rnd = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / 16777216.0f;
        };
        for (uint32_t layers = 2; layers <= 4; ++layers)
        {
            const std::vector<uint32_t> stock = TerrainProgram(layers);
            const bool made = BlendByHeight(stock.data(), stock.size() * 4, out, found, why);
            char       what[160];
            std::snprintf(what, sizeof what, "%u layers: rewritten, every layer read with its height (four: layers 4 and 5 too)", layers);
            Check(made && found.layers == layers && found.heights == (layers == 4 ? 6u : layers), what);
            if (!made) continue;
            float worst = 0.0f;
            bool  same  = true, ran = true;
            for (int trial = 0; trial < 400; ++trial)
            {
                float a[3], h[4], params[4][4] = {};
                Vec   t[4];
                for (float& x : a) x = trial % 7 == 0 ? std::round(rnd()) : rnd();
                for (float& x : h) x = rnd();
                for (Vec& x : t) x = {rnd(), rnd(), rnd(), rnd()};
                const bool heights = trial % 3 != 0; // a third of the chunks without any height
                for (uint32_t k = 0; k < layers; ++k)
                    if (heights && (trial + k) % 4 != 0)
                    {
                        params[k][0] = 1.0f + 10.0f * rnd();
                        params[k][1] = trial % 5 ? 1.0f : 0.5f;
                        params[k][2] = 1.0f;
                    }
                const auto run = [&](const std::vector<uint32_t>& program, Vec& colour) {
                    Machine m;
                    m.sample = [&](uint32_t s, const Vec&) -> Vec {
                        if (s < layers) return t[s];
                        if (s == layers) return {a[0], a[1], a[2], 1.0f};
                        if (s >= shader::kHeightSampler && s < shader::kHeightSampler + 4) return {0.0f, 0.0f, 0.0f, h[s - shader::kHeightSampler]};
                        return {};
                    };
                    for (uint32_t k = 0; k < 4; ++k) std::memcpy(&m.c[shader::kParamRow + k], params[k], sizeof params[k]);
                    if (!m.Run(program)) return false;
                    colour = m.o;
                    return true;
                };
                Vec before{}, after{};
                ran = ran && run(stock, before) && run(out, after);
                const Vec want = Reference(layers, a, h, params, t);
                for (int i = 0; i < 4; ++i) worst = std::max(worst, std::fabs(after[i] - want[i]));
                if (!heights) same = same && std::memcmp(&before, &after, sizeof before) == 0;
            }
            std::snprintf(what, sizeof what, "%u layers: the program's colour is the retail blend's (worst %.2g over 400 pixels)", layers, worst);
            Check(ran && worst < 2e-5f, what);
            std::snprintf(what, sizeof what, "%u layers: a chunk without heights draws bit for bit as the client's program", layers);
            Check(ran && same, what);
        }
        std::vector<uint32_t> cube = TerrainProgram(3, true);
        Check(BlendByHeight(cube.data(), cube.size() * 4, out, found, why) && found.heights == 2, "a cube-map layer 0 is read without a height");
        std::vector<uint32_t> busy = TerrainProgram(4);
        busy.insert(busy.begin() + 1, {0x1F | (2u << 24), 0x80000000u | (2u << 27), D(10, 12)});
        Check(!BlendByHeight(busy.data(), busy.size() * 4, out, found, why) && why == "a height sampler is in use", "a program using sampler 12 already: left");

    }

    /// Four layers: eight, in two draws blended as the device blends them, in both mixes.
    void EightLayerCases()
    {
        using shader::BlendByHeight;
        uint32_t seed = 4242;
        const auto rnd = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / 16777216.0f;
        };
        for (int form = 0; form < 2; ++form)
        {
        const bool sums = form == 1;
        std::vector<uint32_t> out;
        shader::Found         found;
        std::string           why;
        const std::vector<uint32_t> four = sums ? BigAlphaProgram(4) : TerrainProgram(4);
        Check(BlendByHeight(four.data(), four.size() * 4, out, found, why) && found.extras && found.heights == 6 && found.sums == sums,
              sums ? "a summing four-layer program is rewritten for eight layers, six with their heights"
                   : "a four-layer program is rewritten for eight layers, six with their heights");
        float worst = 0.0f;
        bool  same = true, ran = true;
        for (int trial = 0; trial < 600; ++trial)
        {
            float alpha[8] = {}, params[8][4] = {};
            Vec   t[8];
            float left = 1.0f;
            for (int k = 1; k < 8; ++k)
            {
                alpha[k] = trial % 9 == 0 ? std::round(rnd()) : rnd();
                if (sums) alpha[k] *= left, left -= alpha[k]; // retail's: at most 1 in all
            }
            const bool extras = trial % 4 != 0, heights = trial % 3 != 0;
            if (!extras)
                for (int k = 4; k < 8; ++k) alpha[k] = 0.0f;
            for (Vec& x : t) x = {rnd(), rnd(), rnd(), rnd()};
            for (int k = 0; k < 6; ++k)
                if (heights && (trial + k) % 3 != 0) params[k][0] = 1.0f + 9.0f * rnd(), params[k][1] = trial % 5 ? 1.0f : 0.5f, params[k][2] = 1.0f;
            // Each layer's coordinates from the alpha maps' (c41 + k), its height a function of where it is read.
            const Vec ac = {rnd(), rnd(), 0.0f, 1.0f};
            float     coordRow[8][4];
            Vec       coord[8];
            for (int k = 0; k < 8; ++k)
            {
                coordRow[k][0] = 1.0f + static_cast<float>(k), coordRow[k][1] = 2.0f - 0.1f * static_cast<float>(k);
                coordRow[k][2] = 0.1f * static_cast<float>(k), coordRow[k][3] = -0.2f * static_cast<float>(k);
                coord[k] = {ac[0] * coordRow[k][0] + coordRow[k][2], ac[1] * coordRow[k][1] + coordRow[k][3], 0.0f, 1.0f};
            }
            const auto height = [](int layer, const Vec& at) {
                const float v = std::sin(at[0] * 3.1f + at[1] * 1.7f + static_cast<float>(layer));
                return v * v;
            };
            const auto draw = [&](const std::vector<uint32_t>& program, int pass, Vec& colour) {
                Machine m;
                for (int k = 0; k < 4; ++k) m.v[2 + k] = coord[pass * 4 + k];
                m.v[6] = ac;
                m.sample = [&](uint32_t s, const Vec& at) -> Vec {
                    if (s < 4) return t[pass * 4 + s];
                    if (s == 4) return {alpha[1], alpha[2], alpha[3], 1.0f};
                    if (s == shader::kExtraAlphaSampler) return {alpha[4], alpha[5], alpha[6], alpha[7]};
                    if (s == shader::kExtraHeightSampler || s == shader::kExtraHeightSampler + 1)
                        return {0, 0, 0, height(4 + static_cast<int>(s - shader::kExtraHeightSampler), at)};
                    if (s >= shader::kHeightSampler && s < shader::kHeightSampler + 4)
                        return {0, 0, 0, height(static_cast<int>(s - shader::kHeightSampler), at)};
                    return {};
                };
                for (int k = 0; k < 8; ++k) std::memcpy(&m.c[shader::kParamRow + k], params[k], 16);
                m.c[shader::kPassRow] = {static_cast<float>(pass), extras ? 1.0f : 0.0f, 0.0f, 0.0f};
                for (int k = 0; k < 8; ++k) std::memcpy(&m.c[shader::kCoordRow + k], coordRow[k], 16);
                if (!m.Run(program)) return false;
                colour = m.o;
                return true;
            };
            Vec first{}, second{}, stock{};
            ran = ran && draw(out, 0, first) && draw(four, 0, stock) && (!extras || draw(out, 1, second));
            Vec final = first;
            if (extras)
                for (int c = 0; c < 3; ++c) final[c] = second[3] * second[c] + (1.0f - second[3]) * first[c];
            // Worked out apart: the eight layers' weights, each over the ones below, cut and shared out.
            float wgt[8] = {}, rem = 1.0f, pct[8] = {}, top = 0.0f, sum = 0.0f;
            if (sums)
            {
                float total = 0.0f;
                for (int k = 1; k < 8; ++k) total += wgt[k] = alpha[k];
                wgt[0] = 1.0f - std::min(1.0f, total);
            }
            else
            {
                for (int k = 7; k >= 1; --k)
                {
                    wgt[k] = alpha[k] * rem;
                    rem *= 1.0f - alpha[k];
                }
                wgt[0] = rem;
            }
            bool any = false;
            for (int k = 0; k < 8; ++k)
            {
                any    = any || params[k][2] != 0.0f;
                pct[k] = wgt[k] * (params[k][2] != 0.0f ? height(k, coord[k]) * params[k][0] + params[k][1] : 1.0f);
                top    = std::max(top, pct[k]);
            }
            for (int k = 0; k < 8; ++k)
            {
                if (any) pct[k] *= 1.0f - std::min(1.0f, std::max(0.0f, top - pct[k]));
                sum += pct[k];
            }
            Vec want{};
            for (int k = 0; k < 8; ++k)
                for (int c = 0; c < 3; ++c) want[c] += pct[k] / std::max(sum, 1e-6f) * t[k][c];
            for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(final[c] - want[c]));
            if (!extras && !heights) same = same && std::memcmp(&first, &stock, sizeof first) == 0;
        }
        char what[200];
        std::snprintf(what, sizeof what, "eight layers%s in two draws: the blended colour is the eight-layer height blend's, across both draws (worst %.2g over 600 pixels)",
                      sums ? ", summed," : "", worst);
        Check(ran && worst < 5e-5f, what);
        Check(ran && same, sums ? "a summing four-layer chunk without heights or extra layers draws bit for bit as the client's program"
                                : "a four-layer chunk without heights or extra layers draws bit for bit as the client's program");
        }
    }

    // --- Layers past four -----------------------------------------------------------------------

    /// MCLY and MCAL of chunk 13 of artifact-acquisitionarmsholyshadow_8_49_tex0.adt (retail): five
    /// layers, every alpha map run-length coded (flags 0x300), MCAL 512 bytes.
    const uint8_t kRetailChunk[] = {
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
        0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF,
        0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF,
        0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF,
        0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF,
        0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
        0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
    };

    void SplitCases()
    {
        Adt file;
        file.Chunk(adt::kMVER, Words({18}));
        const std::string names = std::string("t\\a.blp") + '\0' + "t\\b.blp" + '\0';
        file.Chunk(adt::kMTEX, std::vector<uint8_t>(names.begin(), names.end()));
        Adt mcnk;
        mcnk.Chunk(adt::kMCLY, std::vector<uint8_t>(kRetailChunk, kRetailChunk + 80));
        mcnk.Chunk(adt::kMCAL, std::vector<uint8_t>(kRetailChunk + 80, kRetailChunk + sizeof kRetailChunk));
        file.Chunk(adt::kMCNK, mcnk.bytes);
        const auto chunks = adt::ReadSplitChunks(adt::Reader(file.bytes.data(), file.bytes.size()));
        Check(chunks.size() == 1 && chunks[0].layers.size() == 5 && chunks[0].layers[4].texture == 3 && chunks[0].layers[4].offset == 384 &&
                  chunks[0].mcalSize == 512,
              "a retail split chunk: five MCLY layers and its MCAL");
        uint8_t a[4096];
        uint32_t sum = 0;
        const bool decoded = adt::DecodeAlpha(chunks[0], 1, a);
        for (uint8_t v : a) sum += v;
        Check(decoded && a[0] == 255 && sum / 4096 == 223, "its layer 1's map decoded from runs: 64 x 64, as retail drew it");
        Check(adt::DecodeAlpha(chunks[0], 4, a) && a[100] == 0 && !adt::DecodeAlpha(chunks[0], 0, a),
              "layer 4's map decoded; layer 0 has none");
        adt::SplitChunk raw;
        std::vector<uint8_t> bytes(4096);
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i * 7);
        raw.layers = {{0, 0, 0}, {1, 0x100, 0}};
        raw.mcal = bytes.data();
        raw.mcalSize = 4096;
        Check(adt::DecodeAlpha(raw, 1, a) && a[4095] == static_cast<uint8_t>(4095 * 7), "an uncompressed map: 4096 bytes as they are");
        raw.mcalSize = 2048;
        Check(adt::DecodeAlpha(raw, 1, a) && a[0] == (bytes[0] & 15) * 17 && a[1] == (bytes[0] >> 4) * 17, "a map of four bits: low nibble first, times 17");
        raw.mcalSize = 100;
        Check(!adt::DecodeAlpha(raw, 1, a), "a map past MCAL: none");
    }

    /// A root ADT of names @p root, and its split file whose chunk 0 holds @p split's layers.
    void ExtraCases()
    {
        const auto names = [](std::initializer_list<const char*> list) {
            std::string out;
            for (const char* n : list) (out += n) += '\0';
            return std::vector<uint8_t>(out.begin(), out.end());
        };
        Adt root;
        root.Chunk(adt::kMVER, Words({18}));
        root.Chunk(adt::kMHDR, std::vector<uint8_t>(64, 0));
        root.Chunk(adt::kMTEX, names({"T\\a.blp", "T\\b.blp", "T\\c.blp", "T\\d.blp"}));
        Adt split;
        split.Chunk(adt::kMVER, Words({18}));
        split.Chunk(adt::kMTEX, names({"t\\A.blp", "t\\b.blp", "t\\c.blp", "t\\d.blp", "t\\e.blp", "t\\f.blp"}));
        Adt mtxp;
        for (uint32_t i = 0; i < 6; ++i)
        {
            mtxp.U32(i == 4 ? 0x20u : 0u);
            mtxp.F32(i == 4 ? 6.0f : 0.0f);
            mtxp.F32(1.0f);
            mtxp.U32(0);
        }
        split.Chunk(adt::kMTXP, mtxp.bytes);
        Adt mcnk;
        Adt mcly;
        for (uint32_t i = 0; i < 6; ++i)
        {
            mcly.U32(i);
            mcly.U32(i ? 0x100u : 0u);
            mcly.U32(i ? (i - 1) * 4096u : 0u);
            mcly.U32(0);
        }
        mcnk.Chunk(adt::kMCLY, mcly.bytes);
        std::vector<uint8_t> mcal(5 * 4096);
        for (size_t i = 0; i < mcal.size(); ++i) mcal[i] = static_cast<uint8_t>(i / 4096 * 40 + i % 3);
        mcnk.Chunk(adt::kMCAL, mcal);
        for (int c = 0; c < 3; ++c) split.Chunk(adt::kMCNK, mcnk.bytes);
        std::vector<std::string> read;
        Store       store;
        HeightBlend blend(store, [](const std::string& n) { return n.find("e_h") != std::string::npos; });
        // The map's WDT: MPHD flags height texturing and big alpha, as WorldGen writes it.
        Adt wdt;
        wdt.Chunk(adt::kMVER, Words({18}));
        wdt.Chunk(adt::kMPHD, Words({0x84, 0, 0, 0, 0, 0, 0, 0}));
        ExtraLayers extra(store, &blend, [&](const std::string& name, std::vector<uint8_t>& bytes) {
            read.push_back(name);
            if (name != "World\\Maps\\M\\M_32_48_tex0.adt") return false;
            bytes = split.bytes;
            return true;
        });
        Check(ExtraLayers::SplitName("W\\M_1_2.ADT") == "W\\M_1_2_tex0.adt" && ExtraLayers::SplitName("x.wdt").empty(), "a tile's split texture file is its name with _tex0");
        extra.TileLoaded("World\\Maps\\M\\M_32_48.adt", 32, 48);
        wxl_terrain_chunk chunk{};
        chunk.struct_size = sizeof chunk;
        chunk.tile_x = 32, chunk.tile_y = 48;
        chunk.chunk_x = 32 * 16 + 1, chunk.chunk_y = 48 * 16; // chunk 1 of the tile
        chunk.layers = 4;
        for (uint32_t k = 0; k < 4; ++k) chunk.texture[k] = k;
        chunk.file          = root.bytes.data();
        chunk.file_size     = static_cast<uint32_t>(root.bytes.size());
        chunk.map_file      = wdt.bytes.data();
        chunk.map_file_size = static_cast<uint32_t>(wdt.bytes.size());
        static wxl_terrain_extra_layers out;
        out = wxl_terrain_extra_layers{};
        extra.Answer(chunk, out);
        Check(read.size() == 1 && extra.Files() == 1 && out.count == 2 && std::string(out.layer[0].texture) == "t\\e.blp" &&
                  out.layer[0].texture_scale == 4.0f && out.layer[1].texture_scale == 1.0f,
              "a chunk of six layers whose first four are the root's: layers 4 and 5, their textures and scales");
        Check(out.layer[0].extra_texture && std::string(out.layer[0].extra_texture) == "t\\e_h.blp" && out.layer[0].params[0] == 6.0f &&
                  !out.layer[1].extra_texture,
              "layer 4's height as height_blend gives it; layer 5 has none");
        Check(out.alpha[0] == 3 * 40 + 12288 % 3 && out.alpha[1] == 4 * 40 + 16384 % 3 && out.alpha[4 * 7 + 1] == 4 * 40 + 16391 % 3 &&
                  out.alpha[3] == 0,
              "their alpha maps four bytes a texel, layer 4 in the first");
        chunk.texture[2] = 3;
        out = wxl_terrain_extra_layers{};
        extra.Answer(chunk, out);
        Check(out.count == 0 && extra.Mismatched() == 1, "a chunk whose first four are not the root's: drawn as the client draws it");
        chunk.texture[2] = 2;
        chunk.chunk_x    = 32 * 16 + 5;
        out = wxl_terrain_extra_layers{};
        extra.Answer(chunk, out);
        Check(out.count == 0, "a chunk past those the split file holds: none");
        extra.TileLoaded("World\\Maps\\M\\M_31_48.adt", 31, 48);
        chunk.tile_x = 31;
        chunk.chunk_x = 31 * 16;
        out = wxl_terrain_extra_layers{};
        extra.Answer(chunk, out);
        Check(read.size() == 2 && out.count == 0 && extra.Files() == 1, "a tile without its split file: its chunks keep their four");
    }

    void MapCases()
    {
        Adt flagged, plain;
        for (Adt* a : {&flagged, &plain}) a->Chunk(adt::kMVER, Words({18}));
        flagged.Chunk(adt::kMPHD, Words({0x84, 0, 0, 0, 0, 0, 0, 0}));
        plain.Chunk(adt::kMPHD, Words({0x4, 0, 0, 0, 0, 0, 0, 0}));
        uint32_t flags = 0;
        Check(adt::MapFlags(adt::Reader(flagged.bytes.data(), flagged.bytes.size()), &flags) && flags == 0x84 &&
                  !adt::MapFlags(adt::Reader(nullptr, 0), &flags),
              "a WDT's MPHD flags read; no WDT, none");
        Store       store;
        HeightBlend strict(store, [](const std::string&) { return true; }), loose(store, [](const std::string&) { return true; }, false);
        const auto  size = [](const Adt& a) { return static_cast<uint32_t>(a.bytes.size()); };
        Check(strict.MapAllows(flagged.bytes.data(), size(flagged)) && !strict.MapAllows(plain.bytes.data(), size(plain)) &&
                  !strict.MapAllows(nullptr, 0),
              "the height blend on a map flagged 0x80 only, as retail decides it");
        Check(loose.MapAllows(plain.bytes.data(), size(plain)) && loose.MapAllows(nullptr, 0), "require_wdt_flag = 0: on every map");
        // The map's flags decide an uncompressed map's size.
        adt::SplitChunk chunk;
        std::vector<uint8_t> bytes(4096 + 2048, 0x31);
        chunk.layers = {{0, 0, 0}, {1, 0x100, 0}, {2, 0x100, 2048}};
        chunk.mcal = bytes.data();
        chunk.mcalSize = static_cast<uint32_t>(bytes.size()) - 1;
        uint8_t a[4096];
        Check(adt::DecodeAlpha(chunk, 1, a, 0) && a[1] == 3 * 17 && adt::DecodeAlpha(chunk, 1, a, 1) && a[1] == 0x31 &&
                  !adt::DecodeAlpha(chunk, 2, a, 1) && adt::DecodeAlpha(chunk, 2, a, 0),
              "an uncompressed map: 2048 bytes of four bits without 0x4 or 0x80, 4096 with either");
        Options o = ParseOptions("[height_blend]\nrequire_wdt_flag = false\n");
        Check(!o.require_wdt_flag && o.problems.empty() && ParseOptions("").require_wdt_flag, "[height_blend] require_wdt_flag, on by default");
    }

    /// The programs of maps with big alpha sum their layers: the blend's shares go back as their
    /// alphas, not as lerps. Heights from a real _h, filtered, read where each layer is.
    void BigAlphaCases()
    {
        uint32_t seed = 777;
        const auto rnd = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / 16777216.0f;
        };
        for (uint32_t layers = 2; layers <= 4; ++layers)
        {
            const std::vector<uint32_t> stock = BigAlphaProgram(layers);
            std::vector<uint32_t>       out;
            shader::Found               found;
            std::string                 why;
            char                        what[200];
            const bool made = shader::BlendByHeight(stock.data(), stock.size() * 4, out, found, why);
            std::snprintf(what, sizeof what, "%u layers, big alpha: rewritten as a summing program", layers);
            Check(made && found.sums && found.heights == (layers == 4 ? 6u : layers), what);
            if (!made) continue;
            float worst = 0.0f;
            bool  same = true, ran = true;
            for (int trial = 0; trial < 500; ++trial)
            {
                float a[3] = {}, params[4][4] = {};
                float left = 1.0f;
                for (uint32_t k = 0; k + 1 < layers; ++k)
                {
                    a[k] = rnd() * left; // retail's alphas: their sum at most 1
                    left -= a[k];
                }
                Vec at[5], t[4];
                for (int k = 0; k < 5; ++k) at[k] = {rnd() * 4.0f, rnd() * 4.0f, 0.0f, 1.0f};
                for (Vec& x : t) x = {rnd(), rnd(), rnd(), rnd()};
                const float lod     = rnd();
                const bool  heights = trial % 4 != 0;
                for (uint32_t k = 0; k < layers; ++k)
                    if (heights && (trial + k) % 3 != 0) params[k][0] = 2.0f + 10.0f * rnd(), params[k][1] = 1.0f, params[k][2] = 1.0f;
                const auto run = [&](const std::vector<uint32_t>& program, Vec& colour) {
                    Machine m;
                    for (int k = 0; k < 5; ++k) m.v[2 + k] = at[k];
                    m.sample = [&](uint32_t sm, const Vec& c) -> Vec {
                        if (sm < layers) return t[sm];
                        if (sm == layers) return {a[0], a[1], a[2], 1.0f};
                        if (sm >= shader::kHeightSampler && sm < shader::kHeightSampler + 4) return {0, 0, 0, HeightAt(c[0], c[1], lod)};
                        return {};
                    };
                    for (uint32_t k = 0; k < 4; ++k) std::memcpy(&m.c[shader::kParamRow + k], params[k], 16);
                    if (!m.Run(program)) return false;
                    colour = m.o;
                    return true;
                };
                Vec before{}, after{};
                ran = ran && run(stock, before) && run(out, after);
                float wgt[4] = {}, pct[4] = {}, top = 0.0f, sum = 0.0f, want[3] = {};
                wgt[0] = 1.0f - std::min(1.0f, a[0] + a[1] + a[2]);
                for (uint32_t k = 1; k < layers; ++k) wgt[k] = a[k - 1];
                bool any = false;
                for (uint32_t k = 0; k < layers; ++k)
                {
                    any    = any || params[k][2] != 0.0f;
                    pct[k] = wgt[k] * (params[k][2] != 0.0f ? HeightAt(at[k][0], at[k][1], lod) * params[k][0] + params[k][1] : 1.0f);
                    top    = std::max(top, pct[k]);
                }
                for (uint32_t k = 0; k < layers; ++k)
                {
                    if (any) pct[k] *= 1.0f - std::min(1.0f, std::max(0.0f, top - pct[k]));
                    sum += pct[k];
                }
                for (uint32_t k = 0; k < layers; ++k)
                    for (int c = 0; c < 3; ++c) want[c] += (any ? pct[k] / std::max(sum, 1e-6f) : wgt[k]) * t[k][c];
                for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(after[c] - want[c]));
                if (!heights) same = same && std::memcmp(&before, &after, sizeof before) == 0;
            }
            std::snprintf(what, sizeof what, "%u layers, big alpha: the colour is the summed height blend with a real _h, filtered (worst %.2g)", layers, worst);
            Check(ran && worst < 5e-5f, what);
            std::snprintf(what, sizeof what, "%u layers, big alpha: a chunk without heights draws bit for bit as the client's program", layers);
            Check(ran && same, what);
        }
        // Both mixes or neither: left as it is.
        std::vector<uint32_t> odd = TerrainProgram(3);
        odd.pop_back();
        odd.insert(odd.end(), {0x08u | (3u << 24), D(0, 9, 1), S(0, 3), S(2, 0, 0x00), 0x0000FFFFu});
        std::vector<uint32_t> out;
        shader::Found         found;
        std::string           why;
        Check(!shader::BlendByHeight(odd.data(), odd.size() * 4, out, found, why) && why.find("neither") != std::string::npos,
              "a program that both lerps and sums its alphas: left as it is");
    }

    /// MTEX, MTXP and chunk (14, 4) of DawnlightWorld_38_19.adt as WorldGen wrote it: five layers in
    /// the root, no _tex0 beside it, uncompressed 4096-byte alpha maps (the map's WDT says 0x84).
    const uint8_t kDawnlightRoot[] = {
#include "modern_adt_root_chunk.inc"
    };

    void RootExtraCases()
    {
        const uint32_t mtexSize = kDawnlightRoot[4] | kDawnlightRoot[5] << 8 | kDawnlightRoot[6] << 16 | kDawnlightRoot[7] << 24;
        const size_t   mtxpAt = 8 + mtexSize;
        const uint32_t mtxpSize = kDawnlightRoot[mtxpAt + 4] | kDawnlightRoot[mtxpAt + 5] << 8 | kDawnlightRoot[mtxpAt + 6] << 16 |
                                  kDawnlightRoot[mtxpAt + 7] << 24;
        const size_t   mcnkAt = mtxpAt + 8 + mtxpSize;
        // The root: MVER, MHDR (MTEX's slot), MTEX, MTXP, chunks 0 to 77 empty, then the real chunk 78.
        Adt root;
        root.Chunk(adt::kMVER, Words({18}));
        const size_t mhdr = root.Chunk(adt::kMHDR, std::vector<uint8_t>(64, 0));
        const size_t mtex = root.bytes.size();
        root.bytes.insert(root.bytes.end(), kDawnlightRoot, kDawnlightRoot + mcnkAt);
        for (int c = 0; c < 78; ++c) root.Chunk(adt::kMCNK, std::vector<uint8_t>(128, 0));
        const size_t real = root.bytes.size();
        root.bytes.insert(root.bytes.end(), kDawnlightRoot + mcnkAt, kDawnlightRoot + sizeof kDawnlightRoot);
        const uint32_t v = static_cast<uint32_t>(mtex - (mhdr + 8));
        std::memcpy(&root.bytes[mhdr + 8 + adt::kSlotMtex], &v, 4);
        // The runtime has cut the client's count to 4; MCLY still holds the five.
        uint32_t named = 0, four = 4;
        std::memcpy(&named, &root.bytes[real + 8 + 0x0C], 4);
        std::memcpy(&root.bytes[real + 8 + 0x0C], &four, 4);
        adt::SplitChunk chunk;
        Check(named == 5 && adt::ReadRootChunk(adt::Reader(root.bytes.data(), root.bytes.size()), 78, chunk) && chunk.layers.size() == 5 &&
                  chunk.mcalSize >= 4 * 4096,
              "a WorldGen root chunk of five layers: MCLY read by its own size, past the cut count, with its MCAL");
        Adt wdt;
        wdt.Chunk(adt::kMVER, Words({18}));
        wdt.Chunk(adt::kMPHD, Words({0x84, 0, 0, 0, 0, 0, 0, 0}));
        Store       store;
        HeightBlend blend(store, [](const std::string&) { return true; });
        ExtraLayers extra(store, &blend, [](const std::string&, std::vector<uint8_t>&) { return false; });
        extra.TileLoaded("World\\Maps\\DawnlightWorld\\DawnlightWorld_38_19.adt", 38, 19);
        wxl_terrain_chunk info{};
        info.struct_size = sizeof info;
        info.tile_x = 38, info.tile_y = 19;
        info.chunk_x = 38 * 16 + 14, info.chunk_y = 19 * 16 + 4;
        info.layers  = 4;
        for (uint32_t k = 0; k < 4; ++k) info.texture[k] = chunk.layers[k].texture;
        info.file          = root.bytes.data();
        info.file_size     = static_cast<uint32_t>(root.bytes.size());
        info.map_file      = wdt.bytes.data();
        info.map_file_size = static_cast<uint32_t>(wdt.bytes.size());
        static wxl_terrain_extra_layers out;
        out = wxl_terrain_extra_layers{};
        extra.Answer(info, out);
        const Area& area = store.AreaOf(38, 19, info.file, info.file_size);
        const std::string name = area.names[chunk.layers[4].texture];
        bool same = out.count == 1;
        for (uint32_t t = 0; same && t < 4096; ++t) same = out.alpha[t * 4] == chunk.mcal[chunk.layers[4].offset + t] && out.alpha[t * 4 + 1] == 0;
        Check(out.count == 1 && name == out.layer[0].texture && extra.FromRoot() == 1 && name.find("grass07") != std::string::npos,
              "no _tex0 beside it: layer 4 answered from the root itself, its texture from the root's MTEX");
        Check(same, "its alpha map the root's MCAL as WorldGen wrote it, 4096 bytes (MPHD 0x84)");
        Check(out.layer[0].extra_texture && out.layer[0].params[2] == 1.0f, "its height as height_blend gives it on a map flagged 0x80");
    }

    /// A split tile made up: a root of 256 MCNKs with their header, a _tex0 naming two textures with
    /// one layer of alpha a chunk, an _obj0 placing one doodad referred to by chunk 0.
    void SurveyCases()
    {
        const auto tile = [](bool byId, uint32_t ref) {
            adt::TileFiles f;
            Adt root, tex, obj;
            root.Chunk(adt::kMVER, Words({18}));
            root.Chunk(adt::kMHDR, std::vector<uint8_t>(64, 0));
            for (int c = 0; c < 256; ++c) root.Chunk(adt::kMCNK, std::vector<uint8_t>(128, 0));
            tex.Chunk(adt::kMVER, Words({18}));
            if (byId) tex.Chunk(adt::Id("MDID"), Words({123456, 654321}));
            else
            {
                const std::string names = std::string("t\\a.blp") + '\0' + "t\\b.blp" + '\0';
                tex.Chunk(adt::kMTEX, std::vector<uint8_t>(names.begin(), names.end()));
            }
            for (int c = 0; c < 256; ++c)
            {
                Adt mcnk;
                mcnk.Chunk(adt::kMCLY, Words({0, 0, 0, 0, 1, 0x100, 0, 0}));
                mcnk.Chunk(adt::kMCAL, std::vector<uint8_t>(4096, 7));
                tex.Chunk(adt::kMCNK, mcnk.bytes);
            }
            obj.Chunk(adt::kMVER, Words({18}));
            const std::string m2 = std::string("w\\tree.m2") + '\0';
            obj.Chunk(adt::Id("MMDX"), std::vector<uint8_t>(m2.begin(), m2.end()));
            obj.Chunk(adt::Id("MMID"), Words({0}));
            std::vector<uint8_t> mddf(36, 0);
            mddf[34] = byId ? 0x40 : 0;
            obj.Chunk(adt::Id("MDDF"), mddf);
            Adt refs;
            refs.Chunk(adt::Id("MCRD"), Words({ref}));
            obj.Chunk(adt::kMCNK, refs.bytes);
            f.root = root.bytes;
            f.tex0 = tex.bytes;
            f.obj0 = obj.bytes;
            return f;
        };
        adt::SurveyStats stats;
        Check(adt::SurveyTile("named", tile(false, 0), 0x84, stats) == adt::Era::split && stats.errors == 0 && stats.alphaMaps == 256 &&
                  stats.doodads == 1,
              "a split tile by name: root, _tex0 and _obj0 read as one, every alpha map decoded, no error");
        Check(adt::SurveyTile("ids", tile(true, 0), 0x84, stats) == adt::Era::file_ids && stats.errors == 0 && stats.doodadsById == 1,
              "a tile by FileDataID (MDID, MDDF flag 0x40)");
        Check(adt::SurveyTile("broken", tile(false, 3), 0x84, stats) == adt::Era::split && stats.errors == 1 &&
                  stats.firstErrors.back().find("MCRD names placement 3 of 1") != std::string::npos,
              "a chunk referring to a placement past MDDF is an error, named");
    }

    void TileSetCases()
    {
        // A split root (MHDR of zeros, MCNKs) and a monolithic one (MCIN).
        Adt split;
        split.Chunk(adt::kMVER, Words({18}));
        split.Chunk(adt::kMHDR, std::vector<uint8_t>(64, 0));
        split.Chunk(adt::kMCNK, std::vector<uint8_t>(128, 0));
        Adt mono;
        mono.Chunk(adt::kMVER, Words({18}));
        std::vector<uint8_t> mhdr(64, 0);
        mhdr[4] = 0x40;
        mono.Chunk(adt::kMHDR, mhdr);
        mono.Chunk(adt::kMCNK, std::vector<uint8_t>(128, 0));
        Check(TileSet::IsSplitRoot(split.bytes.data(), static_cast<uint32_t>(split.bytes.size())) &&
                  !TileSet::IsSplitRoot(mono.bytes.data(), static_cast<uint32_t>(mono.bytes.size())) && !TileSet::IsSplitRoot(nullptr, 0),
              "a root without MCIN is split; one with MCIN, or nothing, is not");
        Check(TileSet::SplitName("W\\M_1_2.ADT", "_obj0") == "W\\M_1_2_obj0.adt" && TileSet::SplitName("x.wdt", "_tex0").empty(),
              "a tile's files are its name with their suffix");

        // The tex0 of a split tile names its own textures.
        Adt tex0;
        tex0.Chunk(adt::kMVER, Words({18}));
        const std::string names("t\\x.blp\0t\\y.blp\0", 16);
        tex0.Chunk(adt::kMTEX, std::vector<uint8_t>(names.begin(), names.end()));
        tex0.Chunk(adt::kMTXF, Words({0x20, 0}));
        std::vector<std::string> read;
        TileSet tiles([&](const std::string& name, std::vector<uint8_t>& bytes) {
            read.push_back(name);
            if (name == "W\\M_3_4_tex0.adt") bytes = tex0.bytes;
            else if (name == "W\\M_3_4_obj0.adt") bytes = Words({1, 2});
            else return false;
            return true;
        });
        const TileSet::Files& files = tiles.Loaded("W\\M_3_4.adt", 3, 4, split.bytes.data(), static_cast<uint32_t>(split.bytes.size()));
        Check(files.split && files.tex0 == tex0.bytes && files.obj0.size() == 8 && files.obj1.empty() && read.size() == 3,
              "a split root's _tex0, _obj0 and _obj1 are read (the absent one left empty)");
        tiles.Loaded("W\\M_5_4.adt", 5, 4, mono.bytes.data(), static_cast<uint32_t>(mono.bytes.size()));
        Check(read.size() == 4 && !tiles.Of(5, 4)->split, "a root with MCIN has its _tex0 read only");

        Store store(adt::Prefer::automatic, &tiles);
        const Area& area = store.AreaOf(3, 4, split.bytes.data(), static_cast<uint32_t>(split.bytes.size()));
        Check(area.names.size() == 2 && area.names[1] == "t\\y.blp" && area.textures.size() == 2 && area.textures[0].scale == 2,
              "a split tile's textures and scales are its _tex0's");

        SplitTiles provider(tiles);
        wxl_terrain_tile tile{};
        tile.struct_size = sizeof tile;
        tile.tile_x = 3, tile.tile_y = 4;
        tile.path      = wxl_str{"W\\M_3_4.adt", 11};
        tile.file      = split.bytes.data();
        tile.file_size = static_cast<uint32_t>(split.bytes.size());
        wxl_terrain_tile_files out{};
        out.struct_size = sizeof out;
        provider.Answer(tile, out);
        Check(read.size() == 4 && out.data[WXL_TERRAIN_TILE_TEX0] == files.tex0.data() && out.size[WXL_TERRAIN_TILE_OBJ0] == 8 &&
                  !out.data[WXL_TERRAIN_TILE_OBJ1] && provider.Tiles() == 1,
              "the provider answers the files read with the root, without reading them again");
        tile.tile_x = 7;
        tile.path   = wxl_str{"W\\M_7_4.adt", 11};
        out         = wxl_terrain_tile_files{};
        provider.Answer(tile, out);
        Check(read.size() == 7 && !out.data[WXL_TERRAIN_TILE_TEX0] && provider.Missing() == 1,
              "a tile asked before it was read has its files read then; none found, none answered");
    }

    void OptionCases()
    {
        Options o = ParseOptions("; comment\n[texture_scale]\nenabled = 0\nsource = MTXF\n");
        Check(!o.texture_scale && o.source == adt::Prefer::mtxf && o.problems.empty(), "the options read");
        o = ParseOptions("[texture_scale]\nenabled = yes\ncolour = red\n[height]\nx = 1\n");
        Check(o.texture_scale && o.problems.size() == 3, "lines not understood are kept as problems, the defaults stand");
        Check(ParseOptions("").texture_scale && ParseOptions("").source == adt::Prefer::automatic, "no file: on, auto");
        o = ParseOptions("[height_blend]\nenabled = 0\n");
        Check(!o.height_blend && o.texture_scale && o.problems.empty() && ParseOptions("").height_blend, "[height_blend] enabled, on by default");
        o = ParseOptions("[split_tiles]\nenabled = 0\n");
        Check(!o.split_tiles && o.problems.empty() && ParseOptions("").split_tiles, "[split_tiles] enabled, on by default");
    }
}

int main()
{
    ReaderCases();
    FeatureCases();
    HeightBlendCases();
    ShaderCases();
    SplitCases();
    ExtraCases();
    MapCases();
    RootExtraCases();
    SurveyCases();
    BigAlphaCases();
    EightLayerCases();
    OptionCases();
    TileSetCases();
    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
