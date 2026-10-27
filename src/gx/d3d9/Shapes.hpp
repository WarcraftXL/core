// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw's shapes on a Direct3D 9 device: the game's own, on the stock client and on the
// recompiled one when it runs on Direct3D 9. The frame's vertices (gx/shapes/Geometry.hpp) drawn
// run by run with the runtime's programs (shaders/draw.hlsl: DrawPs for lines and plain insides,
// SurfacePs for surfaces, their texture on sampler 0) into the target the world is being drawn into,
// against its depth, in the viewport and depth range the world was drawn with; the states the pass
// sets are put back and the game told to send its own again. Nothing is made on the device until a
// shape is drawn; the programs and the declaration survive a reset, and are made again on a new
// device.

#pragma once

#include "gx/shapes/Geometry.hpp"

#include <cstdint>
#include <string>

namespace wxl::core::gx::d3d9
{
    class Device;
}

namespace wxl::core::gx::d3d9::shapes
{
    /// Draw through this device instead of the game's (the harness's recording one); null: the game's.
    void UseDevice(Device* device);

    /// Whether there is a Direct3D 9 device to draw with (the game's, or the harness's).
    bool HasDevice();

    /// Null when the device can draw shapes now; otherwise why not, in a sentence.
    const char* Unsupported();

    /**
     * @brief Draws @p tested + @p on_top vertices: the first @p tested against the depth, the
     *        others over it.
     * @param constants  wxl::gx::shapes::Constants of the world's view, c240 to c245
     * @param viewport   x, y, width, height in pixels, min and max depth: the world's
     * @return the vertices drawn; 0 with @p why when nothing could be
     */
    uint32_t Draw(const wxl::gx::shapes::Vertex* vertices, uint32_t tested, uint32_t on_top, const float constants[24],
                  const float viewport[6], std::string* why = nullptr);

    /**
     * @brief Draws @p runs of @p vertices in their order, each with its program, blend and depth
     *        test; a surface's run reads @p surfaces[run.surface] (c200 to c205), its texture an
     *        IDirect3DBaseTexture9* (null: untextured) on sampler 0.
     */
    uint32_t Draw(const wxl::gx::shapes::Vertex* vertices, const wxl::gx::shapes::Run* runs, uint32_t run_count,
                  const wxl::gx::shapes::Surface* surfaces, const float constants[24], const float viewport[6], std::string* why = nullptr);

    /// The client is shutting down: what was made on the device is released.
    void Shutdown();
}
