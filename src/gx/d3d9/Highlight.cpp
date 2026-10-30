// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.highlight's passes on the stock client's Direct3D 9 device (see Highlight.hpp).

#include "gx/d3d9/Highlight.hpp"

#include "binding/Binding.hpp"
#include "binding/HighlightDrawX86.hpp"
#include "common/Log.hpp"
#include "gx/d3d9/Device.hpp"
#include "runtime/Core.hpp"

#include "wxl/events.h"
#include "wxl/services/highlight.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
#include "d3d9_shaders.inc"

    using namespace wxl::core;
    using namespace wxl::core::gx::d3d9;
    namespace draw = wxl::core::binding::x86::highlight_draw;

    constexpr uint32_t kMaxBatches    = 512;  ///< kept per frame; a highlighted model has a few dozen
    constexpr uint32_t kMaskRegister3 = 200;  ///< MaskPs3's c200 (colour), c201 (glow, width), c202 (alpha test)
    constexpr uint32_t kMaskRegister2 = 29;   ///< MaskPs2's c29 (alpha test), c30 (colour), c31 (glow, width)
    constexpr uint32_t kQuadRegister  = 250;  ///< FullscreenVs's c250: above every register the game's programs read
    constexpr float    kWidthCode     = 32.0f; ///< the mask's width: its alpha times this many pixels
    constexpr float    kMaxOutline    = 32.0f; ///< pixels, once scaled to the render's height
    constexpr float    kMaxGlow       = 128.0f;
    constexpr float    kReferenceHeight = 1080.0f; ///< widths are given for this many lines
    constexpr float    kGlowGain      = 2.0f;  ///< the blurred edge is half as bright as the object's
    constexpr D3DFORMAT kMaskFormat   = D3DFMT_A8R8G8B8;

    struct Kept
    {
        draw::State          state;
        wxl_model_batch_args draw;
        uint32_t             style;
    };

    struct Targets
    {
        UINT                width = 0, height = 0;
        D3DMULTISAMPLE_TYPE samples = D3DMULTISAMPLE_NONE;
        DWORD               quality = 0;
        IDirect3DSurface9*  multiColor = nullptr; ///< multisampled masks, resolved into the textures
        IDirect3DSurface9*  multiExtra = nullptr;
        IDirect3DTexture9*  color = nullptr;
        IDirect3DTexture9*  extra = nullptr;
        IDirect3DTexture9*  glowA = nullptr;      ///< half the size
        IDirect3DTexture9*  glowB = nullptr;
        IDirect3DSurface9*  colorSurface = nullptr;
        IDirect3DSurface9*  extraSurface = nullptr;
        IDirect3DSurface9*  glowASurface = nullptr;
        IDirect3DSurface9*  glowBSurface = nullptr;
    };

    struct Programs
    {
        IDirect3DVertexShader9*      fullscreen = nullptr;
        IDirect3DPixelShader9*       mask3      = nullptr;
        IDirect3DPixelShader9*       mask2      = nullptr;
        IDirect3DPixelShader9*       blur       = nullptr;
        IDirect3DPixelShader9*       composite  = nullptr;
        IDirect3DVertexDeclaration9* quad       = nullptr;
    };

    highlight::Source g_source;
    Device*           g_override = nullptr;  ///< the harness's device
    std::unique_ptr<D3d9Device> g_game;      ///< the game's device behind the interface
    const char*       g_refusal  = nullptr;  ///< what the current device's caps refuse
    bool              g_checked  = false;    ///< g_refusal is known for the current device
    Programs          g_programs;
    Targets           g_targets;

    std::vector<const void*> g_roots;
    std::vector<uint32_t>    g_styleOf;
    std::vector<Kept>        g_kept;
    std::vector<float>       g_constants;    ///< kRows * 4 floats per kept batch
    std::vector<std::pair<const void*, bool>> g_versions; ///< vertex programs seen: of shader model 3
    bool                     g_fixedSaid  = false;
    bool                     g_failedSaid = false;

    template <class T>
    void Drop(Device& device, T*& object)
    {
        if (object) device.Release(object);
        object = nullptr;
    }

    void DropTargets(Device& device)
    {
        Targets& t = g_targets;
        Drop(device, t.colorSurface);
        Drop(device, t.extraSurface);
        Drop(device, t.glowASurface);
        Drop(device, t.glowBSurface);
        Drop(device, t.multiColor);
        Drop(device, t.multiExtra);
        Drop(device, t.color);
        Drop(device, t.extra);
        Drop(device, t.glowA);
        Drop(device, t.glowB);
        t = Targets{};
    }

    void DropPrograms(Device& device)
    {
        Programs& p = g_programs;
        Drop(device, p.fullscreen);
        Drop(device, p.mask3);
        Drop(device, p.mask2);
        Drop(device, p.blur);
        Drop(device, p.composite);
        Drop(device, p.quad);
    }

    /// The device the passes draw with now: the harness's, or the game's while it has one. What
    /// was made on a device the game has since replaced is released through it (each object holds
    /// its device), and made again on the new one.
    Device* Current()
    {
        if (g_override) return g_override;
        void* raw = binding::gx_d3d9_device();
        if (!raw) return nullptr;
        if (!g_game || g_game->Raw() != raw)
        {
            if (g_game)
            {
                DropTargets(*g_game);
                DropPrograms(*g_game);
            }
            g_game    = std::make_unique<D3d9Device>(static_cast<IDirect3DDevice9*>(raw));
            g_checked = false;
            g_versions.clear();
        }
        return g_game.get();
    }

    /// Whether a vertex program is of shader model 3: its pixel program must be too.
    bool Model3(Device& device, IDirect3DVertexShader9* program)
    {
        for (const auto& known : g_versions)
            if (known.first == program) return known.second;
        const bool model3 = (device.VertexShaderVersion(program) & 0xFF00u) >= 0x0300u;
        if (g_versions.size() >= 4096) g_versions.clear();
        g_versions.emplace_back(program, model3);
        return model3;
    }

    /// What the device's caps refuse, once per device: the passes need shader model 3.0 and two
    /// render targets at once, and the composite's length.
    const char* CheckCaps(Device& device)
    {
        D3DCAPS9 caps{};
        if (FAILED(device.Caps(caps))) return "the Direct3D 9 device does not answer for its capabilities";
        if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0) || caps.VertexShaderVersion < D3DVS_VERSION(3, 0))
            return "the Direct3D 9 device has no shader model 3.0, which the highlight's passes are written in";
        if (caps.NumSimultaneousRTs < 2) return "the Direct3D 9 device cannot draw into two render targets at once";
        if (caps.MaxPixelShader30InstructionSlots < kHighlightCompositeSlots)
            return "the Direct3D 9 device's pixel programs are shorter than the highlight's composite pass";
        return nullptr;
    }

    bool MakePrograms(Device& device)
    {
        Programs& p = g_programs;
        if (p.composite) return true;
        static const D3DVERTEXELEMENT9 kQuad[] = {
            {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            D3DDECL_END()};
        const bool made = SUCCEEDED(device.CreateVertexShader(reinterpret_cast<const DWORD*>(kHighlightFullscreen), &p.fullscreen)) &&
                          SUCCEEDED(device.CreatePixelShader(reinterpret_cast<const DWORD*>(kHighlightMaskPs3), &p.mask3)) &&
                          SUCCEEDED(device.CreatePixelShader(reinterpret_cast<const DWORD*>(kHighlightMaskPs2), &p.mask2)) &&
                          SUCCEEDED(device.CreatePixelShader(reinterpret_cast<const DWORD*>(kHighlightBlur), &p.blur)) &&
                          SUCCEEDED(device.CreatePixelShader(reinterpret_cast<const DWORD*>(kHighlightComposite), &p.composite)) &&
                          SUCCEEDED(device.CreateDeclaration(kQuad, &p.quad));
        if (!made) DropPrograms(device);
        return made;
    }

    bool MakeTarget(Device& device, UINT width, UINT height, IDirect3DTexture9*& texture, IDirect3DSurface9*& surface)
    {
        return SUCCEEDED(device.CreateTargetTexture(width, height, kMaskFormat, &texture)) &&
               SUCCEEDED(device.TextureSurface(texture, &surface));
    }

    /// The targets for a scene of this size whose depth buffer is sampled so; false when the
    /// device cannot make them.
    bool EnsureTargets(Device& device, UINT width, UINT height, D3DMULTISAMPLE_TYPE samples, DWORD quality)
    {
        Targets& t = g_targets;
        if (t.color && t.width == width && t.height == height && t.samples == samples && t.quality == quality) return true;
        DropTargets(device);
        t.width   = width;
        t.height  = height;
        t.samples = samples;
        t.quality = quality;
        const UINT halfW = (width + 1) / 2, halfH = (height + 1) / 2;
        bool made = MakeTarget(device, width, height, t.color, t.colorSurface) &&
                    MakeTarget(device, width, height, t.extra, t.extraSurface) &&
                    MakeTarget(device, halfW, halfH, t.glowA, t.glowASurface) &&
                    MakeTarget(device, halfW, halfH, t.glowB, t.glowBSurface);
        if (made && samples != D3DMULTISAMPLE_NONE)
            made = SUCCEEDED(device.CreateRenderTarget(width, height, kMaskFormat, samples, quality, &t.multiColor)) &&
                   SUCCEEDED(device.CreateRenderTarget(width, height, kMaskFormat, samples, quality, &t.multiExtra));
        if (!made) DropTargets(device);
        return made;
    }

    void Sampler(Device& device, DWORD sampler, IDirect3DBaseTexture9* texture, bool linear)
    {
        const DWORD filter = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
        device.SetTexture(sampler, texture);
        device.SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device.SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device.SetSamplerState(sampler, D3DSAMP_MAGFILTER, filter);
        device.SetSamplerState(sampler, D3DSAMP_MINFILTER, filter);
        device.SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    }

    /// A quad over the whole target, 1 to 1 in pixels once moved by half a pixel (c0).
    void Quad(Device& device, UINT width, UINT height)
    {
        const float halfPixel[4] = {-1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height), 0.0f, 0.0f};
        device.SetVertexConstants(kQuadRegister, halfPixel, 1);
        struct Vertex
        {
            float x, y, z, w, u, v;
        };
        const Vertex quad[4] = {
            {-1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f},
            {1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f},
            {-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f},
            {1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f}};
        device.DrawUp(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
    }

    float Channel(uint32_t color, int shift) { return static_cast<float>((color >> shift) & 0xFFu) / 255.0f; }

    /// The kept batches drawn again into the masks; answers how many.
    uint32_t DrawMasks(Device& device, const highlight::Style* styles, uint32_t styleCount)
    {
        device.SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device.SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device.SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device.SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device.SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device.SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device.SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        device.SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
        device.SetRenderState(D3DRS_FOGENABLE, FALSE);
        device.SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device.SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device.SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        device.SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        device.SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device.SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device.SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

        uint32_t drawn = 0, low = draw::kRows, high = 0;
        for (size_t i = 0; i < g_kept.size(); ++i)
        {
            const Kept& kept = g_kept[i];
            if (kept.style >= styleCount) continue;
            const highlight::Style& style = styles[kept.style];
            const draw::State&      s     = kept.state;

            D3DVIEWPORT9 viewport{kept.draw.viewport_x, kept.draw.viewport_y, kept.draw.viewport_width,
                                  kept.draw.viewport_height, kept.draw.viewport_min_z, kept.draw.viewport_max_z};
            device.SetViewport(viewport);
            device.SetDeclaration(static_cast<IDirect3DVertexDeclaration9*>(s.declaration));
            for (UINT stream = 0; stream < draw::kStreams; ++stream)
                device.SetStreamSource(stream, static_cast<IDirect3DVertexBuffer9*>(s.streams[stream]), s.offsets[stream],
                                       s.strides[stream]);
            device.SetIndices(static_cast<IDirect3DIndexBuffer9*>(s.indices));
            auto* program = static_cast<IDirect3DVertexShader9*>(s.vertexShader);
            device.SetVertexShader(program);

            // The constants the game's program read at its draw, in runs of registers set this frame.
            const float* rows = g_constants.data() + i * draw::kRows * 4;
            for (uint32_t row = 0; row < draw::kRows;)
            {
                if (!draw::RowSet(rows + row * 4))
                {
                    ++row;
                    continue;
                }
                uint32_t end = row + 1;
                while (end < draw::kRows && draw::RowSet(rows + end * 4)) ++end;
                device.SetVertexConstants(row, rows + row * 4, end - row);
                low  = std::min(low, row);
                high = std::max(high, end - 1);
                row  = end;
            }

            // The colour and coverage; the glow and the outline's width; the alpha test the game
            // drew with, on its first texture (-1: none, every texel kept).
            const float r = Channel(style.color, 16), g = Channel(style.color, 8), b = Channel(style.color, 0);
            const float alpha = s.alphaRef > 0 && s.texture0 ? static_cast<float>(s.alphaRef) / 255.0f : -1.0f;
            const float color[4] = {r, g, b, 1.0f};
            const float extra[4] = {r * style.strength, g * style.strength, b * style.strength, style.outline / kWidthCode};
            const float test[4]  = {alpha, 0.0f, 0.0f, 0.0f};
            device.SetTexture(0, static_cast<IDirect3DBaseTexture9*>(s.texture0));
            const bool model3 = Model3(device, program);
            device.SetPixelShader(model3 ? g_programs.mask3 : g_programs.mask2);
            if (model3)
            {
                const float mask[12] = {color[0], color[1], color[2], color[3], extra[0], extra[1], extra[2], extra[3],
                                        test[0],  test[1],  test[2],  test[3]};
                device.SetPixelConstants(kMaskRegister3, mask, 3);
            }
            else
            {
                const float mask[12] = {test[0],  test[1],  test[2],  test[3],  color[0], color[1], color[2], color[3],
                                        extra[0], extra[1], extra[2], extra[3]};
                device.SetPixelConstants(kMaskRegister2, mask, 3);
            }

            device.DrawIndexed(static_cast<D3DPRIMITIVETYPE>(kept.draw.primitive_type), kept.draw.base_vertex,
                               kept.draw.min_index, kept.draw.vertex_count, kept.draw.start_index,
                               kept.draw.primitive_count);
            ++drawn;
        }
        // The registers set here hold the batches' values now: each goes back to what the engine
        // last set in it, its copy's (every one of them was set this frame, so its copy has it).
        // Only those: a register the engine did not set this frame holds a value the device keeps
        // from an earlier one, which no copy knows.
        if (g_source.vertexShadow)
        {
            const float* now = reinterpret_cast<const float*>(g_source.vertexShadow);
            for (uint32_t row = low; row <= high && row < draw::kRows;)
            {
                if (!draw::RowSet(now + row * 4))
                {
                    ++row;
                    continue;
                }
                uint32_t end = row + 1;
                while (end <= high && draw::RowSet(now + end * 4)) ++end;
                device.SetVertexConstants(row, now + row * 4, end - row);
                row = end;
            }
        }
        return drawn;
    }

    /// The glow: the second mask blurred into the half-size targets, across then down.
    void Blur(Device& device, float radius)
    {
        const Targets& t    = g_targets;
        const UINT     w    = (t.width + 1) / 2, h = (t.height + 1) / 2;
        const float    step = radius / 4.0f; // four taps each side reach the radius

        device.SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device.SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device.SetDeclaration(g_programs.quad);
        device.SetVertexShader(g_programs.fullscreen);
        device.SetPixelShader(g_programs.blur);

        device.SetRenderTarget(0, t.glowASurface);
        Sampler(device, 0, t.extra, true);
        const float across[4] = {step / static_cast<float>(t.width), 0.0f, 0.0f, 0.0f};
        device.SetPixelConstants(200, across, 1);
        Quad(device, w, h);

        device.SetRenderTarget(0, t.glowBSurface);
        Sampler(device, 0, t.glowA, true);
        const float down[4] = {0.0f, step / static_cast<float>(t.height), 0.0f, 0.0f};
        device.SetPixelConstants(200, down, 1);
        Quad(device, w, h);
    }

    void Composite(Device& device, IDirect3DSurface9* scene, IDirect3DSurface9* depth, float widest, bool glow)
    {
        const Targets& t = g_targets;
        device.SetRenderTarget(0, scene);
        device.SetDepthStencil(depth);
        device.SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device.SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        device.SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device.SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device.SetPixelShader(g_programs.composite);
        Sampler(device, 0, t.color, false);
        Sampler(device, 1, t.extra, false);
        Sampler(device, 2, t.glowB, true);
        const float screen[8] = {1.0f / static_cast<float>(t.width), 1.0f / static_cast<float>(t.height), widest,
                                 glow ? 1.0f : 0.0f, kGlowGain, 0.0f, 0.0f, 0.0f};
        device.SetPixelConstants(200, screen, 2);
        Quad(device, t.width, t.height);
    }

    /**
     * @brief Puts the device back as the engine believes it is.
     *
     * The engine keeps a copy of what it bound and sends only what differs from it; some of it
     * (the index buffer) it does not even compare until it changes its own. So the declaration,
     * the index buffer and the streams go back to what its copy holds, the states the engine never
     * sets go back to their value, and the rest the engine is told to send again (its render
     * states, its samplers and textures, the viewport and the scissor).
     */
    void Restore(Device& device, const draw::State& engine)
    {
        for (DWORD sampler = 0; sampler < 3; ++sampler) device.SetTexture(sampler, nullptr);
        device.SetRenderTarget(1, nullptr);
        device.SetDeclaration(static_cast<IDirect3DVertexDeclaration9*>(engine.declaration));
        for (UINT stream = 0; stream < draw::kStreams; ++stream)
            device.SetStreamSource(stream, static_cast<IDirect3DVertexBuffer9*>(engine.streams[stream]), engine.offsets[stream],
                                   engine.strides[stream]);
        device.SetIndices(static_cast<IDirect3DIndexBuffer9*>(engine.indices));
        device.SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE); // CGxDeviceD3d::IStateSetD3DDefaults; never changed after
        device.SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
        device.SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        binding::gx_invalidate_state();
    }

    void ForgetFrame()
    {
        g_roots.clear();
        g_styleOf.clear();
        g_kept.clear();
        g_constants.clear();
    }
}

