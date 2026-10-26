// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// One draw of the engine read out of its device.

#include "gx/vk/gxfront/Engine.hpp"

#include <algorithm>
#include <cmath>
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

        /// The inverse of @p m; false when it has none.
        bool Inverse(const float* m, float* out)
        {
            float inv[16];
            inv[0]  = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
            inv[4]  = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
            inv[8]  = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
            inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
            inv[1]  = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
            inv[5]  = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
            inv[9]  = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
            inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
            inv[2]  = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
            inv[6]  = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
            inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
            inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
            inv[3]  = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
            inv[7]  = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
            inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
            inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
            const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
            if (det == 0.0f || !std::isfinite(det)) return false;
            for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
            return true;
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

        /// p (a row vector) times the 4x4 @p m.
        void RowTimes(const float p[4], const float* m, float out[4])
        {
            for (int j = 0; j < 4; ++j) out[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + p[3] * m[12 + j];
        }
    }

    const char* VerdictName(Verdict verdict)
    {
        switch (verdict)
        {
        case Verdict::Draw: return "drawn";
        case Verdict::Target: return "into a depth texture alone";
        case Verdict::Program: return "a client program not translated";
        default: return "a buffer not ready";
        }
    }

    bool IntoTexture(const engine::CGxDevice& d) { return d.m_textureTarget[0].m_texture || d.m_textureTarget[1].m_texture; }

    void ClearRect(const engine::CGxDevice& d, int32_t rect[4])
    {
        wxl_gx_viewport vp{};
        std::memcpy(&vp, d.m_viewport, sizeof vp);
        const engine::CRect& window = d.m_curWindowRect;
        rect[0] = static_cast<int32_t>(vp.x_low * window.maxX + 0.5f);
        rect[1] = static_cast<int32_t>((1.0f - vp.y_high) * window.maxY + 0.5f);
        rect[2] = static_cast<int32_t>(vp.x_high * window.maxX - static_cast<float>(rect[0]) + 0.5f);
        rect[3] = static_cast<int32_t>((1.0f - vp.y_low) * window.maxY - static_cast<float>(rect[1]) + 0.5f);
        if (IntoTexture(d)) rect[1] = static_cast<int32_t>(window.maxY * vp.y_low);
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
        const bool     texture = IntoTexture(d);

        if (!d.m_textureTarget[0].m_texture && d.m_textureTarget[1].m_texture) return Verdict::Target;

        out = draw::Packet{};
        out.vertex = static_cast<uint16_t>(pipelines::VertexProgram::Fixed);
        out.pixel  = static_cast<uint16_t>(pipelines::PixelProgram::Fixed);
        if (const void* vs = P(kVertexShader))
        {
            const int program = resolver.Program(vs, true);
            if (program < 0) return Verdict::Program;
            out.vertex = static_cast<uint16_t>(program);
        }
        if (const void* ps = P(kPixelShader))
        {
            const int program = resolver.Program(ps, false);
            if (program < 0) return Verdict::Program;
            out.pixel = static_cast<uint16_t>(program);
        }
        const bool fixedVertex = out.vertex == static_cast<uint16_t>(pipelines::VertexProgram::Fixed);

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
        // D3DRS_MULTISAMPLEANTIALIAS (IRsSendToHw); it changes nothing in a target of one sample.
        out.multisample_off = I(kMultisample) == 0;

        // The viewport and scissor, in the target's pixels (IXformSetViewport, IStateSyncScissorRect);
        // a texture target's are not turned upside down.
        wxl_gx_viewport vp{};
        std::memcpy(&vp, d.m_viewport, sizeof vp);
        const engine::CRect& window = d.m_curWindowRect;
        const uint32_t       x      = static_cast<uint32_t>(vp.x_low * window.maxX + 0.5f);
        uint32_t             y      = static_cast<uint32_t>((1.0f - vp.y_high) * window.maxY + 0.5f);
        out.viewport[2]             = static_cast<float>(static_cast<uint32_t>(vp.x_high * window.maxX - static_cast<float>(x) + 0.5f));
        out.viewport[3]             = static_cast<float>(static_cast<uint32_t>((1.0f - vp.y_low) * window.maxY - static_cast<float>(y) + 0.5f));
        if (texture) y = static_cast<uint32_t>(window.maxY * vp.y_low);
        out.viewport[0]             = static_cast<float>(x);
        out.viewport[1]             = static_cast<float>(y);
        out.viewport[4]             = vp.z_low;
        out.viewport[5]             = vp.z_high;
        if (I(kScissorTest))
        {
            const engine::CRect& s = d.m_scissorRect;
            int32_t left   = static_cast<int32_t>(s.minX * window.maxX + 0.5f);
            int32_t right  = static_cast<int32_t>(s.maxX * window.maxX + 1.0f);
            int32_t top    = static_cast<int32_t>((1.0f - s.maxY) * window.maxY + 0.5f);
            int32_t bottom = static_cast<int32_t>(1.0f + (1.0f - s.minY) * window.maxY);
            if (texture)
            {
                top    = static_cast<int32_t>(s.minY * window.maxY + 0.5f);
                bottom = static_cast<int32_t>(1.0f + s.maxY * window.maxY);
            }
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

        // Every stage's texture and sampler (ISetTexture).
        for (uint32_t stage = 0; stage < draw::kStages; ++stage)
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

        // D3D9's fog: applied after a pixel program below ps_3_0 and after fixed function (the
        // renderer asks only the programs it applies to), with the vertex's fog factor.
        const bool fog = (masters & kMasterFog) && I(kFog);
        if (fog) out.flags |= shaders::kFlagFog;
        out.fog_color = U(kFogColor);

        // Points: their size (D3D9's POINTSIZE where a vertex program writes none), as sprites.
        out.point_size = std::max(F(kPointScale), 1.0f);
        if (!std::isfinite(out.point_size)) out.point_size = 1.0f;
        // D3D9 clamps every point's size to POINTSIZE_MIN..POINTSIZE_MAX (the engine's 0..1 by
        // default; the weather's sprites set 24 at most), the device's own limit applied after.
        const float low = F(kPointScaleMin), high = F(kPointScaleMax);
        out.point_range[0] = std::isfinite(low) ? std::max(low, 0.0f) : 0.0f;
        out.point_range[1] = std::isfinite(high) ? std::max(high, out.point_range[0]) : out.point_range[0];
        if (I(kPointSprite) && batch.primitive == draw::kPoints) out.flags |= shaders::kFlagPointSprite;

        draw::DrawData& data = out.data;
        float world[16], viewMatrix[16];
        Copy(world, Top(d.m_xforms[8]));
        Copy(viewMatrix, Top(d.m_xforms[10]));

        // The user clip plane (ClipPlaneSet's plane 0, the only one the engine sets): against the
        // clip-space position with a vertex program, as D3D9 takes it; in world space with fixed
        // function, carried to clip space here.
        if ((U(kClipPlaneMask) & 1) && d.m_caps.m_maxClipPlanes > 0)
        {
            const engine::C4Plane& plane = d.m_clipPlane[0];
            const float            p[4]  = {plane.n.x, plane.n.y, plane.n.z, plane.d};
            out.flags |= shaders::kFlagClip;
            if (!fixedVertex) std::memcpy(out.clip_plane, p, sizeof p);
            else
            {
                // A world point x: x . p = 0; in clip space c = x * VP, so p' = VP^-1 p (a column).
                float vp4[16], projection[16], inverse[16];
                Copy(projection, d.m_projNative);
                Multiply(viewMatrix, projection, vp4);
                if (Inverse(vp4, inverse))
                    for (int i = 0; i < 4; ++i)
                        out.clip_plane[i] = inverse[i * 4] * p[0] + inverse[i * 4 + 1] * p[1] + inverse[i * 4 + 2] * p[2] + inverse[i * 4 + 3] * p[3];
                else
                    out.flags &= ~shaders::kFlagClip;
            }
        }

        // The cascade of fixed function's pixel stages: a stage without a texture ends it
        // (ISetTexture's DISABLE).
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
        if (!fixedVertex)
        {
            // The interface's program reads c0..c3.
            if (view.vertex_constants) std::memcpy(data.constants, view.vertex_constants, sizeof data.constants);
            return Verdict::Draw;
        }

        // The fixed function's transforms (IStateSyncXforms): world then view, the native projection.
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

        // The colour (IStateSyncLights, IStateSyncMaterial, ISetMaterial): D3D9's lighting with the
        // engine's four lights when Lighting is on; with it off, the vertex's colour, or with no
        // vertex colour and a material that is not white, D3D9's ambient-only lighting.
        const bool     vertexColor = (d.m_primVertexMask & (1u << 4)) != 0;
        const uint32_t diffuse     = U(kMatDiffuse);
        float          dif[4], emi[4], spe[4];
        ArgbToFloats(diffuse, dif);
        ArgbToFloats(U(kMatEmissive), emi);
        ArgbToFloats(U(kMatSpecular), spe);
        std::memcpy(data.matDiffuse, dif, sizeof dif);
        std::memcpy(data.matAmbient, dif, sizeof dif);
        std::memcpy(data.matEmissive, emi, sizeof emi);
        std::memcpy(data.matSpecular, spe, sizeof spe);
        const float power   = F(kMatSpecularExp);
        data.matSpecular[3] = power;
        if (vertexColor)
        {
            const int32_t material = I(kColorMaterial);
            data.sources[0] = data.sources[1] = material == 0 ? 1 : 0;
            data.sources[2] = material == 2 ? 1 : 0;
            data.sources[3] = material == 1 ? 1 : 0;
        }
        if ((masters & kMasterLighting) && I(kLighting))
        {
            data.lighting[1] = 1;
            data.lighting[2] = power > 0.0f && U(kMatSpecular) != 0 ? 1 : 0;
            data.lighting[3] = I(kNormalizeNormals) != 0 ? 1 : 0;
            const auto* lights = reinterpret_cast<const wxl_gx_light*>(d.m_lights);
            for (int i = 0; i < 4; ++i)
            {
                const wxl_gx_light& l = lights[i];
                if (!l.enable) continue;
                data.lighting[0] |= 1 << i;
                // In camera space: a position through the view, a direction towards the light.
                float position[4];
                if (l.dir[3] == 1.0f)
                {
                    const float p[4] = {l.dir[0], l.dir[1], l.dir[2], 1.0f};
                    RowTimes(p, viewMatrix, position);
                    position[3] = 1.0f;
                }
                else
                {
                    const float p[4] = {-l.dir[0], -l.dir[1], -l.dir[2], 0.0f};
                    RowTimes(p, viewMatrix, position);
                    const float length = std::sqrt(position[0] * position[0] + position[1] * position[1] + position[2] * position[2]);
                    for (int c = 0; c < 3; ++c) position[c] = length > 0.0f ? position[c] / length : 0.0f;
                    position[3] = 0.0f;
                }
                std::memcpy(data.lightPosition[i], position, sizeof position);
                for (int c = 0; c < 3; ++c)
                {
                    data.lightDiffuse[i][c]  = l.diffuse[c];
                    data.lightAmbient[i][c]  = l.ambient[c];
                    data.lightSpecular[i][c] = l.specular[c];
                }
                data.lightAttenuation[i][0] = l.attenuation[0];
                data.lightAttenuation[i][1] = l.attenuation[1];
                data.lightAttenuation[i][2] = l.attenuation[2];
                data.lightAttenuation[i][3] = 3.402823466e38f; // D3D9's MaxLightRange
            }
        }
        else if (!vertexColor && diffuse != 0xFFFFFFFFu)
        {
            data.lighting[1] = 1;
            data.globalAmbient[0] = data.globalAmbient[1] = data.globalAmbient[2] = data.globalAmbient[3] = 1.0f;
        }

        // D3D9's linear vertex fog on the view depth.
        if (fog)
        {
            const float start = F(kFogStart), end = F(kFogEnd);
            const float range = end - start;
            data.fog[0]       = range != 0.0f ? -1.0f / range : 0.0f;
            data.fog[1]       = range != 0.0f ? end / range : 1.0f;
            data.fog[2]       = 1.0f;
        }
        return Verdict::Draw;
    }
}
