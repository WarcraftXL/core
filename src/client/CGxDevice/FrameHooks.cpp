// SPDX-License-Identifier: GPL-3.0-or-later

#include "client/CGxDevice/FrameHooks.hpp"

#include "common/Log.hpp"
#include "common/Mem.hpp"
#include "engine/events/Event.hpp"
#include "engine/input/Input.hpp"
#include "runtime/HookPoints.hpp"
#include "wxl/game/Gx.hpp"
#include "wxl/offsets/CGxDevice.hpp"
#include "wxl/offsets/CGxDeviceD3d.hpp"
#include "wxl/offsets/free/G.hpp"

#include <windows.h>
#include <d3d9.h>

namespace wxl::client::frame
{
    namespace
    {
        namespace off = wxl::offsets;
        namespace ev  = wxl::events;
        namespace gx  = wxl::game::gx;

        using EndSceneFn = long(__stdcall*)(void*);
        using PresentFn  = long(__stdcall*)(void*, const void*, const void*, void*, const void*);
        using ResetFn    = long(__stdcall*)(void*, D3DPRESENT_PARAMETERS*);

        // IDirect3DDevice9 vtable slots.
        constexpr unsigned kReset = 16, kPresent = 17, kEndScene = 42;

        constexpr unsigned kMaxResetLines       = 16;
        constexpr unsigned kMaxResetFailedLines = 8;

        EndSceneFn               g_origEndScene     = nullptr;
        PresentFn                g_origPresent      = nullptr;
        ResetFn                  g_origReset        = nullptr;
        off::GxScenePresentFn    g_origScenePresent = nullptr;
        IDirect3DDevice9*        g_hookedDevice     = nullptr;
        bool                     g_sawD3d9          = false; ///< for good: then the vtable hooks raise the frame events

        long __stdcall EndSceneDetour(void* device)
        {
            const ev::EndSceneArgs args{ device };
            ev::Emit<ev::Event::OnEndScene>(args);
            return g_origEndScene(device);
        }

        /// OnFrame just before the buffers swap.
        long __stdcall PresentDetour(void* device, const void* source, const void* destination, void* window,
                                     const void* dirty)
        {
            const ev::FrameArgs args{ device };
            ev::Emit<ev::Event::OnFrame>(args);
            return g_origPresent(device, source, destination, window, dirty);
        }

        /// D3D9 fails a Reset while a D3DPOOL_DEFAULT resource lives: OnDeviceLost lets subscribers
        /// release theirs, OnDeviceReset lets them make them again.
        long __stdcall ResetDetour(void* device, D3DPRESENT_PARAMETERS* params)
        {
            static unsigned resetLines = 0;
            const bool      logThis    = resetLines < kMaxResetLines;
            if (logThis)
            {
                ++resetLines;
                WLOG_INFO("render: Reset begin %ux%u windowed=%u", params ? params->BackBufferWidth : 0,
                          params ? params->BackBufferHeight : 0, params ? static_cast<unsigned>(params->Windowed) : 0);
            }

            const ev::DeviceResetArgs args{ device, params };
            ev::Emit<ev::Event::OnDeviceLost>(args);
            const long result = g_origReset(device, params);
            if (SUCCEEDED(result))
            {
                ev::Emit<ev::Event::OnDeviceReset>(args);
                if (logThis) WLOG_INFO("render: Reset ok");
                return result;
            }
            static unsigned failedLines = 0;
            if (failedLines < kMaxResetFailedLines)
            {
                ++failedLines;
                WLOG_WARN("render: Reset failed hr=0x%08lX", static_cast<unsigned long>(result));
            }
            return result;
        }

        /// A frame without a D3D9 device (a backend that makes none): OnEndScene then OnFrame, with a
        /// null device, before the present. With D3D9 the vtable hooks raise them, never both.
        void RaiseFrameWithoutD3d9(void* graphics)
        {
            WLOG_INFO_ONCE("render: no IDirect3DDevice9 in this process; OnEndScene and OnFrame fire from "
                           "GxScenePresent with a null device (OnDeviceLost/OnDeviceReset do not apply: nothing "
                           "is lost on a resize)");
            wxl::input::FollowWindow(wxl::game::At<HWND>(graphics, off::CGxDeviceD3d::fields::m_hwnd));
            const ev::EndSceneArgs endScene{ nullptr };
            ev::Emit<ev::Event::OnEndScene>(endScene);
            const ev::FrameArgs frame{ nullptr };
            ev::Emit<ev::Event::OnFrame>(frame);
        }

        void __cdecl ScenePresentDetour()
        {
            if (!g_sawD3d9)
            {
                void* graphics = gx::RawGraphicsDevice();
                if (gx::RawDevice()) g_sawD3d9 = true;
                else if (graphics && wxl::game::At<uint32_t>(graphics, off::CGxDevice::fields::m_context))
                    RaiseFrameWithoutD3d9(graphics);
            }
            g_origScenePresent();
        }

        template <class Fn>
        void SwapEntry(void** vtable, unsigned index, Fn* detour, Fn** original)
        {
            wxl::mem::SwapPointer(&vtable[index], reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
        }
    }

    void EnsureDeviceHooks(IDirect3DDevice9* device)
    {
        if (!device || g_hookedDevice == device) return;
        void** vtable = *reinterpret_cast<void***>(device);
        if (vtable[kPresent] != reinterpret_cast<void*>(&PresentDetour))
        {
            SwapEntry(vtable, kEndScene, &EndSceneDetour, &g_origEndScene);
            SwapEntry(vtable, kPresent, &PresentDetour, &g_origPresent);
            SwapEntry(vtable, kReset, &ResetDetour, &g_origReset);
            WLOG_INFO("render: device hooks installed (dev=%p)", static_cast<void*>(device));
        }
        g_hookedDevice = device;
    }

    void Install()
    {
        if (void* device = gx::RawDevice()) EnsureDeviceHooks(static_cast<IDirect3DDevice9*>(device));
        else WLOG_WARN("render: device not up, vtable hooks deferred to first world finalize");
        wxl::runtime::hookpoints::Attach("GxScenePresent", &ScenePresentDetour, &g_origScenePresent);
    }
}