namespace wxl::core::gx::d3d9::highlight
{
    void SetSource(const Source& source) { g_source = source; }

    void UseDevice(Device* device)
    {
        g_override = device;
        g_checked  = false;
        g_versions.clear();
    }

    const char* Unsupported()
    {
        if (!g_source.device) return "this runtime cannot read the stock client's renderer";
        if (g_source.enableShaders && *g_source.enableShaders == 0)
            return "the game draws without its shaders (its settings or its device), and highlights redraw models with them";
        Device* device = Current();
        if (!device) return "the game has no Direct3D 9 device (none yet, or it runs on OpenGL)";
        if (!g_checked)
        {
            g_refusal = CheckCaps(*device);
            g_checked = true;
            if (g_refusal) WLOG_WARN(kTag, "wxl.highlight: %s; highlights are not drawn", g_refusal);
        }
        return g_refusal;
    }

    void Begin(const void* const* roots, const uint32_t* styles, uint32_t count)
    {
        ForgetFrame();
        if (!count) return;
        g_roots.assign(roots, roots + count);
        g_styleOf.assign(styles, styles + count);
    }

    bool Batch(const void* render, const wxl_model_batch_args& args)
    {
        if (g_roots.empty() || !render || !g_source.device || g_kept.size() >= kMaxBatches) return false;
        const auto at = std::find(g_roots.begin(), g_roots.end(), args.root_model);
        if (at == g_roots.end()) return false;
        // Translucent parts (blended: a weapon's enchant, a ghost) are not the object's silhouette.
        if (args.blend_mode > 1) return false;
        const void* device = g_source.device();
        if (!device || draw::IntoTexture(device)) return false;

        Kept kept{};
        kept.draw  = args;
        kept.style = g_styleOf[static_cast<size_t>(at - g_roots.begin())];
        if (!draw::ReadState(device, kept.state)) return false;
        if (!kept.state.vertexShader)
        {
            if (!g_fixedSaid) WLOG_INFO(kTag, "wxl.highlight: a model drawn without a vertex program is not highlighted");
            g_fixedSaid = true;
            return false;
        }
        if (!g_source.vertexShadow) return false;
        g_kept.push_back(kept);
        const float* shadow = reinterpret_cast<const float*>(g_source.vertexShadow);
        g_constants.insert(g_constants.end(), shadow, shadow + draw::kRows * 4);
        return true;
    }

