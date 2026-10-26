// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// One draw of the engine read out of its device: CGxDevice through the SDK's engine layouts
// (wxl/engine/gx.hpp), its render states, its vertex constants, turned into a draw::Packet as the
// D3D9 device's IStateSync turns the same state into D3D9 calls (its viewport, scissor, vertex
// declaration, blend, depth, cull, texture stages, transforms and fog). The same reading serves
// the live client (Front) and a frame capture replayed (src/gx/capture/): what a pointer of the
// engine names is asked of a Resolver, which follows it on the live client and looks it up in
// the capture otherwise. Portable: no Windows, no runtime.
//
// What V2 draws: the engine's 2D work, the interface. A draw is left out, with the reason, when
//   - it goes into a texture target (V3 draws into targets);
//   - it is inside the world's pass (CGWorldFrame::RenderWorld: terrain, models, particles, V3/V4);
//   - it projects in perspective with fixed function outside the world's pass (a model's
//     particles in a frame, the login screen's scene): the interface's projections are
//     orthographic;
//   - it binds a program of the client the renderer has none of (translated from V3): the models
//     of the frames, whose effects bind theirs, among them.

#pragma once

#include "gx/vk/draw/Draw.hpp"

#include "wxl/core/gx_state.h"
#include "wxl/engine/gx.hpp"

#include <cstdint>

namespace wxl::vk::gxfront
{
    namespace engine = wxl::engine;

    /// What the engine's pointers name.
    class Resolver
    {
    public:
        /// The CGxBuf @p buf points at; null when unknown.
        virtual const engine::CGxBuf* Buffer(const void* buf) = 0;
        /// The renderer's pool for the CGxPool @p pool; null when it has none.
        virtual draw::Pool* Pool(const void* pool) = 0;
        /// The renderer's texture for the CGxTex @p texture, made and up to date, and the engine's
        /// object (for its flags) in @p fields; null when it has none.
        virtual draw::Texture* Texture(const void* texture, const engine::CGxTex*& fields) = 0;
        /// The renderer's program for the CGxShader @p shader, a VertexProgram or PixelProgram; -1
        /// for a program of the client it has none of.
        virtual int Program(const void* shader, bool vertex) = 0;

    protected:
        ~Resolver() = default;
    };

    struct EngineView
    {
        const engine::CGxDevice* device = nullptr;
        const wxl_gx_app_state*  states = nullptr;           ///< m_appRenderStates' elements (GxRenderStates_Last)
        const float*             vertex_constants = nullptr; ///< 256 rows: the vertex program's registers
        bool                     world = false;              ///< inside CGWorldFrame::RenderWorld
    };

    enum class Verdict
    {
        Draw,    ///< the packet is filled
        Target,  ///< into a texture target
        World,   ///< inside the world's pass
        Perspective, ///< fixed function in perspective outside the world's pass: 3D
        Program, ///< a program of the client the renderer has none of
        Unready, ///< a buffer or a pool it reads is unknown
    };

    const char* VerdictName(Verdict verdict);

    /// The engine's render states this reading uses (EGxRenderState).
    enum State : uint32_t
    {
        kPolygonOffset = 0,
        kMatDiffuse    = 1,
        kMatEmissive   = 2,
        kBlendingMode  = 6,
        kAlphaRef      = 7,
        kFogStart      = 8,
        kFogEnd        = 9,
        kFogColor      = 10,
        kLighting      = 11,
        kFog           = 12,
        kDepthTest     = 13,
        kDepthFunc     = 14,
        kDepthWrite    = 15,
        kColorWrite    = 16,
        kCulling       = 17,
        kScissorTest   = 20,
        kTexture0      = 21,
        kColorOp0      = 37,
        kAlphaOp0      = 45,
        kTexGen0       = 53,
        kTextureShader0 = 61,
        kTextureCoord0 = 69,
        kVertexShader  = 77,
        kPixelShader   = 78,
        kBlendFactor   = 84,
        kRenderStates  = 86,
    };

    /// The rectangle a clear covers, in the window's pixels (x, y, width, height): D3D9 clears the
    /// viewport, the one IXformSetViewport makes.
    void ClearRect(const engine::CGxDevice& device, int32_t rect[4]);

    /**
     * @brief Reads the draw of @p batch (indexed when @p indexed) out of @p view into @p out.
     * @return Draw when @p out is to be recorded; otherwise why the draw is left out.
     */
    Verdict BuildDraw(const EngineView& view, const wxl_gx_batch& batch, int32_t indexed, Resolver& resolver, draw::Packet& out);
}
