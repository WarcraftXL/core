// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The renderer's own shaders, written as SPIR-V by spirv::Module when a pipeline needs them.
//
//   PresentVertex    one triangle that covers a rectangle given in push constants (vec4 x0, y0,
//                    x1, y1 in clip space, 16 bytes, vertex stage), with the texture coordinate 0..1
//                    across it: the whole target for the scene, the cursor's square for the cursor,
//                    the scissor keeping the triangle's excess out.
//   PresentFragment  the source (set 0, binding 0) sampled at that coordinate, its red, green and
//                    blue each looked up in the gamma ramp (set 0, binding 1: 256 rows of
//                    vec4(red, green, blue, 0), linear between rows), its alpha kept. An identity
//                    ramp leaves the colour as it was. The cursor's variant keeps only the pixels
//                    whose alpha is 224 or more, as the D3D9 device's alpha key draws the
//                    software cursor.

#pragma once

#include <cstdint>
#include <vector>

namespace wxl::vk::shaders
{
    std::vector<uint32_t> PresentVertex();
    std::vector<uint32_t> PresentFragment(bool alpha_key = false);

    /// The alpha below which the cursor's variant drops a pixel: the engine's alpha key, 224.
    constexpr float kAlphaKey = 224.0f / 255.0f;

    /// The gamma ramp's rows, as the fragment shader reads them (binding 1).
    constexpr uint32_t kGammaRows = 256;
}
