// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The terrain pixel programs' height blend (see Ps3.hpp). DX9 shader bytecode: a version token,
// instructions (an opcode token whose bits 24 to 27 count the tokens after it; comments counted in
// bits 16 to 30), an end token. A register token: number in bits 0 to 10, type in bits 28 to 30
// and 11 to 12; a destination's write mask in bits 16 to 19, a source's swizzle in 16 to 23.

#include "shader/Ps3.hpp"

#include <cstring>
#include <map>
#include <set>

namespace wxl::modern_adt::shader
{
    namespace
    {
        constexpr uint32_t kVersionPs30 = 0xFFFF0300u;
        constexpr uint32_t kEnd         = 0x0000FFFFu;
        constexpr uint32_t kComment     = 0xFFFEu;

        enum Op : uint32_t
        {
            kMov = 0x01, kAdd = 0x02, kMad = 0x04, kMul = 0x05, kRcp = 0x06, kDp4 = 0x09, kMax = 0x0B,
            kDcl = 0x1F, kDef = 0x51, kDefI = 0x52, kDefB = 0x53, kCmp = 0x58, kTexld = 0x42,
        };
        enum Reg : uint32_t
        {
            kTemp = 0, kInput = 1, kConst = 2, kSampler = 10,
        };
        constexpr uint32_t kTex2d = 2;   ///< D3DSTT_2D in a sampler's dcl
        constexpr uint32_t kXyzw  = 0xE4; ///< the identity swizzle
        constexpr uint32_t kTemps = 32;  ///< ps_3_0's temporaries

        uint32_t TypeOf(uint32_t t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); }
        uint32_t NumOf(uint32_t t) { return t & 0x7FF; }
        uint32_t RegTok(uint32_t type, uint32_t num) { return 0x80000000u | ((type & 7) << 28) | (((type >> 3) & 3) << 11) | num; }
        uint32_t Dst(uint32_t type, uint32_t num, uint32_t mask = 0xF, bool sat = false)
        {
            return RegTok(type, num) | (mask << 16) | (sat ? 0x00100000u : 0u);
        }
        uint32_t Src(uint32_t type, uint32_t num, uint32_t swizzle = kXyzw, bool negate = false)
        {
            return RegTok(type, num) | (swizzle << 16) | (negate ? 0x01000000u : 0u);
        }
        /// One component everywhere: .xxxx (0) to .wwww (3).
        constexpr uint32_t Rep(uint32_t c) { return c | (c << 2) | (c << 4) | (c << 6); }
        uint32_t OpTok(uint32_t op, uint32_t count) { return op | (count << 24); }
        uint32_t FloatTok(float f)
        {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return u;
        }

        struct Ins
        {
            size_t   at;  ///< its opcode token
            uint32_t op;
            uint32_t len; ///< tokens after the opcode
        };

