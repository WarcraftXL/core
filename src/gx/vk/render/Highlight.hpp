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
//
// A world pass, as the service drives it: World (the highlighted top models and their styles),
// then Batch for each model batch the engine draws (with the renderer's record of that draw), then
// Draw after the world. Pass() counts what each step took and keeps the first reason something
// was left out, for the runtime's log.

#pragma once

#include "gx/vk/render/Render.hpp"

#include <cstdint>
#include <string>
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

        /// A highlighted top model of this world pass and the index of its style.
        struct Root
        {
            const void* model;
            uint32_t    style;
        };

        /// What one world pass did.
        struct Counts
        {
            uint32_t    roots = 0;     ///< highlighted top models
            uint32_t    models = 0;    ///< model batches the engine drew
            uint32_t    seen = 0;      ///< of those, the highlighted models'
            uint32_t    kept = 0;      ///< of those, kept to be drawn again
            uint32_t    drawn = 0;     ///< drawn again into the masks
            bool        composited = false; ///< the outline and the glow went onto the scene
            std::string refusal;       ///< the first reason something was left out; empty when nothing was
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

        /// Nothing kept, nothing counted: no world pass is open.
        void Begin();
        /// A world pass starts with @p count highlighted top models (none: nothing is kept in it).
        void World(const Root* roots, uint32_t count);
        /// Whether the open world pass has highlighted models: the renderer keeps each draw.
        bool Capturing() const { return !roots_.empty(); }
        /// The engine drew a model batch of top model @p root in blend mode @p blend; @p last is the
        /// renderer's record of that draw (null when it kept none). Kept when the model is
        /// highlighted, the batch opaque (blend 0 or 1) and drawn with a vertex program of the
        /// client's into the scene.
        bool Batch(const void* root, uint32_t blend, const draw::Batch* last);
        /// A batch of a highlighted model, its style's index: kept when it is drawn into the scene
        /// (not into a texture) and fewer than kMaxBatches are.
        bool Keep(const draw::Batch& batch, uint32_t style);
        size_t Kept() const { return kept_.size(); }

        /// After the world: the kept batches drawn again and the highlights onto the scene. Answers
        /// how many batches were drawn again into the masks, 0 when the highlights did not reach the
        /// scene; @p why says what failed first (valid until the next pass).
        uint32_t Draw(const Style* styles, uint32_t count, const char** why = nullptr);

        /// The open or the last world pass's counts.
        const Counts& Pass() const { return counts_; }

    private:
        bool     Make();
        uint32_t DrawPasses(const Style* styles, uint32_t count);
        /// Notes @p reason as the pass's first when it has none; answers false, for a refusal's return.
        bool Refuse(const char* reason);

        struct KeptBatch
        {
            draw::Batch batch;
            uint32_t    style;
        };

        Render*                render_ = nullptr;
        std::vector<KeptBatch> kept_;
        std::vector<Root>      roots_;
        Counts                 counts_;
        Image*                 color_ = nullptr; ///< masks, the scene's samples, resolved
        Image*                 extra_ = nullptr;
        Image*                 glow_a_ = nullptr; ///< half the size
        Image*                 glow_b_ = nullptr;
        Pipeline*              mask_ = nullptr;
        Pipeline*              blur_ = nullptr;
        Pipeline*              composite_ = nullptr;
    };
}
