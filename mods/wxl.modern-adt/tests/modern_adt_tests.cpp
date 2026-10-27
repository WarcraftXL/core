// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.modern-adt's reading layer, store, texture scale and options, on ADTs made up byte by byte:
// a root ADT with MVER, MHDR, MTEX, MTXF (its bits 4 to 7) and a trailing MTXP no MHDR slot points
// to, as WorldGen and retail write them; a stock one (MTXF bit 0 only); a file cut short. The
// height blend: its answers (a retail MTXP's numbers, from arakkoadungeon_26_30_tex0.adt), and the
// rewritten pixel program run by a small ps_3_0 interpreter against the blend's formula worked out
// apart: the Direct3D 9 path's CPU reference (the Vulkan renderer's is held on lavapipe).

#include "Options.hpp"
#include "adt/Reader.hpp"
#include "features/ExtraLayers.hpp"
#include "features/HeightBlend.hpp"
#include "features/TextureScale.hpp"
#include "shader/Ps3.hpp"
#include "store/Store.hpp"

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

        // Four layers: eight, in two draws blended as the device blends them.
        const std::vector<uint32_t> four = TerrainProgram(4);
        Check(BlendByHeight(four.data(), four.size() * 4, out, found, why) && found.extras && found.heights == 6,
              "a four-layer program is rewritten for eight layers, six with their heights");
        float worst = 0.0f;
        bool  same = true, ran = true;
        for (int trial = 0; trial < 600; ++trial)
        {
            float alpha[8] = {}, params[8][4] = {};
            Vec   t[8];
            for (int k = 1; k < 8; ++k) alpha[k] = trial % 9 == 0 ? std::round(rnd()) : rnd();
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
            for (int k = 7; k >= 1; --k)
            {
                wgt[k] = alpha[k] * rem;
                rem *= 1.0f - alpha[k];
            }
            wgt[0]   = rem;
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
        std::snprintf(what, sizeof what, "eight layers in two draws: the blended colour is the eight-layer height blend's, across both draws (worst %.2g over 600 pixels)", worst);
        Check(ran && worst < 5e-5f, what);
        Check(ran && same, "a four-layer chunk without heights or extra layers draws bit for bit as the client's program");
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

    void OptionCases()
    {
        Options o = ParseOptions("; comment\n[texture_scale]\nenabled = 0\nsource = MTXF\n");
        Check(!o.texture_scale && o.source == adt::Prefer::mtxf && o.problems.empty(), "the options read");
        o = ParseOptions("[texture_scale]\nenabled = yes\ncolour = red\n[height]\nx = 1\n");
        Check(o.texture_scale && o.problems.size() == 3, "lines not understood are kept as problems, the defaults stand");
        Check(ParseOptions("").texture_scale && ParseOptions("").source == adt::Prefer::automatic, "no file: on, auto");
        o = ParseOptions("[height_blend]\nenabled = 0\n");
        Check(!o.height_blend && o.texture_scale && o.problems.empty() && ParseOptions("").height_blend, "[height_blend] enabled, on by default");
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
    OptionCases();
    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
