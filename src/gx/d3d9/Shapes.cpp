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
    constexpr DWORD kColorOnly    = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE;

    struct Programs
    {
        IDirect3DVertexShader9*      vertex = nullptr;
        IDirect3DPixelShader9*       pixel  = nullptr;
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
        if (g_programs.vertex && g_programs.pixel && g_programs.layout) return true;
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
        if (!g_programs.layout && FAILED(device.CreateDeclaration(kLayout, &g_programs.layout))) g_programs.layout = nullptr;
        if (g_programs.vertex && g_programs.pixel && g_programs.layout) return true;
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
        if (!tested && !on_top) return 0;
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
        d.SetPixelShader(g_programs.pixel);
        d.SetDeclaration(g_programs.layout);
        d.SetVertexConstants(kViewRegister, constants, 6);
        d.SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        d.SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        d.SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        d.SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d.SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d.SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        d.SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        d.SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        d.SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d.SetRenderState(D3DRS_COLORWRITEENABLE, kColorOnly); // the scene's alpha (the glow's mask) is left as it is
        d.SetRenderState(D3DRS_FOGENABLE, FALSE);
        d.SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        d.SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        d.SetRenderState(D3DRS_DEPTHBIAS, 0);

        // Whole triangles per call, within the device's limit.
        const UINT per = std::min<UINT>(g_maxPrimitives ? g_maxPrimitives : 1u, 21844u) * 3u;
        uint32_t   drawn = 0;
        const auto run = [&](const wxl::gx::shapes::Vertex* from, uint32_t count) {
            for (uint32_t at = 0; at + 3 <= count; at += per)
            {
                const UINT n = std::min<UINT>(per, (count - at) / 3u * 3u);
                d.DrawUp(D3DPT_TRIANGLELIST, n / 3u, from + at, sizeof(wxl::gx::shapes::Vertex));
                drawn += n;
            }
        };
        run(vertices, tested);
        if (on_top)
        {
            d.SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
            run(vertices + tested, on_top);
        }

        // What the engine leaves set and never sends again (CGxDeviceD3d::IStateSetD3DDefaults),
        // then everything else it keeps, sent again before its next draw.
        d.SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
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
