// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.highlight on the Vulkan renderer, made of wxl.render's own pieces: the passes of the
// Direct3D 9 renderer (gx/d3d9/Highlight.hpp), one for one.
//
// The batches of a highlighted model the engine draws in the world pass are kept (draw::Batch, as
// wxl.render's keep_batch keeps them). After the world, they are drawn again with their own vertex
// program and a BATCH program of wxl.render into two masks that test the scene's depth without
// writing it, so that only what is seen of them is marked: the colour and coverage in one, the glow
// and the outline width in the other. Both have the scene's samples and are resolved when the pass
// ends. The second is blurred at half the size, across then down; a FULLSCREEN pass finds the
// outline around the mask and adds the glow onto the scene, blended ONE, ONE_MINUS_SRC_ALPHA.
// Widths are given for 1080 lines and drawn at the scene's height.
//
// Nothing is kept, drawn or made while nothing is highlighted; the images and the pipelines are
// made at the first highlight and kept (made again with the device after a loss, as every
// wxl.render resource).

#pragma once

#include "gx/vk/render/Render.hpp"

#include <cstdint>
#include <vector>

namespace wxl::vk::render
{
    class Highlight
    {
    public:
        /// One highlight's look, as wxl.highlight was given it.
        struct Style
        {
            uint32_t color;    ///< 0xRRGGBB
            float    outline;  ///< pixels at 1080 lines
            float    glow;
            float    strength; ///< 0 to 1
        };

        static constexpr uint32_t kMaxBatches = 512; ///< kept per frame, as on Direct3D 9

        Highlight() = default;
        Highlight(const Highlight&)            = delete;
        Highlight& operator=(const Highlight&) = delete;
        ~Highlight() { Detach(); }

        /// Draws with @p render (its device made).
        void Attach(Render& render) { render_ = &render; }
        /// Its images and pipelines given back to the render it drew with.
        void Detach();
        /// Its images and pipelines forgotten: the render they were made on is gone with them.
        void Forget();

        /// A world pass starts: nothing kept yet.
        void Begin() { kept_.clear(); }
        /// A batch of a highlighted model, its style's index: kept when it is the world's, drawn into
        /// the scene (not into a texture), with a vertex program of the client's.
        bool Keep(const draw::Batch& batch, uint32_t style);
        size_t Kept() const { return kept_.size(); }

        /// After the world: the kept batches drawn again and the highlights onto the scene. Answers
        /// how many batches were drawn again; @p why says what failed when nothing could be.
        uint32_t Draw(const Style* styles, uint32_t count, const char** why = nullptr);

    private:
        bool Make(const char** why);

        struct KeptBatch
        {
            draw::Batch batch;
            uint32_t    style;
        };

        Render*                render_ = nullptr;
        std::vector<KeptBatch> kept_;
        Image*                 color_ = nullptr; ///< masks, the scene's samples, resolved
        Image*                 extra_ = nullptr;
        Image*                 glow_a_ = nullptr; ///< half the size
        Image*                 glow_b_ = nullptr;
        Pipeline*              mask_ = nullptr;
        Pipeline*              blur_ = nullptr;
        Pipeline*              composite_ = nullptr;
    };
}
