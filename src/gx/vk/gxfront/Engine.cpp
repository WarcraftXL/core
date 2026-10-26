// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// One draw of the engine read out of its device.

#include "gx/vk/gxfront/Engine.hpp"

#include <algorithm>
#include <cstring>

namespace wxl::vk::gxfront
{
    namespace
    {
        // CGxDevice::m_appMasterEnables (EGxMasterEnables).
        constexpr uint32_t kMasterLighting   = 1u << 0;
        constexpr uint32_t kMasterFog        = 1u << 1;
        constexpr uint32_t kMasterDepthTest  = 1u << 2;
        constexpr uint32_t kMasterDepthWrite = 1u << 3;
        constexpr uint32_t kMasterColorWrite = 1u << 4;
        constexpr uint32_t kMasterCulling    = 1u << 5;
        constexpr uint32_t kMasterFill       = 1u << 8;

        constexpr uint32_t kIdentity = 0x1; // CGxMatrixStack::F_Identity

        // D3D's TEXTURETRANSFORMFLAGS.
        constexpr int32_t kCount2    = 2;
        constexpr int32_t kCount3    = 3;
        constexpr int32_t kProjected = 0x100;

        /// The engine's attribute sizes by type (the D3D9 device's s_gxAttribToD3dAttribSize).
        constexpr uint32_t kAttributeSize[7] = {4, 4, 4, 8, 12, 4, 4};

        void Copy(float out[16], const engine::C44Matrix& m) { std::memcpy(out, &m, sizeof(float) * 16); }

        /// a * b, the engine's C44Matrix::operator*: rows of a by columns of b.
        void Multiply(const float* a, const float* b, float* out)
        {
            float r[16];
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
            std::memcpy(out, r, sizeof r);
        }

        const engine::C44Matrix& Top(const engine::CGxMatrixStack& stack) { return stack.m_mtx[std::min<uint32_t>(stack.m_level, 3)]; }
        bool IsIdentity(const engine::CGxMatrixStack& stack) { return (stack.m_flags[std::min<uint32_t>(stack.m_level, 3)] & kIdentity) != 0; }

        void ArgbToFloats(uint32_t argb, float out[4])
        {
            out[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
            out[1] = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
            out[2] = static_cast<float>(argb & 0xFF) / 255.0f;
            out[3] = static_cast<float>(argb >> 24) / 255.0f;
        }
    }

    const char* VerdictName(Verdict verdict)
    {
        switch (verdict)
        {
        case Verdict::Draw: return "drawn";
        case Verdict::Target: return "into a texture target";
        case Verdict::World: return "in the world's pass";
        case Verdict::Perspective: return "3D outside the world";
        case Verdict::Program: return "a client program not translated yet";
        default: return "a buffer not ready";
        }
    }

    void ClearRect(const engine::CGxDevice& d, int32_t rect[4])
    {
        wxl_gx_viewport vp{};
        std::memcpy(&vp, d.m_viewport, sizeof vp);
        const engine::CRect& window = d.m_curWindowRect;
        rect[0] = static_cast<int32_t>(vp.x_low * window.maxX + 0.5f);
        rect[1] = static_cast<int32_t>((1.0f - vp.y_high) * window.maxY + 0.5f);
        rect[2] = static_cast<int32_t>(vp.x_high * window.maxX - static_cast<float>(rect[0]) + 0.5f);
        rect[3] = static_cast<int32_t>((1.0f - vp.y_low) * window.maxY - static_cast<float>(rect[1]) + 0.5f);
    }

