// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.highlight service: world objects outlined and lit by a glow, drawn after the world.

#pragma once

#include <cstdint>
#include <string>

#include "wxl/events.h"
#include "wxl/services/highlight.h"

#if !defined(_M_IX86)
namespace wxl::vk
{
    namespace render
    {
        class Render;
    }
    namespace draw
    {
        class Renderer;
    }
}
#endif

namespace wxl::core::services
{
    /// The table, WXL_HIGHLIGHT_SERVICE version WXL_HIGHLIGHT_SERVICE_VERSION.
    const wxl_highlight_api& HighlightApi();

    namespace highlight
    {
        /// The world pass starts (WorldFrame.RenderWorld Before): at its first batch the
        /// highlighted objects are found, those gone dropped, and their models' batches kept.
        void WorldBegin();

        /// Whether a model batch drawn now is wanted (Batch is to be called with its numbers).
        bool WantsBatch();

        /// Whether the renderer is to keep each draw (the Vulkan renderer's batches).
        bool Capturing();

        /// The game drew a model batch (M2.DrawBatch After, a CM2SceneRender*), its numbers @p args
        /// read already.
        void Batch(const void* render, const wxl_model_batch_args& args);

        /// The world is drawn (WorldFrame.RenderWorld After): the highlights go onto it.
        void WorldEnd();

        /// A frame is presented (Gx.ScenePresent Before): while highlights are set, one line at
        /// debug level about once a second of what the last world pass did.
        void Present();

        /// What a world pass with highlights did.
        struct PassCounts
        {
            bool        open = false;     ///< the world pass is being drawn
            bool        resolved = false; ///< a model batch reached the service: the objects were looked up
            uint32_t    entries = 0;      ///< highlights set
            uint32_t    roots = 0;        ///< top models found for them
            uint32_t    models = 0;       ///< model batches drawn once they were found
            uint32_t    seen = 0;         ///< of those, the highlighted models'
            uint32_t    kept = 0;         ///< kept to be drawn again
            uint32_t    drawn = 0;        ///< drawn again into the masks
            bool        composited = false; ///< the outline and the glow went onto the scene
            std::string refusal;          ///< the first reason something was left out
        };

        /// The open or the last world pass with highlights.
        const PassCounts& LastPass();

        /// The device's resources are about to go.
        void DeviceLost();

        /// The Vulkan device's renderer goes with everything made on it (its wxl.render records).
        void DeviceGone();

        /// A mod's highlights are taken off: disabled or unloaded.
        void ForgetMod(uint32_t mod);

        /// Every highlight is forgotten and what was made on the device released: the client is
        /// shutting down.
        void Clear();

#if !defined(_M_IX86)
        /// What the Vulkan passes draw with, and the renderer whose draws they keep.
        struct VulkanTarget
        {
            wxl::vk::render::Render*  render = nullptr;
            wxl::vk::draw::Renderer*  engine = nullptr; ///< null: no draw is kept
        };

        /// Tests: @p target in place of the Vulkan device's (null: the device's again).
        void UseVulkanTarget(const VulkanTarget* target);
#endif
    }
}
