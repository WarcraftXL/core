// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// D3D9 shader tokens assembled by hand, for the tests: a program's version, its instructions with
// their destination and source tokens (and relative address tokens from SM2), its dcl and def.
// Below SM2 the instruction tokens carry no length, as the D3D9 assembler writes them.

#pragma once

#include "gx/vk/shaders/Dx9.hpp"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace wxl::vk::test::dx9asm
{
    using wxl::vk::shaders::dx9::Reg;
    using wxl::vk::shaders::dx9::SrcMod;

    inline uint32_t F2U(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }



    inline uint32_t TypeBits(Reg type)
    {
        const uint32_t t = static_cast<uint32_t>(type);
        return ((t & 7) << 28) | (((t >> 3) & 3) << 11);
    }

    inline uint32_t Dst(Reg type, uint32_t index, uint32_t mask = 0xF, bool sat = false, int shift = 0)
    {
        return 0x80000000u | TypeBits(type) | index | (mask << 16) | (sat ? 1u << 20 : 0) | ((static_cast<uint32_t>(shift) & 0xF) << 24);
    }

    constexpr uint32_t Swz(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return x | y << 2 | z << 4 | w << 6; }
    constexpr uint32_t kXYZW = 0xE4;

    inline uint32_t Src(Reg type, uint32_t index, uint32_t swizzle = kXYZW, SrcMod mod = SrcMod::None, bool relative = false)
    {
        return 0x80000000u | TypeBits(type) | index | (swizzle << 16) | (static_cast<uint32_t>(mod) << 24) | (relative ? 1u << 13 : 0);
    }

    /// The address token of a relative operand (SM2 and up): a0.<component> or aL.
    inline uint32_t Rel(Reg type, uint32_t component) { return 0x80000000u | TypeBits(type) | (component * 0x55u) << 16; }

    struct Asm
    {
        std::vector<uint32_t> t;
        bool                  sm1;

        Asm(bool pixel, int major, int minor) : sm1(major < 2)
        {
            t.push_back((pixel ? 0xFFFF0000u : 0xFFFE0000u) | static_cast<uint32_t>(major << 8) | static_cast<uint32_t>(minor));
        }
        void Op(uint16_t op, std::initializer_list<uint32_t> operands, uint32_t control = 0)
        {
            t.push_back(op | (sm1 ? 0u : static_cast<uint32_t>(operands.size()) << 24) | control << 16);
            t.insert(t.end(), operands);
        }
        void Dcl(uint32_t usage, uint32_t index, uint32_t dst) { Op(wxl::vk::shaders::dx9::kDcl, {0x80000000u | usage | index << 16, dst}); }
        void DclSampler(uint32_t texture, uint32_t s) { Op(wxl::vk::shaders::dx9::kDcl, {0x80000000u | texture << 27, Dst(Reg::Sampler, s)}); }
        void Def(uint32_t c, float x, float y, float z, float w) { Op(wxl::vk::shaders::dx9::kDef, {Dst(Reg::Const, c), F2U(x), F2U(y), F2U(z), F2U(w)}); }
        void DefI(uint32_t i, int32_t x, int32_t y, int32_t z, int32_t w)
        {
            Op(wxl::vk::shaders::dx9::kDefI, {Dst(Reg::ConstInt, i), static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z),
                            static_cast<uint32_t>(w)});
        }
        void DefB(uint32_t b, bool v) { Op(wxl::vk::shaders::dx9::kDefB, {Dst(Reg::ConstBool, b), v ? 1u : 0u}); }
        std::vector<uint8_t> Bytes() const
        {
            std::vector<uint32_t> all = t;
            all.push_back(0x0000FFFFu);
            std::vector<uint8_t> bytes(all.size() * 4);
            std::memcpy(bytes.data(), all.data(), bytes.size());
            return bytes;
        }
    };
}
