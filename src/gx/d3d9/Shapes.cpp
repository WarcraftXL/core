// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw's shapes on a Direct3D 9 device (see Shapes.hpp).

#include "gx/d3d9/Shapes.hpp"

#include "binding/Binding.hpp"
#include "gx/d3d9/Device.hpp"

#include <algorithm>
#include <memory>

namespace
{
#include "d3d9_draw_shaders.inc"

    using namespace wxl::core;
    using namespace wxl::core::gx::d3d9;

    constexpr UINT  kViewRegister = 240; ///< DrawVs's c240 to c245: above every register the game's programs read
    constexpr UINT  kSurfaceRegister = 200; ///< SurfacePs's c200 to c205: above what the game's pixel programs read
    constexpr DWORD kColorOnly    = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE;

    struct Programs
    {
        IDirect3DVertexShader9*      vertex = nullptr;
        IDirect3DPixelShader9*       pixel  = nullptr;
        IDirect3DPixelShader9*       surface = nullptr;
        IDirect3DPixelShader9*       particle = nullptr;
        IDirect3DVertexDeclaration9* layout = nullptr;
    };

    Device*                     g_override = nullptr;
    std::unique_ptr<D3d9Device> g_game;
    Programs                    g_programs;
    const char*                 g_refusal = nullptr;
    bool                        g_checked = false;
    UINT                        g_maxPrimitives = 0;

    void Drop(Device& device)
    {
        if (g_programs.vertex) device.Release(g_programs.vertex);
        if (g_programs.pixel) device.Release(g_programs.pixel);
        if (g_programs.surface) device.Release(g_programs.surface);
        if (g_programs.particle) device.Release(g_programs.particle);
        if (g_programs.layout) device.Release(g_programs.layout);
        g_programs = Programs{};
    }

    /// The device the shapes are drawn with now: the harness's, or the game's while it has one; what
    /// was made on a device the game has since replaced goes with it.
    Device* Current()
    {
        if (g_override) return g_override;
        void* raw = binding::gx_d3d9_device();
        if (!raw) return nullptr;
        if (!g_game || g_game->Raw() != raw)
        {
            if (g_game) Drop(*g_game);
            g_game    = std::make_unique<D3d9Device>(static_cast<IDirect3DDevice9*>(raw));
            g_checked = false;
        }
        return g_game.get();
    }

    const char* Check(Device& device)
    {
        if (g_checked) return g_refusal;
        g_checked = true;
        g_refusal = nullptr;
        D3DCAPS9 caps{};
        if (FAILED(device.Caps(caps))) return g_refusal = "the Direct3D 9 device does not answer its caps";
        if (caps.VertexShaderVersion < D3DVS_VERSION(3, 0) || caps.PixelShaderVersion < D3DPS_VERSION(3, 0))
            return g_refusal = "the Direct3D 9 device has no shader model 3.0, which the shapes are drawn with";
        if (!(caps.DeclTypes & D3DDTCAPS_UBYTE4)) return g_refusal = "the Direct3D 9 device reads no UBYTE4 vertex data";
        g_maxPrimitives = std::max<UINT>(caps.MaxPrimitiveCount, 1u);
        return nullptr;
    }

    bool Make(Device& device)
    {
        if (g_programs.vertex && g_programs.pixel && g_programs.surface && g_programs.particle && g_programs.layout) return true;
        static const D3DVERTEXELEMENT9 kLayout[] = {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
            {0, 28, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
            D3DDECL_END()};
        if (!g_programs.vertex && FAILED(device.CreateVertexShader(reinterpret_cast<const DWORD*>(kDrawVs), &g_programs.vertex)))
            g_programs.vertex = nullptr;
        if (!g_programs.pixel && FAILED(device.CreatePixelShader(reinterpret_cast<const DWORD*>(kDrawPs), &g_programs.pixel)))
            g_programs.pixel = nullptr;
        if (!g_programs.surface && FAILED(device.CreatePixelShader(reinterpret_cast<const DWORD*>(kSurfacePs), &g_programs.surface)))
            g_programs.surface = nullptr;
        if (!g_programs.particle && FAILED(device.CreatePixelShader(reinterpret_cast<const DWORD*>(kParticlePs), &g_programs.particle)))
            g_programs.particle = nullptr;
        if (!g_programs.layout && FAILED(device.CreateDeclaration(kLayout, &g_programs.layout))) g_programs.layout = nullptr;
        if (g_programs.vertex && g_programs.pixel && g_programs.surface && g_programs.particle && g_programs.layout) return true;
        Drop(device);
        return false;
    }
}

namespace wxl::core::gx::d3d9::shapes
{
    void UseDevice(Device* device)
    {
        if (g_override && g_override != device) Drop(*g_override);
        g_override = device;
        g_checked  = false;
    }

    bool HasDevice() { return g_override || binding::gx_d3d9_device(); }

    const char* Unsupported()
    {
        Device* device = Current();
        if (!device) return "there is no Direct3D 9 device";
        return Check(*device);
    }

    uint32_t Draw(const wxl::gx::shapes::Vertex* vertices, uint32_t tested, uint32_t on_top, const float constants[24],
                  const float viewport[6], std::string* why)
    {
        wxl::gx::shapes::Run runs[2];
        runs[0].count  = tested;
        runs[1].first  = tested;
        runs[1].count  = on_top;
        runs[1].on_top = true;
        return Draw(vertices, runs, 2, nullptr, constants, viewport, why);
    }