    Verdict BuildDraw(const EngineView& view, const wxl_gx_batch& batch, int32_t indexed, Resolver& resolver, draw::Packet& out)
    {
        const engine::CGxDevice& d  = *view.device;
        const wxl_gx_app_state*  rs = view.states;
        auto I = [rs](uint32_t which) { return rs[which].value.data.i[0]; };
        auto U = [rs](uint32_t which) { return rs[which].value.data.u[0]; };
        auto F = [rs](uint32_t which) { return rs[which].value.data.f[0]; };
        auto P = [rs](uint32_t which) { return static_cast<const void*>(rs[which].value.data.p); };
        const uint32_t masters = d.m_appMasterEnables;

        if (d.m_textureTarget[0].m_texture || d.m_textureTarget[1].m_texture) return Verdict::Target;
        if (view.world) return Verdict::World;

        out = draw::Packet{};
        if (const void* vs = P(kVertexShader))
        {
            const int program = resolver.Program(vs, true);
            if (program < 0) return Verdict::Program;
            out.vertex = static_cast<pipelines::VertexProgram>(program);
        }
        if (const void* ps = P(kPixelShader))
        {
            const int program = resolver.Program(ps, false);
            if (program < 0) return Verdict::Program;
            out.pixel = static_cast<pipelines::PixelProgram>(program);
        }
        // Fixed function in perspective (the native projection's w taken from z, XformSetProjection's
        // d3 of 0): a model's work outside the world, not the interface's.
        if (out.vertex == pipelines::VertexProgram::Fixed && d.m_projNative.d3 == 0.0f) return Verdict::Perspective;

        // The vertex: D3D9's declaration from the attributes the mask names, one stream per buffer,
        // each stream as wide as its attributes (IStateSyncVertexPtrs).
        const auto* attribs = reinterpret_cast<const wxl_gx_vertex_attrib*>(d.m_primVertexFormatAttrib);
        const void* streamBufs[14] = {};
        for (uint32_t a = 0; a < 14; ++a)
        {
            if (!(d.m_primVertexMask & (1u << a))) continue;
            const void* buf    = d.m_primVertexFormatBuf[a];
            uint32_t    stream = 0;
            while (stream < out.stream_count && streamBufs[stream] != buf) ++stream;
            if (stream == out.stream_count)
            {
                const engine::CGxBuf* fields = resolver.Buffer(buf);
                draw::Pool*           pool   = fields ? resolver.Pool(fields->m_pool) : nullptr;
                if (!pool) return Verdict::Unready;
                streamBufs[stream]          = buf;
                out.streams[stream].pool    = pool;
                out.streams[stream].offset  = fields->m_index;
                out.streams[stream].stride  = 0;
                ++out.stream_count;
            }
            const wxl_gx_vertex_attrib& attrib = attribs[a];
            out.streams[stream].stride += attrib.type < 7 ? kAttributeSize[attrib.type] : 0;
            draw::Attribute& at = out.attributes[out.attribute_count++];
            at.location         = static_cast<uint8_t>(attrib.attrib);
            at.type             = static_cast<uint8_t>(attrib.type);
            at.stream           = static_cast<uint8_t>(stream);
            at.offset           = attrib.offset;
        }

        out.primitive = batch.primitive;
        out.count     = batch.count;
        out.indexed   = indexed != 0;
        if (out.indexed)
        {
            const engine::CGxBuf* fields = resolver.Buffer(d.m_primIndexBuf);
            out.indices                  = fields ? resolver.Pool(fields->m_pool) : nullptr;
            if (!out.indices) return Verdict::Unready;
            out.first_index = batch.start + (fields->m_index >> 1);
        }

        // Blend, colour, alpha, depth, cull, fill (IRsSendToHw and IStateSyncEnables).
        out.blend        = static_cast<uint32_t>(std::clamp(I(kBlendingMode), 0, static_cast<int32_t>(draw::kBlendModes) - 1));
        const uint32_t w = (masters & kMasterColorWrite) ? U(kColorWrite) : 0;
        // The engine's bits are red 1, blue 2, green 4, alpha 8.
        out.color_mask   = ((w & 1) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((w & 4) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                         ((w & 2) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((w & 8) ? VK_COLOR_COMPONENT_A_BIT : 0);
        out.blend_factor = static_cast<float>(static_cast<uint32_t>(F(kBlendFactor) * 255.0f) & 0xFF) / 255.0f;
        out.alpha_ref    = I(kAlphaRef) > 0 ? static_cast<float>(std::min(I(kAlphaRef), 255)) : 0.0f;
        out.depth_test   = (masters & kMasterDepthTest) && I(kDepthTest);
        out.depth_write  = (masters & kMasterDepthWrite) && I(kDepthWrite);
        static const VkCompareOp kCompare[4] = {VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL,
                                                VK_COMPARE_OP_LESS};
        out.depth_compare = kCompare[std::clamp(I(kDepthFunc), 0, 3)];
        out.depth_bias    = d.m_caps.m_depthBias ? -F(kPolygonOffset) : 0.0f;
        const int32_t cull = (masters & kMasterCulling) ? std::min(I(kCulling), 2) : 0;
        // D3D9's CW culls clockwise faces: with the y flip in the programs and a clockwise front
        // face, that is Vulkan's front; CCW its back.
        out.cull      = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        out.wireframe = (masters & kMasterFill) == 0;

        // The viewport and scissor, in the window's pixels (IXformSetViewport, IStateSyncScissorRect).
        wxl_gx_viewport vp{};
        std::memcpy(&vp, d.m_viewport, sizeof vp);
        const engine::CRect& window = d.m_curWindowRect;
        const uint32_t       x      = static_cast<uint32_t>(vp.x_low * window.maxX + 0.5f);
        const uint32_t       y      = static_cast<uint32_t>((1.0f - vp.y_high) * window.maxY + 0.5f);
        out.viewport[0]             = static_cast<float>(x);
        out.viewport[1]             = static_cast<float>(y);
        out.viewport[2]             = static_cast<float>(static_cast<uint32_t>(vp.x_high * window.maxX - static_cast<float>(x) + 0.5f));
        out.viewport[3]             = static_cast<float>(static_cast<uint32_t>((1.0f - vp.y_low) * window.maxY - static_cast<float>(y) + 0.5f));
        out.viewport[4]             = vp.z_low;
        out.viewport[5]             = vp.z_high;
        if (I(kScissorTest))
        {
            const engine::CRect& s = d.m_scissorRect;
            int32_t left   = static_cast<int32_t>(s.minX * window.maxX + 0.5f);
            int32_t right  = static_cast<int32_t>(s.maxX * window.maxX + 1.0f);
            int32_t top    = static_cast<int32_t>((1.0f - s.maxY) * window.maxY + 0.5f);
            int32_t bottom = static_cast<int32_t>(1.0f + (1.0f - s.minY) * window.maxY);
            left           = std::max(left, 0);
            top            = std::max(top, 0);
            right          = std::min(right, static_cast<int32_t>(window.maxX));
            bottom         = std::min(bottom, static_cast<int32_t>(window.maxY));
            out.scissor[0] = left;
            out.scissor[1] = top;
            out.scissor[2] = std::max(right - left, 0);
            out.scissor[3] = std::max(bottom - top, 0);
        }
        else
        {
            out.scissor[2] = static_cast<int32_t>(window.maxX);
            out.scissor[3] = static_cast<int32_t>(window.maxY);
        }

        // Textures of stages 0 and 1, and their samplers (ISetTexture).
        for (uint32_t stage = 0; stage < 2; ++stage)
        {
            const void* tex = P(kTexture0 + stage);
            if (!tex) continue;
            const engine::CGxTex* fields = nullptr;
            out.textures[stage]          = resolver.Texture(tex, fields);
            if (out.textures[stage] && fields)
            {
                resources::SamplerKey& key = out.samplers[stage];
                key.filter                 = static_cast<uint8_t>(fields->m_flags.m_filter);
                key.wrap_u                 = static_cast<uint8_t>(fields->m_flags.m_wrapU);
                key.wrap_v                 = static_cast<uint8_t>(fields->m_flags.m_wrapV);
                key.anisotropy             = static_cast<uint8_t>(std::clamp<uint32_t>(fields->m_flags.m_maxAnisotropy, 1, 16));
            }
        }

        draw::DrawData& data = out.data;
        // The cascade of fixed function's pixel stages, whichever program the vertex has: a stage
        // without a texture ends it (ISetTexture's DISABLE).
        data.ops[0] = data.ops[1] = data.ops[2] = data.ops[3] = -1;
        if (out.textures[0])
        {
            data.ops[0] = I(kColorOp0);
            data.ops[1] = I(kAlphaOp0);
            if (out.textures[1])
            {
                data.ops[2] = I(kColorOp0 + 1);
                data.ops[3] = I(kAlphaOp0 + 1);
            }
        }
        if (out.vertex != pipelines::VertexProgram::Fixed)
        {
            // The interface's program reads c0..c3.
            if (view.vertex_constants) std::memcpy(data.constants, view.vertex_constants, sizeof data.constants);
        }
        else
        {
            // The fixed function's transforms (IStateSyncXforms): world then view, the native projection.
            float world[16], viewMatrix[16];
            Copy(world, Top(d.m_xforms[8]));
            Copy(viewMatrix, Top(d.m_xforms[10]));
            Multiply(world, viewMatrix, data.worldView);
            Copy(data.projection, d.m_projNative);

            // Each stage's coordinate (ISetTexCoordIndex, ISetTexGen, IXformSetTex).
            for (uint32_t stage = 0; stage < 2; ++stage)
            {
                const engine::CGxMatrixStack& texGen = d.m_texGen[stage];
                const engine::CGxMatrixStack& xform  = d.m_xforms[stage];
                int32_t                       flags  = 0;
                float*                        m      = data.texMatrix[stage];
                switch (I(kTextureShader0 + stage))
                {
                case 0:
                    Copy(m, Top(texGen));
                    flags = IsIdentity(texGen) ? 0 : kCount3;
                    break;
                case 1:
                {
                    float a[16], b[16];
                    Copy(a, Top(texGen));
                    Copy(b, Top(xform));
                    Multiply(a, b, m);
                    if (I(kTexGen0 + stage)) flags = kCount3;
                    else
                    {
                        // COUNT2: the translation of row 3 moves to row 2, where D3D reads a 2D one.
                        flags = kCount2;
                        m[8]  = m[12];
                        m[9]  = m[13];
                    }
                    break;
                }
                case 2:
                {
                    float a[16], b[16];
                    Copy(a, Top(texGen));
                    Copy(b, Top(xform));
                    Multiply(a, b, m);
                    flags = kCount3 | kProjected;
                    break;
                }
                default: break;
                }
                data.texgen[stage * 2]     = I(kTexGen0 + stage);
                data.texgen[stage * 2 + 1] = flags;
                data.inputs[stage]         = I(kTextureCoord0 + stage);
            }

            // The colour (IStateSyncLights): the vertex's, or with no vertex colour and a material
            // that is not white, D3D9's ambient-only lighting: the material's diffuse plus its
            // emissive. The engine's lights themselves come with the world (V3/V4).
            const bool     vertexColor = (d.m_primVertexMask & (1u << 4)) != 0;
            const uint32_t diffuse     = U(kMatDiffuse);
            if (!vertexColor && diffuse != 0xFFFFFFFFu)
            {
                float dif[4], emi[4];
                ArgbToFloats(diffuse, dif);
                ArgbToFloats(U(kMatEmissive), emi);
                data.inputs[2]   = 1;
                data.material[0] = std::min(dif[0] + emi[0], 1.0f);
                data.material[1] = std::min(dif[1] + emi[1], 1.0f);
                data.material[2] = std::min(dif[2] + emi[2], 1.0f);
                data.material[3] = dif[3];
            }
            (void)kMasterLighting;

            // D3D9's linear vertex fog on the view depth.
            if ((masters & kMasterFog) && I(kFog))
            {
                const float start = F(kFogStart), end = F(kFogEnd);
                const float range = end - start;
                data.fog[0]       = range != 0.0f ? -1.0f / range : 0.0f;
                data.fog[1]       = range != 0.0f ? end / range : 1.0f;
                data.fog[2]       = 1.0f;
                ArgbToFloats(U(kFogColor), data.fogColor);
            }
        }
        return Verdict::Draw;
    }
}
