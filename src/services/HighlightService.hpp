// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.highlight service: world objects outlined and lit by a glow, drawn after the world.

#pragma once

#include <cstdint>

#include "wxl/events.h"
#include "wxl/services/highlight.h"

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

        /// The device's resources are about to go.
        void DeviceLost();

        /// The Vulkan device's renderer goes with everything made on it (its wxl.render records).
        void DeviceGone();

        /// A mod's highlights are taken off: disabled or unloaded.
        void ForgetMod(uint32_t mod);

        /// Every highlight is forgotten and what was made on the device released: the client is
        /// shutting down.
        void Clear();
    }
}