        class Emit
        {
        public:
            void operator()(uint32_t op, std::initializer_list<uint32_t> params)
            {
                words.push_back(OpTok(op, static_cast<uint32_t>(params.size())));
                words.insert(words.end(), params.begin(), params.end());
            }
            std::vector<uint32_t> words;
        };
    }

    bool BlendByHeight(const void* code, size_t size, std::vector<uint32_t>& out, Found& found, std::string& why)
    {
        found = Found{};
        if (!code || size < 8 || size % 4)
        {
            why = "no bytecode";
            return false;
        }
        std::vector<uint32_t> w(size / 4);
        std::memcpy(w.data(), code, size);
        if (w[0] != kVersionPs30)
        {
            why = "not ps_3_0";
            return false;
        }

        // The instructions, and what they use.
        std::vector<Ins>                  ins;
        std::map<uint32_t, uint32_t>      samplers; ///< declared: number -> texture type
        std::set<uint32_t>                consts;
        uint32_t                          temps = 0; ///< one past the highest temporary used
        size_t                            end   = 0;
        for (size_t i = 1; i < w.size();)
        {
            const uint32_t tok = w[i];
            if (tok == kEnd)
            {
                end = i;
                break;
            }
            const uint32_t op  = tok & 0xFFFF;
            const uint32_t len = op == kComment ? (tok >> 16) & 0x7FFF : (tok >> 24) & 0xF;
            if (i + 1 + len > w.size())
            {
                why = "an instruction runs past the end";
                return false;
            }
            ins.push_back(Ins{i, op, len});
            if (op == kDcl && len == 2)
            {
                if (TypeOf(w[i + 2]) == kSampler) samplers[NumOf(w[i + 2])] = (w[i + 1] >> 27) & 0xF;
            }
            else if ((op == kDef || op == kDefI || op == kDefB) && len >= 1)
                consts.insert(NumOf(w[i + 1]) | (TypeOf(w[i + 1]) << 16));
            else if (op != kComment)
                for (uint32_t k = 1; k <= len; ++k)
                {
                    const uint32_t t = w[i + k];
                    if (!(t & 0x80000000u)) continue;
                    if (TypeOf(t) == kTemp) temps = std::max(temps, NumOf(t) + 1);
                    if (TypeOf(t) == kConst) consts.insert(NumOf(t) | (kConst << 16));
                }
            i += 1 + len;
        }
        if (!end)
        {
            why = "no end token";
            return false;
        }

        // L layers: samplers 0 to L - 1 the layers' textures, L the alpha maps; above 4 the shadows'.
        uint32_t layers = 0;
        while (samplers.count(layers) && layers < 5) ++layers;
        if (layers < 3)
        {
            why = "fewer than two layers";
            return false;
        }
        --layers;
        found.layers = layers;
        for (uint32_t k = 0; k < 4; ++k)
            if (samplers.count(kHeightSampler + k))
            {
                why = "a height sampler is in use";
                return false;
            }
        constexpr uint32_t kK = kParamRow - 1; // c31: 0, 1, a small number
        for (uint32_t r = kK; r < kParamRow + 4; ++r)
            if (consts.count(r | (kConst << 16)))
            {
                why = "a constant the blend needs is in use";
                return false;
            }
        const uint32_t base = temps;
        if (base + 7 > kTemps)
        {
            why = "too few temporaries left";
            return false;
        }

        // Each layer's coordinates (its texture's read), and the read of the alpha maps.
        uint32_t coord[4] = {};
        bool     height[4] = {};
        size_t   alphaAt = 0, alphaReads = 0;
        uint32_t alpha = 0;
        for (const Ins& in : ins)
        {
            if (in.op != kTexld || in.len != 3 || TypeOf(w[in.at + 3]) != kSampler) continue;
            const uint32_t s = NumOf(w[in.at + 3]);
            if (s < layers && !coord[s] && TypeOf(w[in.at + 2]) == kInput)
            {
                coord[s]  = w[in.at + 2];
                height[s] = samplers[s] == kTex2d;
            }
            if (s == layers)
            {
                ++alphaReads;
                alphaAt = in.at;
                alpha   = w[in.at + 1];
            }
        }
        if (alphaReads != 1 || TypeOf(alpha) != kTemp || ((alpha >> 16) & 0x7) != 0x7)
        {
            why = "the alpha maps are not read once into a temporary";
            return false;
        }
        for (uint32_t k = 0; k < layers; ++k) found.heights += height[k] ? 1 : 0;
        if (!found.heights)
        {
            why = "no layer read in 2d at an input's coordinates";
            return false;
        }

        const uint32_t A = NumOf(alpha);
        const uint32_t W = base, H = base + 1, F = base + 2, P = base + 3, M = base + 4, D = base + 5, X = base + 6;
        const auto     c = [](uint32_t row, uint32_t comp, bool neg = false) { return Src(kConst, row, Rep(comp), neg); };
        const auto     t = [](uint32_t r, uint32_t comp, bool neg = false) { return Src(kTemp, r, Rep(comp), neg); };
        const auto     m = [](uint32_t comp) { return 1u << comp; };

        Emit head; // declarations, ahead of the program's own
        head(kDef, {Dst(kConst, kK), FloatTok(0.0f), FloatTok(1.0f), FloatTok(1.0e-6f), FloatTok(0.0f)});
        for (uint32_t k = 0; k < layers; ++k)
            if (height[k]) head(kDcl, {0x80000000u | (kTex2d << 27), Dst(kSampler, kHeightSampler + k)});

        Emit e; // after the alpha maps' read
        // w: each layer's share of the client's mix; X.x the part left under the layers above.
        e(kMov, {Dst(kTemp, W), c(kK, 0)});
        e(kMov, {Dst(kTemp, X, m(0)), c(kK, 1)});
        for (uint32_t k = layers - 1; k >= 1; --k)
        {
            e(kMul, {Dst(kTemp, W, m(k)), t(A, k - 1), t(X, 0)});
            e(kMad, {Dst(kTemp, X, m(0)), t(A, k - 1, true), t(X, 0), t(X, 0)});
        }
        e(kMov, {Dst(kTemp, W, m(0)), t(X, 0)});
        // h: the height textures' alpha.
        e(kMov, {Dst(kTemp, H), c(kK, 0)});
        for (uint32_t k = 0; k < layers; ++k)
            if (height[k])
            {
                e(kTexld, {Dst(kTemp, D), coord[k], Src(kSampler, kHeightSampler + k)});
                e(kMov, {Dst(kTemp, H, m(k)), t(D, 3)});
            }
        // f = 1 + e (h * scale + offset - 1), one constant register an instruction.
        e(kMov, {Dst(kTemp, F), c(kK, 1)});
        for (uint32_t k = 0; k < layers; ++k)
            if (height[k])
            {
                e(kMad, {Dst(kTemp, D, m(0)), t(H, k), c(kParamRow + k, 0), c(kParamRow + k, 1)});
                e(kAdd, {Dst(kTemp, D, m(0)), t(D, 0), c(kK, 1, true)});
                e(kMul, {Dst(kTemp, D, m(1)), t(D, 0), c(kParamRow + k, 2)});
                e(kAdd, {Dst(kTemp, F, m(k)), t(D, 1), c(kK, 1)});
            }
        // pct = w f, cut below the highest, shared out.
        e(kMul, {Dst(kTemp, P), Src(kTemp, W), Src(kTemp, F)});
        e(kMax, {Dst(kTemp, M, m(0)), t(P, 0), t(P, 1)});
        e(kMax, {Dst(kTemp, M, m(1)), t(P, 2), t(P, 3)});
        e(kMax, {Dst(kTemp, M, m(0)), t(M, 0), t(M, 1)});
        e(kAdd, {Dst(kTemp, D, 0xF, true), t(M, 0), Src(kTemp, P, kXyzw, true)});
        e(kAdd, {Dst(kTemp, D), c(kK, 1), Src(kTemp, D, kXyzw, true)});
        e(kMul, {Dst(kTemp, P), Src(kTemp, P), Src(kTemp, D)});
        e(kDp4, {Dst(kTemp, M, m(1)), Src(kTemp, P), c(kK, 1)});
        e(kMax, {Dst(kTemp, M, m(1)), t(M, 1), c(kK, 2)});
        e(kRcp, {Dst(kTemp, M, m(1)), t(M, 1)});
        e(kMul, {Dst(kTemp, P), Src(kTemp, P), t(M, 1)});
        // The shares back into the client's alphas: layer k's over what is left under the ones above.
        e(kMov, {Dst(kTemp, M, m(3)), c(kK, 1)});
        for (uint32_t k = layers - 1; k >= 1; --k)
        {
            e(kMax, {Dst(kTemp, M, m(2)), t(M, 3), c(kK, 2)});
            e(kRcp, {Dst(kTemp, M, m(2)), t(M, 2)});
            e(kMul, {Dst(kTemp, X, m(k - 1), true), t(P, k), t(M, 2)});
            e(kAdd, {Dst(kTemp, M, m(3)), t(M, 3), t(P, k, true)});
        }
        // No layer with its height in the chunk: the client's own alphas.
        e(kMov, {Dst(kTemp, M, m(2)), c(kK, 0)});
        for (uint32_t k = 0; k < layers; ++k)
            if (height[k]) e(kMax, {Dst(kTemp, M, m(2)), t(M, 2), c(kParamRow + k, 2)});
        const uint32_t alphas = (1u << (layers - 1)) - 1; // x, xy or xyz
        e(kCmp, {Dst(kTemp, A, alphas), t(M, 2, true), Src(kTemp, A), Src(kTemp, X)});

        // The program: version, our declarations, its own up to the alpha maps' read, the blend, the rest.
        const Ins* alphaIns = nullptr;
        for (const Ins& in : ins)
            if (in.at == alphaAt) alphaIns = &in;
        const size_t after = alphaAt + 1 + alphaIns->len;
        out.clear();
        out.reserve(w.size() + head.words.size() + e.words.size());
        out.push_back(w[0]);
        out.insert(out.end(), head.words.begin(), head.words.end());
        out.insert(out.end(), w.begin() + 1, w.begin() + static_cast<std::ptrdiff_t>(after));
        out.insert(out.end(), e.words.begin(), e.words.end());
        out.insert(out.end(), w.begin() + static_cast<std::ptrdiff_t>(after), w.end());
        return true;
    }
}