    uint32_t Draw(const Style* styles, uint32_t count)
    {
        if (g_kept.empty() || Unsupported())
        {
            ForgetFrame();
            return 0;
        }
        Device* device = Current();
        if (!device || !MakePrograms(*device))
        {
            if (!g_failedSaid) WLOG_WARN(kTag, "wxl.highlight: the device refused the highlight's programs; nothing is drawn");
            g_failedSaid = true;
            ForgetFrame();
            return 0;
        }

        // What the engine believes is bound, read before anything changes it.
        draw::State engine;
        if (g_source.device) draw::ReadBindings(g_source.device(), engine);

        IDirect3DSurface9* scene = nullptr;
        IDirect3DSurface9* depth = nullptr;
        D3DSURFACE_DESC    sceneDesc{}, depthDesc{};
        uint32_t           drawn = 0;
        if (SUCCEEDED(device->GetRenderTarget(0, &scene)) && scene && SUCCEEDED(device->GetDepthStencil(&depth)) && depth &&
            SUCCEEDED(device->SurfaceDesc(scene, sceneDesc)) && SUCCEEDED(device->SurfaceDesc(depth, depthDesc)) &&
            EnsureTargets(*device, sceneDesc.Width, sceneDesc.Height, depthDesc.MultiSampleType, depthDesc.MultiSampleQuality))
        {
            // Widths are given for a render 1080 lines high, and drawn the same at any height.
            const float scale = static_cast<float>(sceneDesc.Height) / kReferenceHeight;
            std::vector<highlight::Style> scaled(styles, styles + count);
            for (highlight::Style& style : scaled)
            {
                style.outline = std::min(style.outline * scale, kMaxOutline);
                style.glow    = std::min(style.glow * scale, kMaxGlow);
            }
            styles = scaled.data();

            const Targets& t = g_targets;
            const bool multi = t.multiColor != nullptr;
            device->SetRenderTarget(0, multi ? t.multiColor : t.colorSurface);
            device->SetRenderTarget(1, multi ? t.multiExtra : t.extraSurface);
            device->Clear(D3DCLEAR_TARGET, 0);
            drawn = DrawMasks(*device, styles, count);
            device->SetRenderTarget(1, nullptr);
            if (multi)
            {
                device->StretchRect(t.multiColor, t.colorSurface);
                device->StretchRect(t.multiExtra, t.extraSurface);
            }

            // The blur reaches past the widest outline, which the composite searches only where
            // the blurred mask reaches.
            float widest = 0.0f, radius = 2.0f;
            bool  glow   = false;
            for (uint32_t i = 0; i < count; ++i)
            {
                widest = std::max(widest, styles[i].outline);
                radius = std::max({radius, styles[i].glow, styles[i].outline + 2.0f});
                glow   = glow || (styles[i].glow > 0.0f && styles[i].strength > 0.0f);
            }
            device->SetDepthStencil(nullptr); // the half-size targets are smaller than the scene's depth
            Blur(*device, radius);
            Composite(*device, scene, depth, widest, glow);
        }
        else if (!g_failedSaid)
        {
            WLOG_WARN(kTag, "wxl.highlight: the scene's target or depth could not be read or matched; nothing is drawn");
            g_failedSaid = true;
        }
        if (scene)
        {
            device->SetRenderTarget(0, scene);
            device->SetDepthStencil(depth);
        }
        Restore(*device, engine);
        device->Release(scene);
        device->Release(depth);
        ForgetFrame();
        return drawn;
    }

    void DeviceLost()
    {
        ForgetFrame();
        // The targets must go before a reset; the programs too, which could survive one, so that
        // nothing of this runtime's keeps a device the game is destroying.
        if (Device* device = g_override ? g_override : (g_game ? g_game.get() : nullptr))
        {
            DropTargets(*device);
            DropPrograms(*device);
        }
    }

    void Shutdown()
    {
        ForgetFrame();
        if (Device* device = g_override ? g_override : (g_game ? g_game.get() : nullptr))
        {
            DropTargets(*device);
            DropPrograms(*device);
        }
        g_game.reset();
        g_checked = false;
        g_versions.clear();
    }
}
