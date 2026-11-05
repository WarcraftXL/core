// SPDX-License-Identifier: GPL-3.0-or-later
// The world render seams: OnWorldRender, OnWorldSceneEnd, OnWorldRenderEnd and OnLiquidRender.

#include "client/CGxDevice/FrameHooks.hpp"
#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "runtime/HookPoints.hpp"
#include "wxl/game/Gx.hpp"
#include "wxl/offsets/CGWorldFrame.hpp"
#include "wxl/offsets/Liquid.hpp"

#include <windows.h>
#include <d3d9.h>

namespace
{
    namespace off = wxl::offsets;
    namespace ev  = wxl::events;
    namespace gx  = wxl::game::gx;

    /// One pass of the liquid renderer's material bank (its `this`), indexed by pass.
    struct LiquidPassEntry
    {
        uint32_t unknown00;
        uint32_t count;
        uint32_t instances;
        uint32_t unknown0c;
    };
    static_assert(sizeof(LiquidPassEntry) == 0x10);

    off::CGWorldFrame::RenderWorldFn   g_origRenderWorld   = nullptr;
    off::CGWorldFrame::OnWorldRenderFn g_origWorldScene    = nullptr;
    off::Liquid::CRenderer::FlushFn    g_origLiquidRender  = nullptr;

    /// Once per pass (0 main, 1 secondary), textures bound and waves applied.
    void __fastcall LiquidRenderDetour(void* bank, void* edx, void* transform, int pass)
    {
        const LiquidPassEntry& entry = static_cast<LiquidPassEntry*>(bank)[pass];
        const ev::LiquidRenderArgs args{ bank, transform, pass, entry.count };
        ev::Emit<ev::Event::OnLiquidRender>(args);
        g_origLiquidRender(bank, edx, transform, pass);
    }

    /// OnWorldSceneEnd comes while the world's matrices are still on the device, with the depth surface
    /// the world drew into: taken before the pass, whose post-processing may bind another.
    void __fastcall WorldSceneDetour(void* worldFrame, void* edx)
    {
        IDirect3DSurface9* sceneDepth = nullptr;
        if (auto* device = static_cast<IDirect3DDevice9*>(gx::RawDevice())) device->GetDepthStencilSurface(&sceneDepth);

        const ev::WorldRenderArgs opened{ gx::RawDevice() };
        ev::Emit<ev::Event::OnWorldRender>(opened);
        g_origWorldScene(worldFrame, edx);
        if (ev::Any(ev::Event::OnWorldSceneEnd))
        {
            const ev::WorldSceneEndArgs ended{ gx::RawDevice(), sceneDepth };
            ev::Emit<ev::Event::OnWorldSceneEnd>(ended);
        }
        if (sceneDepth) sceneDepth->Release();
    }

    /// The world -> UI boundary. A function hook survives a device recreate, so the device hooks are
    /// put back from here.
    void __cdecl RenderWorldDetour(void* worldFrame)
    {
        if (auto* device = static_cast<IDirect3DDevice9*>(gx::RawDevice())) wxl::client::frame::EnsureDeviceHooks(device);
        g_origRenderWorld(worldFrame);
        const ev::WorldRenderEndArgs args{ gx::RawDevice() };
        ev::Emit<ev::Event::OnWorldRenderEnd>(args);
    }

    bool InstallRender()
    {
        wxl::client::frame::Install();
        wxl::runtime::hookpoints::Attach("CGWorldFrame::RenderWorld", &RenderWorldDetour, &g_origRenderWorld);
        wxl::runtime::hookpoints::Attach("CGWorldFrame::OnWorldRender", &WorldSceneDetour, &g_origWorldScene);
        wxl::runtime::hookpoints::Attach("Liquid::CRenderer::Flush", &LiquidRenderDetour, &g_origLiquidRender);
        WLOG_INFO("render: hooks installed (EndScene, Present, Reset, WorldFinalize, WorldScenePass, LiquidRenderPass, "
                  "ScenePresent)");
        return true;
    }
}

WXL_REGISTER_FEATURE("render", true, InstallRender)
