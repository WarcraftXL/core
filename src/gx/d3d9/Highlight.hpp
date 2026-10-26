// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.highlight drawn on the stock client's Direct3D 9 device (x86).
//
// During the world pass, each batch of a highlighted model the game draws (CM2SceneRender::
// DrawBatch) is kept: the numbers of its draw, what the device had bound and the engine's copy of
// the vertex program's constants (its bones, its matrices). After the world (WorldFrame.
// RenderWorld After: the world and its glow drawn, the interface not), the kept batches are drawn
// again with the game's own vertex program into two mask targets, depth-tested against the
// scene's depth without writing it, so that only what is seen of them is marked: the colour and
// coverage in one, the glow and the outline width in the other. The second is blurred at half the
// size, and a full-screen pass finds the outline around the mask and adds the glow, onto the
// scene. Everything the passes set is put back, and the game told to send its own state again.
//
// The targets follow the scene's size and its depth buffer's multisampling (a multisampled mask
// is resolved into the textures the passes read), and go with the device's resources at a reset.
// Nothing is kept, drawn or allocated while nothing is highlighted.

#pragma once

#include <cstdint>

namespace wxl::core::gx::d3d9
{
    class Device;
}

namespace wxl::core::gx::d3d9::highlight
{
    /// One highlight's look, as wxl.highlight was given it.
    struct Style
    {
        uint32_t color;    ///< 0xRRGGBB
        float    outline;  ///< pixels, 0 to WXL_HIGHLIGHT_MAX_OUTLINE
        float    glow;     ///< pixels, 0 to WXL_HIGHLIGHT_MAX_GLOW
        float    strength; ///< 0 to 1
    };

    /// Where the stock client keeps what the passes read; the x86 attachment sets it.
    struct Source
    {
        const void* (*device)() = nullptr; ///< the engine's device (g_theGxDevicePtr), a CGxDevice*
        uint8_t*       vertexShadow  = nullptr; ///< CGxDevice::s_shadowConstants[1]: the vertex program's constants
        const int32_t* enableShaders = nullptr; ///< CShaderEffect::s_enableShaders
    };
    void SetSource(const Source& source);

    /// Draw through this device instead of the game's (the harness's recording one); null: the game's.
    void UseDevice(Device* device);

    /// Null when the device can draw highlights now; otherwise why not, in a sentence.
    const char* Unsupported();

    /// A world pass starts: batches of these top models are kept, each with its style's index.
    /// Nothing is kept for an empty list.
    void Begin(const void* const* roots, const uint32_t* styles, uint32_t count);

    /// The game drew a batch (CM2SceneRender*): kept when its model hangs on a highlighted top.
    void Batch(const void* render);

    /// After the world: the kept batches drawn again and the highlights onto the scene. Answers
    /// how many batches were drawn again.
    uint32_t Draw(const Style* styles, uint32_t count);

    /// The device's resources are about to go: the targets go too, made again at the next draw.
    void DeviceLost();

    /// The client is shutting down: everything made on the device is released.
    void Shutdown();
}
