// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.modern-adt's reading layer, store, texture scale and options, on ADTs made up byte by byte:
// a root ADT with MVER, MHDR, MTEX, MTXF (its bits 4 to 7) and a trailing MTXP no MHDR slot points
// to, as WorldGen and retail write them; a stock one (MTXF bit 0 only); a file cut short.

#include "Options.hpp"
#include "adt/Reader.hpp"
#include "features/TextureScale.hpp"
#include "store/Store.hpp"

#include <cstdio>
#include <cstring>
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

    void OptionCases()
    {
        Options o = ParseOptions("; comment\n[texture_scale]\nenabled = 0\nsource = MTXF\n");
        Check(!o.texture_scale && o.source == adt::Prefer::mtxf && o.problems.empty(), "the options read");
        o = ParseOptions("[texture_scale]\nenabled = yes\ncolour = red\n[height]\nx = 1\n");
        Check(o.texture_scale && o.problems.size() == 3, "lines not understood are kept as problems, the defaults stand");
        Check(ParseOptions("").texture_scale && ParseOptions("").source == adt::Prefer::automatic, "no file: on, auto");
    }
}

int main()
{
    ReaderCases();
    FeatureCases();
    OptionCases();
    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
