// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.gx's shader providers: each DX9 program a device makes is shown to the mods' providers
// (set_shader_provider), and made from the bytecode they answer.
//
//   BeforeCreate  the Direct3D 9 device is about to make a program from its record (seams
//   AfterCreate   Gx.D3dIShaderCreatePixel, Gx.D3dIShaderCreateVertex): the answer is put in the
//                 record's code for the call, and the client's own put back after it
//   Answer        the Vulkan renderer is about to translate a program (its backend's
//                 shader_create): the answer, translated instead of the client's code
//
// Main thread only, as the client makes its programs.

#pragma once

#include "wxl/services/gx.h"

#include <cstdint>
#include <vector>

namespace wxl::core::services::shader_code
{
    /// The calling mod's provider; null takes it off.
    wxl_result SetProvider(uint32_t mod, const wxl_gx_shader_provider* provider);
    void       ForgetMod(uint32_t mod);
    /// The client shuts down.
    void Clear();

    /// Asks the providers for program @p name (stage 0 vertex, 1 pixel) made from @p code; true and
    /// @p out filled when one answered.
    bool Answer(const char* name, uint32_t stage, const void* code, uint32_t size, std::vector<uint8_t>& out);

    /// The D3D9 device makes @p shader (CGxShader*) from its record.
    void BeforeCreate(void* shader);
    void AfterCreate(void* shader);

    /// What it did, for the tests and the session's lines.
    struct Counters
    {
        uint64_t asked = 0;    ///< programs shown to providers
        uint64_t answered = 0; ///< of those, made from an answer
    };
    Counters Count();
}