    uint32_t Draw(const wxl::gx::shapes::Vertex* vertices, const wxl::gx::shapes::Run* runs, uint32_t run_count,
                  const wxl::gx::shapes::Surface* surfaces, const float constants[24], const float viewport[6], std::string* why)
    {
        uint32_t total = 0;
        for (uint32_t i = 0; i < run_count; ++i) total += runs[i].count;
        if (!total) return 0;
        Device* device = Current();
        if (!device)
        {
            if (why) *why = "there is no Direct3D 9 device";
            return 0;
        }
        if (const char* refusal = Check(*device))
        {
            if (why) *why = refusal;
            return 0;
        }
        if (!Make(*device))
        {
            if (why) *why = "the Direct3D 9 device refused wxl.draw's programs";
            return 0;
        }
        Device& d = *device;

        D3DVIEWPORT9 port{};
        port.X      = static_cast<DWORD>(std::max(viewport[0], 0.0f));
        port.Y      = static_cast<DWORD>(std::max(viewport[1], 0.0f));
        port.Width  = static_cast<DWORD>(std::max(viewport[2], 1.0f));
        port.Height = static_cast<DWORD>(std::max(viewport[3], 1.0f));
        port.MinZ   = std::clamp(viewport[4], 0.0f, 1.0f);
        port.MaxZ   = std::clamp(viewport[5], 0.0f, 1.0f);
        d.SetViewport(port);
        d.SetVertexShader(g_programs.vertex);
        d.SetDeclaration(g_programs.layout);
        d.SetVertexConstants(kViewRegister, constants, 6);
        d.SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        d.SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        d.SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d.SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        d.SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        d.SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d.SetRenderState(D3DRS_COLORWRITEENABLE, kColorOnly); // the scene's alpha (the glow's mask) is left as it is
        d.SetRenderState(D3DRS_FOGENABLE, FALSE);
        d.SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        d.SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        d.SetRenderState(D3DRS_DEPTHBIAS, 0);

        // Whole triangles per call, within the device's limit.
        const UINT per     = std::min<UINT>(g_maxPrimitives ? g_maxPrimitives : 1u, 21844u) * 3u;
        uint32_t   drawn   = 0;
        bool       sampled = false;
        int        zState = -1, program = -1, blend = -1;
        float      alpha  = 1.0f;
        float      lift[4] = {constants[20], constants[21], 1.0f, constants[23]};
        for (uint32_t i = 0; i < run_count; ++i)
        {
            const wxl::gx::shapes::Run& run = runs[i];
            if (run.count < 3) continue;
            // On top: no depth; tested: LESSEQUAL; behind the scene: GREATER.
            const int z = run.on_top ? 0 : run.behind ? 2 : 1;
            if (z != zState)
            {
                if (zState <= 0 || z == 0) d.SetRenderState(D3DRS_ZENABLE, z ? D3DZB_TRUE : D3DZB_FALSE);
                if (z) d.SetRenderState(D3DRS_ZFUNC, z == 2 ? D3DCMP_GREATER : D3DCMP_LESSEQUAL);
                zState = z;
            }
            if (run.alpha != alpha)
            {
                alpha   = run.alpha;
                lift[2] = alpha;
                d.SetVertexConstants(kViewRegister + 5, lift, 1);
            }
            // Surfaces and particles read their numbers and their picture; lines and plain insides none.
            const int  which   = surfaces ? (run.program == wxl::gx::shapes::kSurface ? 1 : run.program == wxl::gx::shapes::kParticle ? 2 : 0) : 0;
            const bool surface = which != 0;
            if (which != program)
            {
                program = which;
                d.SetPixelShader(which == 1 ? g_programs.surface : which == 2 ? g_programs.particle : g_programs.pixel);
            }
            const int b = surface ? std::min<int>(run.blend, 2) : 0;
            if (b != blend)
            {
                blend = b;
                // ALPHA over the scene, ADD as light, MODULATE multiplying it (SurfacePs gives 1 + (c - 1) a).
                static const DWORD kSource[3]      = {D3DBLEND_SRCALPHA, D3DBLEND_SRCALPHA, D3DBLEND_DESTCOLOR};
                static const DWORD kDestination[3] = {D3DBLEND_INVSRCALPHA, D3DBLEND_ONE, D3DBLEND_ZERO};
                d.SetRenderState(D3DRS_SRCBLEND, kSource[b]);
                d.SetRenderState(D3DRS_DESTBLEND, kDestination[b]);
            }
            if (surface)
            {
                const wxl::gx::shapes::Surface& s = surfaces[run.surface];
                wxl::gx::shapes::SurfaceConstants k = s.constants;
                if (!s.texture) k.k[2][3] = 0.0f; // no picture to read
                d.SetPixelConstants(kSurfaceRegister, &k.k[0][0], 6);
                d.SetTexture(0, static_cast<IDirect3DBaseTexture9*>(s.texture));
                const DWORD address = s.repeats ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
                d.SetSamplerState(0, D3DSAMP_ADDRESSU, address);
                d.SetSamplerState(0, D3DSAMP_ADDRESSV, address);
                d.SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                d.SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                d.SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                sampled = true;
            }
            const wxl::gx::shapes::Vertex* from = vertices + run.first;
            for (uint32_t at = 0; at + 3 <= run.count; at += per)
            {
                const UINT n = std::min<UINT>(per, (run.count - at) / 3u * 3u);
                d.DrawUp(D3DPT_TRIANGLELIST, n / 3u, from + at, sizeof(wxl::gx::shapes::Vertex));
                drawn += n;
            }
        }
        if (sampled) d.SetTexture(0, nullptr);

        // What the engine leaves set and never sends again (CGxDeviceD3d::IStateSetD3DDefaults),
        // then everything else it keeps, sent again before its next draw.
        d.SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        d.SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        d.SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        binding::gx_invalidate_state();
        return drawn;
    }

    void Shutdown()
    {
        if (g_override) Drop(*g_override);
        else if (g_game) Drop(*g_game);
        g_game.reset();
        g_checked = false;
    }
}
