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
            kDcl = 0x1F, kDef = 0x51, kDefI = 0x52, kDefB = 0x53, kCmp = 0x58, kTexld = 0x42, kDp3 = 0x08, kLrp = 0x12,
        };
        enum Reg : uint32_t
        {
            kTemp = 0, kInput = 1, kConst = 2, kSampler = 10,
        };
        constexpr uint32_t kTex2d = 2;   ///< D3DSTT_2D in a sampler's dcl
        constexpr uint32_t kXyzw  = 0xE4; ///< the identity swizzle
        constexpr uint32_t kTemps = 32;  ///< ps_3_0's temporaries
        constexpr uint32_t kColorOut = 8; ///< oC#

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

    namespace
    {
        /// The four-layer programs: the chunk's layers past its four as well (see Ps3.hpp).
        bool EightLayers(const std::vector<uint32_t>& w, const std::vector<Ins>& ins, size_t alphaAt, uint32_t A, uint32_t ac,
                         const uint32_t* coord, const bool* height, uint32_t base, std::vector<uint32_t>& out, Found& found)
        {
            constexpr uint32_t K = kParamRow - 1, Pass = kPassRow, Coord = kCoordRow;
            const uint32_t W = base, W2 = base + 1, H = base + 2, H2 = base + 3, F = base + 4, F2 = base + 5, P = base + 6,
                           P2 = base + 7, M = base + 8, D = base + 9, X = base + 10, B = base + 11, O = base + 12, G = base + 13;
            const auto c = [](uint32_t row, uint32_t comp, bool neg = false) { return Src(kConst, row, Rep(comp), neg); };
            const auto t = [](uint32_t r, uint32_t comp, bool neg = false) { return Src(kTemp, r, Rep(comp), neg); };
            const auto m = [](uint32_t comp) { return 1u << comp; };
            // The alpha maps' coordinate, its first two components twice: .xyxy of what it reads.
            const uint32_t acSwz = (ac >> 16) & 0xFF, ac0 = acSwz & 3, ac1 = (acSwz >> 2) & 3;
            const uint32_t acXy  = (ac & ~0x00FF0000u) | ((ac0 | ac1 << 2 | ac0 << 4 | ac1 << 6) << 16);
            const bool     extraHeight[2] = {height[0], height[1]}; // read at layer 0 and 1's coordinates in the second draw

            Emit head;
            head(kDef, {Dst(kConst, K), FloatTok(0.0f), FloatTok(1.0f), FloatTok(1.0e-6f), FloatTok(0.0f)});
            head(kDcl, {0x80000000u | (kTex2d << 27), Dst(kSampler, kExtraAlphaSampler)});
            for (uint32_t k = 0; k < 2; ++k)
                if (extraHeight[k]) head(kDcl, {0x80000000u | (kTex2d << 27), Dst(kSampler, kExtraHeightSampler + k)});
            for (uint32_t k = 0; k < 4; ++k)
                if (height[k]) head(kDcl, {0x80000000u | (kTex2d << 27), Dst(kSampler, kHeightSampler + k)});

            Emit e;
            // Weights of the eight layers, each over the ones below (the client's lerps, carried on).
            e(kTexld, {Dst(kTemp, B), ac, Src(kSampler, kExtraAlphaSampler)});
            e(kMul, {Dst(kTemp, B), Src(kTemp, B), c(Pass, 1)});
            e(kMov, {Dst(kTemp, X, m(0)), c(K, 1)});
            for (uint32_t k = 4; k-- > 0;)
            {
                e(kMul, {Dst(kTemp, W2, m(k)), t(B, k), t(X, 0)});
                e(kMad, {Dst(kTemp, X, m(0)), t(B, k, true), t(X, 0), t(X, 0)});
            }
            for (uint32_t k = 3; k >= 1; --k)
            {
                e(kMul, {Dst(kTemp, W, m(k)), t(A, k - 1), t(X, 0)});
                e(kMad, {Dst(kTemp, X, m(0)), t(A, k - 1, true), t(X, 0), t(X, 0)});
            }
            e(kMov, {Dst(kTemp, W, m(0)), t(X, 0)});
            // Heights: each layer read at its own coordinates in its own draw, at the alpha maps'
            // carried to it (c41 + k) in the other.
            e(kMov, {Dst(kTemp, H), c(K, 0)});
            e(kMov, {Dst(kTemp, H2), c(K, 0)});
            const auto read = [&](uint32_t layer, uint32_t sampler, uint32_t own, bool ownInFirst, uint32_t into, uint32_t comp) {
                e(kMad, {Dst(kTemp, D, 0x3), acXy, Src(kConst, Coord + layer, 0x44 /* xyxy */), Src(kConst, Coord + layer, 0xEE /* zwzw */)});
                if (ownInFirst) e(kCmp, {Dst(kTemp, D, 0x3), c(Pass, 0, true), own, Src(kTemp, D)});
                else e(kCmp, {Dst(kTemp, D, 0x3), c(Pass, 0, true), Src(kTemp, D), own});
                e(kTexld, {Dst(kTemp, D), Src(kTemp, D), Src(kSampler, sampler)});
                e(kMov, {Dst(kTemp, into, m(comp)), t(D, 3)});
            };
            for (uint32_t k = 0; k < 4; ++k)
                if (height[k]) read(k, kHeightSampler + k, coord[k], true, H, k);
            for (uint32_t k = 0; k < 2; ++k)
                if (extraHeight[k]) read(4 + k, kExtraHeightSampler + k, coord[k], false, H2, k);
            // f = 1 + e (h * scale + offset - 1).
            e(kMov, {Dst(kTemp, F), c(K, 1)});
            e(kMov, {Dst(kTemp, F2), c(K, 1)});
            const auto factor = [&](uint32_t layer, uint32_t h, uint32_t into, uint32_t comp) {
                e(kMad, {Dst(kTemp, D, m(0)), t(h, comp), c(kParamRow + layer, 0), c(kParamRow + layer, 1)});
                e(kAdd, {Dst(kTemp, D, m(0)), t(D, 0), c(K, 1, true)});
                e(kMul, {Dst(kTemp, D, m(1)), t(D, 0), c(kParamRow + layer, 2)});
                e(kAdd, {Dst(kTemp, into, m(comp)), t(D, 1), c(K, 1)});
            };
            for (uint32_t k = 0; k < 4; ++k)
                if (height[k]) factor(k, H, F, k);
            for (uint32_t k = 0; k < 2; ++k)
                if (extraHeight[k]) factor(4 + k, H2, F2, k);
            e(kMul, {Dst(kTemp, P), Src(kTemp, W), Src(kTemp, F)});
            e(kMul, {Dst(kTemp, P2), Src(kTemp, W2), Src(kTemp, F2)});
            // The highest of the eight; whether any layer has its height (M.w).
            e(kMax, {Dst(kTemp, M, m(0)), t(P, 0), t(P, 1)});
            e(kMax, {Dst(kTemp, M, m(1)), t(P, 2), t(P, 3)});
            e(kMax, {Dst(kTemp, M, m(0)), t(M, 0), t(M, 1)});
            e(kMax, {Dst(kTemp, M, m(1)), t(P2, 0), t(P2, 1)});
            e(kMax, {Dst(kTemp, M, m(2)), t(P2, 2), t(P2, 3)});
            e(kMax, {Dst(kTemp, M, m(1)), t(M, 1), t(M, 2)});
            e(kMax, {Dst(kTemp, M, m(0)), t(M, 0), t(M, 1)});
            e(kMov, {Dst(kTemp, M, m(3)), c(K, 0)});
            for (uint32_t k = 0; k < 4; ++k)
                if (height[k]) e(kMax, {Dst(kTemp, M, m(3)), t(M, 3), c(kParamRow + k, 2)});
            for (uint32_t k = 0; k < 2; ++k)
                if (extraHeight[k]) e(kMax, {Dst(kTemp, M, m(3)), t(M, 3), c(kParamRow + 4 + k, 2)});
            // Cut below the highest, where some layer has its height; the plain lerps otherwise.
            for (uint32_t p : {P, P2})
            {
                e(kAdd, {Dst(kTemp, D, 0xF, true), t(M, 0), Src(kTemp, p, kXyzw, true)});
                e(kAdd, {Dst(kTemp, D), c(K, 1), Src(kTemp, D, kXyzw, true)});
                e(kAdd, {Dst(kTemp, D), Src(kTemp, D), c(K, 1, true)});
                e(kMad, {Dst(kTemp, D), Src(kTemp, D), t(M, 3), c(K, 1)});
                e(kMul, {Dst(kTemp, p), Src(kTemp, p), Src(kTemp, D)});
            }
            e(kDp4, {Dst(kTemp, M, m(1)), Src(kTemp, P), c(K, 1)});
            e(kDp4, {Dst(kTemp, M, m(2)), Src(kTemp, P2), c(K, 1)});
            // The second draw's share of the pixel: its alpha.
            e(kAdd, {Dst(kTemp, D, m(0)), t(M, 1), t(M, 2)});
            e(kMax, {Dst(kTemp, D, m(0)), t(D, 0), c(K, 2)});
            e(kRcp, {Dst(kTemp, D, m(0)), t(D, 0)});
            e(kMul, {Dst(kTemp, X, m(3)), t(M, 2), t(D, 0)});
            // This draw's four, shared out among themselves.
            e(kCmp, {Dst(kTemp, G), c(Pass, 0, true), Src(kTemp, P), Src(kTemp, P2)});
            e(kCmp, {Dst(kTemp, D, m(1)), c(Pass, 0, true), t(M, 1), t(M, 2)});
            e(kMax, {Dst(kTemp, D, m(1)), t(D, 1), c(K, 2)});
            e(kRcp, {Dst(kTemp, D, m(1)), t(D, 1)});
            e(kMul, {Dst(kTemp, G), Src(kTemp, G), t(D, 1)});
            // Back into the program's alphas: each layer over what is left under the ones above.
            e(kMov, {Dst(kTemp, D, m(2)), c(K, 1)});
            for (uint32_t k = 3; k >= 1; --k)
            {
                e(kMax, {Dst(kTemp, D, m(3)), t(D, 2), c(K, 2)});
                e(kRcp, {Dst(kTemp, D, m(3)), t(D, 3)});
                e(kMul, {Dst(kTemp, X, m(k - 1), true), t(G, k), t(D, 3)});
                e(kAdd, {Dst(kTemp, D, m(2)), t(D, 2), t(G, k, true)});
            }
            // A chunk with no height and no layer past its four: the client's own alphas.
            e(kMax, {Dst(kTemp, M, m(3)), t(M, 3), c(Pass, 1)});
            e(kCmp, {Dst(kTemp, A, 0x7), t(M, 3, true), Src(kTemp, A), Src(kTemp, X)});

            Emit tail; // the colour, its alpha the second draw's share in the second draw
            tail(kCmp, {Dst(kTemp, O, m(3)), c(Pass, 0, true), t(O, 3), t(X, 3)});
            tail(kMov, {Dst(kColorOut, 0), Src(kTemp, O)});

            const Ins* alphaIns = nullptr;
            for (const Ins& in : ins)
                if (in.at == alphaAt) alphaIns = &in;
            const size_t          after = alphaAt + 1 + alphaIns->len;
            std::vector<uint32_t> body(w);
            // Every write of the colour goes to O, written out once at the end.
            for (const Ins& in : ins)
                if (in.op != kComment && in.op != kDcl && in.op != kDef && in.op != 0x52 && in.op != 0x53 && in.len >= 1 &&
                    TypeOf(body[in.at + 1]) == kColorOut && NumOf(body[in.at + 1]) == 0)
                    body[in.at + 1] = (body[in.at + 1] & 0x00FF0000u) | (body[in.at + 1] & 0x0F000000u) | RegTok(kTemp, O);
            size_t end = body.size() - 1;
            while (end > 0 && body[end] != kEnd) --end;
            out.clear();
            out.push_back(body[0]);
            out.insert(out.end(), head.words.begin(), head.words.end());
            out.insert(out.end(), body.begin() + 1, body.begin() + static_cast<std::ptrdiff_t>(after));
            out.insert(out.end(), e.words.begin(), e.words.end());
            out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(after), body.begin() + static_cast<std::ptrdiff_t>(end));
            out.insert(out.end(), tail.words.begin(), tail.words.end());
            out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(end), body.end());
            found.heights += (extraHeight[0] ? 1 : 0) + (extraHeight[1] ? 1 : 0);
            return true;
        }
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
        for (uint32_t k = kExtraAlphaSampler; k < kHeightSampler + 4; ++k)
            if (samplers.count(k))
            {
                why = "a height sampler is in use";
                return false;
            }
        constexpr uint32_t kK = kParamRow - 1; // c31: 0, 1, a small number
        for (uint32_t r = kK; r < kCoordRow + 8; ++r)
            if (consts.count(r | (kConst << 16)))
            {
                why = "a constant the blend needs is in use";
                return false;
            }
        const uint32_t base = temps;
        if (base + 14 > kTemps)
        {
            why = "too few temporaries left";
            return false;
        }

        // Each layer's coordinates (its texture's read), and the read of the alpha maps.
        uint32_t coord[4] = {};
        bool     height[4] = {};
        size_t   alphaAt = 0, alphaReads = 0;
        uint32_t alpha = 0, alphaCoord = 0;
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
                alphaAt    = in.at;
                alpha      = w[in.at + 1];
                alphaCoord = w[in.at + 2];
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
        // How the program mixes its layers from the alpha maps: each over the ones below (lrp, the
        // client's programs for maps without big alpha), or summed with layer 0 the rest (dp3_sat
        // of the alphas, then 1 minus it: its programs for maps with big alpha, MPHD 0x4).
        bool lerps = false, sums = false;
        for (const Ins& in : ins)
        {
            if (in.at <= alphaAt || in.len < 3) continue;
            const auto reads = [&](uint32_t k) { return TypeOf(w[in.at + k]) == kTemp && NumOf(w[in.at + k]) == A; };
            if (in.op == kLrp && in.len == 4 && reads(2)) lerps = true;
            if (in.op == kDp3 && (reads(2) || reads(3))) sums = true;
        }
        if (lerps == sums)
        {
            why = "the layers' mix is neither the lerps nor the sum of the client's programs";
            return false;
        }
        found.sums = sums;
        const uint32_t W = base, H = base + 1, F = base + 2, P = base + 3, M = base + 4, D = base + 5, X = base + 6;
        const auto     c = [](uint32_t row, uint32_t comp, bool neg = false) { return Src(kConst, row, Rep(comp), neg); };
        const auto     t = [](uint32_t r, uint32_t comp, bool neg = false) { return Src(kTemp, r, Rep(comp), neg); };
        const auto     m = [](uint32_t comp) { return 1u << comp; };

        if (layers == 4 && TypeOf(alphaCoord) == kInput)
        {
            found.extras = true;
            return EightLayers(w, ins, alphaAt, A, alphaCoord, coord, height, base, out, found);
        }

        Emit head; // declarations, ahead of the program's own
        head(kDef, {Dst(kConst, kK), FloatTok(0.0f), FloatTok(1.0f), FloatTok(1.0e-6f), FloatTok(0.0f)});
        for (uint32_t k = 0; k < layers; ++k)
            if (height[k]) head(kDcl, {0x80000000u | (kTex2d << 27), Dst(kSampler, kHeightSampler + k)});

        Emit e; // after the alpha maps' read
        // w: each layer's share of the client's mix.
        e(kMov, {Dst(kTemp, W), c(kK, 0)});
        if (sums)
        {
            // Layer k's alpha is its share; layer 0 has what the others leave, as the program has it.
            e(kDp3, {Dst(kTemp, D, m(0), true), Src(kTemp, A), c(kK, 1)});
            e(kAdd, {Dst(kTemp, W, m(0)), c(kK, 1), t(D, 0, true)});
            for (uint32_t k = 1; k < layers; ++k) e(kMov, {Dst(kTemp, W, m(k)), t(A, k - 1)});
        }
        else
        {
            // Each layer over the ones below; X.x the part left under the layers above.
            e(kMov, {Dst(kTemp, X, m(0)), c(kK, 1)});
            for (uint32_t k = layers - 1; k >= 1; --k)
            {
                e(kMul, {Dst(kTemp, W, m(k)), t(A, k - 1), t(X, 0)});
                e(kMad, {Dst(kTemp, X, m(0)), t(A, k - 1, true), t(X, 0), t(X, 0)});
            }
            e(kMov, {Dst(kTemp, W, m(0)), t(X, 0)});
        }
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
        // The shares back into the program's alphas: summed, layer k's share itself (the program
        // gives layer 0 the rest); otherwise layer k's over what is left under the ones above.
        if (sums)
        {
            // Alphas of layers the chunk lacks are zero, so that the program's sum is the shares'.
            e(kMov, {Dst(kTemp, X, 0x7), c(kK, 0)});
            for (uint32_t k = 1; k < layers; ++k) e(kMov, {Dst(kTemp, X, m(k - 1)), t(P, k)});
        }
        else
        {
            e(kMov, {Dst(kTemp, M, m(3)), c(kK, 1)});
            for (uint32_t k = layers - 1; k >= 1; --k)
            {
                e(kMax, {Dst(kTemp, M, m(2)), t(M, 3), c(kK, 2)});
                e(kRcp, {Dst(kTemp, M, m(2)), t(M, 2)});
                e(kMul, {Dst(kTemp, X, m(k - 1), true), t(P, k), t(M, 2)});
                e(kAdd, {Dst(kTemp, M, m(3)), t(M, 3), t(P, k, true)});
            }
        }
        // No layer with its height in the chunk: the client's own alphas.
        e(kMov, {Dst(kTemp, M, m(2)), c(kK, 0)});
        for (uint32_t k = 0; k < layers; ++k)
            if (height[k]) e(kMax, {Dst(kTemp, M, m(2)), t(M, 2), c(kParamRow + k, 2)});
        const uint32_t alphas = sums ? 0x7u : (1u << (layers - 1)) - 1; // summed: all three; else x, xy or xyz
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
