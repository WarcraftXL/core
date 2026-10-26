// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// One draw of the engine read out of its device: CGxDevice through the SDK's engine layouts
// (wxl/engine/gx.hpp), its render states, its registers, turned into a draw::Packet as the D3D9
// device's IStateSync turns the same state into D3D9 calls (its viewport, scissor, vertex
// declaration, blend, depth, cull, texture stages, transforms, lights, fog and clip plane). The
// same reading serves the live client (Front) and a frame capture replayed (src/gx/capture/): what
// a pointer of the engine names is asked of a Resolver, which follows it on the live client and
// looks it up in the capture otherwise. Portable: no Windows, no runtime.
//
// Every draw is drawn, the world's included, with the program the renderer has for each of its
// shaders (its own, or the client's record translated). A draw is left out, with the reason, when
//   - it binds a shader whose record the translator refused (D3D9 would have refused it too: the
//     engine then draws without it, which the renderer does not imitate);
//   - its target is a depth texture with no colour texture;
//   - a buffer or a pool it reads is unknown.

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
        /// The renderer's program for the CGxShader @p shader; -1 for one it has none of.
        virtual int Program(const void* shader, bool vertex) = 0;

    protected:
        ~Resolver() = default;
    };

    struct EngineView
    {
        const engine::CGxDevice* device = nullptr;
        const wxl_gx_app_state*  states = nullptr;           ///< m_appRenderStates' elements (GxRenderStates_Last)
        const float*             vertex_constants = nullptr; ///< 256 rows: the vertex program's registers
    };

    enum class Verdict
    {
        Draw,    ///< the packet is filled
        Target,  ///< into a depth texture alone
        Program, ///< a shader whose record the translator refused
        Unready, ///< a buffer or a pool it reads is unknown
        Count,
    };

    const char* VerdictName(Verdict verdict);

    /// The engine's render states this reading uses (EGxRenderState).
    enum State : uint32_t
    {
        kPolygonOffset    = 0,
        kMatDiffuse       = 1,
        kMatEmissive      = 2,
        kMatSpecular      = 3,
        kMatSpecularExp   = 4,
        kNormalizeNormals = 5,
        kBlendingMode     = 6,
        kAlphaRef         = 7,
        kFogStart         = 8,
        kFogEnd           = 9,
        kFogColor         = 10,
        kLighting         = 11,
        kFog              = 12,
        kDepthTest        = 13,
        kDepthFunc        = 14,
        kDepthWrite       = 15,
        kColorWrite       = 16,
        kCulling          = 17,
        kClipPlaneMask    = 18,
        kMultisample      = 19,
        kScissorTest      = 20,
        kTexture0         = 21,
        kColorOp0         = 37,
        kAlphaOp0         = 45,
        kTexGen0          = 53,
        kTextureShader0   = 61,
        kTextureCoord0    = 69,
        kVertexShader     = 77,
        kPixelShader      = 78,
        kPointScale       = 79,
        kPointScaleAttenuation = 80,
        kPointScaleMin    = 81,
        kPointScaleMax    = 82,
        kPointSprite      = 83,
        kBlendFactor      = 84,
        kColorMaterial    = 85,
        kRenderStates     = 86,
    };

    /// Whether the engine draws into a texture now (m_textureTarget).
    bool IntoTexture(const engine::CGxDevice& device);

    /// The rectangle a clear covers, in the target's pixels (x, y, width, height): D3D9 clears the
    /// viewport, the one IXformSetViewport makes (not turned upside down in a texture target).
    void ClearRect(const engine::CGxDevice& device, int32_t rect[4]);

    /**
     * @brief Reads the draw of @p batch (indexed when @p indexed) out of @p view into @p out.
     * @return Draw when @p out is to be recorded; otherwise why the draw is left out.
     */
    Verdict BuildDraw(const EngineView& view, const wxl_gx_batch& batch, int32_t indexed, Resolver& resolver, draw::Packet& out);
}
