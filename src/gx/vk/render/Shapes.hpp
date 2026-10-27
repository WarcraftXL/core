// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw's shapes on the Vulkan renderer, made of wxl.render's pieces: two GRAPHICS pipelines of
// the runtime's own (glsl/draw.vert and draw.frag; the one tested against the scene's depth, the
// other on top), the frame's vertices written into the frame's memory and read by address, one
// pass onto the scene's colour against its depth, with the viewport and the depth range the world
// was drawn with. The same vertices and the same arithmetic as the Direct3D 9 passes
// (gx/d3d9/Shapes.cpp). Portable: no runtime, no Windows; the native tests draw it on lavapipe.

#pragma once

#include "gx/shapes/Geometry.hpp"
#include "gx/vk/render/Render.hpp"

#include <cstdint>
#include <string>

namespace wxl::vk::render
{
    /// wxl.draw's programs, for the tests' validation.
    std::vector<OwnProgram> ShapePrograms();

    class Shapes
    {
    public:
        /// Vertices of one draw: what the frame's memory takes in one piece (1 MiB).
        static constexpr uint32_t kVerticesPerDraw = 32766;

        Shapes() = default;
        Shapes(const Shapes&)            = delete;
        Shapes& operator=(const Shapes&) = delete;
        ~Shapes() { Detach(); }

        /// Draws with @p render (its device made).
        void Attach(Render& render) { render_ = &render; }
        /// Its pipelines given back to the render it drew with.
        void Detach();
        /// Its pipelines forgotten: the render they were made on is gone with them.
        void Forget();
        Render* Attached() const { return render_; }

        /**
         * @brief Draws @p tested + @p on_top vertices of @p vertices into the open frame's scene:
         *        the first @p tested hidden behind what the scene's depth holds, the others over it.
         * @param constants  gx::shapes::Constants of the world's view
         * @param viewport   D3D9's: x, y, width, height in pixels, min and max depth
         * @return the vertices drawn; 0 with @p why when nothing could be
         */
        uint32_t Draw(const gx::shapes::Vertex* vertices, uint32_t tested, uint32_t on_top, const float constants[24],
                      const float viewport[6], std::string* why = nullptr);

    private:
        bool Make(std::string* why);

        Render*   render_ = nullptr;
        Pipeline* tested_ = nullptr;
        Pipeline* top_ = nullptr;
    };
}
