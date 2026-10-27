// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The client's DX9 programs as SPIR-V for Vulkan 1.3, translated on the user's machine from the
// user's own records (nothing Blizzard-derived ships). A record is decoded (Dx9.hpp), then written
// as one SPIR-V module against the renderer's interface (Interface.hpp):
//
//   registers   the engine's c0..c255 are read from the frame's copy of its register file, through
//               the buffer address the draw pushes; a register the program sets itself (def, defi,
//               defb) is a constant of the module, also where a0 or aL index it
//   inputs      a vertex program's by the meaning of its dcl, at the engine's attribute; a pixel
//               program's at the varying of that meaning (ps_3_0), or by register below it (v0, v1
//               the colours, t0..t7 the texture coordinates)
//   outputs     a vertex program writes every varying, its position into gl_Position with y turned
//               to Vulkan's (D3D9's z range is Vulkan's), its point size, and the user clip plane's
//               distance; a pixel program writes location 0, after D3D9's fog (below ps_3_0, when
//               the draw asks) and alpha test
//   textures    stage s of a draw is pushed as a bindless slot and a sampler; 2D and cube views
//
// D3D9 rules the module keeps: rcp, rsq and log of 0 give the largest float rather than an infinity
// (what keeps 0 * rcp(0) at 0, as the programs expect); rsq and log read the absolute value, pow
// its base's; ps 1.x constants are clamped to [-1, 1]; vs 1.x/2.x colour outputs are saturated;
// mova rounds (vs 2.0 and up), a mov to a0 floors (vs 1.1); texkill tests x, y, z below ps 2.0 and
// the masked components from it.
//
// A record this translator cannot write (an opcode, a register or a construct it does not handle)
// is refused with the reason, and the client draws without it, as when D3D9 refuses a shader.
// Portable, no device; thread-safe (it holds no state).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wxl::vk::shaders
{
    /// Bumped whenever the translator's output changes: a cache keyed with it drops older modules.
    constexpr uint32_t kTranslatorVersion = 3;

    struct Options
    {
        bool clip_distance = true; ///< the device has shaderClipDistance: the user clip plane is written
    };

    /// What the renderer needs to know of a translated program.
    struct ProgramInfo
    {
        bool     pixel = false;
        uint8_t  major = 0;
        uint8_t  minor = 0;
        uint32_t inputs = 0;      ///< a vertex program: the attribute locations it reads (a bit each)
        uint32_t const_rows = 0;  ///< the register rows it reads from the engine's file (256 with a0/aL indexing, bones aside)
        /// A vertex program reading bones (c31..c33 through a0): from the draw's palette when it has
        /// one, else from the register file, all 256 rows of it.
        bool     palette = false;
        uint32_t samplers = 0;    ///< the stages it samples (a bit each)
        uint32_t cube = 0;        ///< of those, the ones sampled as cube maps
        bool     fixed_fog = false; ///< a pixel program below ps_3_0: D3D9's fog applies after it
    };

    struct Translation
    {
        std::vector<uint32_t> words;
        ProgramInfo           info;
    };

    /**
     * @brief Translates the DX9 record @p code (@p size bytes, a whole number of tokens).
     * @return false with @p why when it cannot be: the client draws without it.
     */
    bool Translate(const void* code, size_t size, const Options& options, Translation& out, std::string& why);
}
