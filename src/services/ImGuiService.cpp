// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.imgui: one Dear ImGui context and one ImPlot context, made with this runtime's allocator
// the first time anyone asks for them. While a mod listens to ImGuiFrame, every frame of the game
// that Scrn.OnPaint opened gets a frame of ImGui at its present (the device still in its scene):
// the Win32 and Direct3D 9 backends start it, the mods add their windows from the event, and the
// draw data is drawn with the game's own device, its state saved and restored by the backend and
// the game told to send its own again. The window's messages reach ImGui first, and those it wants
// stop there. The device objects go at every release of the device's resources and come back at
// ImGui's next frame; a device made anew is taken up in place of the old one.

#include "services/ImGuiService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Core.hpp"
#include "services/InputService.hpp"

#include "wxl/host/mod_host.hpp"

#include <windows.h>
#include <d3d9.h>

#include <imgui.h>
#include <implot.h>

#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

#include <cstdlib>
#include <cstring>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    using namespace wxl::core;

    constexpr const char* kVersion = IMGUI_VERSION ";" IMPLOT_VERSION;

    ImGuiContext*      g_context = nullptr;
    ImPlotContext*     g_plot    = nullptr;
    IDirect3DDevice9*  g_device  = nullptr; ///< the device the DX9 backend was set up with
    HWND               g_window  = nullptr; ///< the window the Win32 backend was set up with
    bool               g_running = false;   ///< a frame was drawn lately: input goes to ImGui
    float              g_scale   = 0.0f;    ///< the scale the style was sized for
    ImGuiStyle         g_style;             ///< the style at scale 1

    void* Alloc(size_t size, void*) { return std::malloc(size); }
    void  Free(void* block, void*) { std::free(block); }

    /// The contexts, made on first use, with this runtime's allocator.
    void Ensure()
    {
        if (g_context) return;
        ImGui::SetAllocatorFunctions(&Alloc, &Free, nullptr);
        g_context = ImGui::CreateContext();
        g_plot    = ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename                     = nullptr; // nothing written into the game's folder
        io.LogFilename                     = nullptr;
        io.ConfigFlags                    |= ImGuiConfigFlags_NoMouseCursorChange; // the game's pointer stays
        io.ConfigErrorRecoveryEnableAssert = false;  // a mod's unbalanced Begin/End is mended, not fatal
        ImGui::StyleColorsDark();
        g_style = ImGui::GetStyle();
    }

    /// Makes this runtime's copy current: a mod's handler may have left its own pointing elsewhere.
    void MakeCurrent()
    {
        ImGui::SetAllocatorFunctions(&Alloc, &Free, nullptr);
        ImGui::SetCurrentContext(g_context);
        ImPlot::SetCurrentContext(g_plot);
    }

    /// Sizes text and spacing for the window: the monitor's DPI scale, or a quarter step per
    /// 1080 lines of height, whichever is larger, from 1 to 3.
    void Scale(HWND window)
    {
        RECT client{};
        GetClientRect(window, &client);
        float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(window);
        const float byHeight = static_cast<float>(static_cast<int>((client.bottom - client.top) / 1080.0f * 4.0f)) / 4.0f;
        if (byHeight > scale) scale = byHeight;
        scale = scale < 1.0f ? 1.0f : (scale > 3.0f ? 3.0f : scale);
        if (scale == g_scale) return;
        g_scale = scale;
        ImGuiStyle& style = ImGui::GetStyle();
        style = g_style;
        style.ScaleAllSizes(scale);
        ImGui::GetIO().FontGlobalScale = scale;
    }

    wxl_str WXL_CALL Version() { return wxl_str{kVersion, static_cast<uint32_t>(std::strlen(kVersion))}; }

    void* WXL_CALL Context()
    {
        Ensure();
        return g_context;
    }

    void* WXL_CALL PlotContext()
    {
        Ensure();
        return g_plot;
    }

    void WXL_CALL Allocators(void** alloc, void** release, void** user)
    {
        if (alloc) *alloc = reinterpret_cast<void*>(&Alloc);
        if (release) *release = reinterpret_cast<void*>(&Free);
        if (user) *user = nullptr;
    }

    bool IsMouse(uint32_t message)
    {
        return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_MOUSEHWHEEL;
    }

    bool IsKeyboard(uint32_t message)
    {
        return (message >= WM_KEYFIRST && message <= WM_KEYLAST);
    }
}

namespace wxl::core::services
{
    const wxl_imgui_api& ImGuiApi()
    {
        static const wxl_imgui_api api = [] {
            wxl_imgui_api table{};
            table.struct_size  = sizeof table;
            table.version      = &Version;
            table.context      = &Context;
            table.plot_context = &PlotContext;
            table.allocators   = &Allocators;
            return table;
        }();
        return api;
    }

    namespace imgui
    {
        void Present(const wxl_frame_args& args)
        {
            wxl::host::ModHost* host = Host();
            if (!host || !host->Listening(WXL_EV_ImGuiFrame))
            {
                g_running = false;
                return;
            }
            auto* device = static_cast<IDirect3DDevice9*>(binding::gx_d3d9_device());
            HWND  window = static_cast<HWND>(input::Window());
            if (!device || !window || binding::gx_has_context() != WXL_TRUE) return;

            Ensure();
            MakeCurrent();
            if (window != g_window)
            {
                if (g_window) ImGui_ImplWin32_Shutdown();
                g_window = ImGui_ImplWin32_Init(window) ? window : nullptr;
            }
            if (device != g_device)
            {
                if (g_device) ImGui_ImplDX9_Shutdown();
                g_device = ImGui_ImplDX9_Init(device) ? device : nullptr;
            }
            if (!g_window || !g_device) return;

            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            Scale(window);
            ImGui::NewFrame();
            host->Raise(WXL_EV_ImGuiFrame, &args);
            MakeCurrent();
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            binding::gx_invalidate_state();
            g_running = true;
        }

        bool WindowMessage(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam)
        {
            if (!g_running || !g_context || !g_window) return false;
            MakeCurrent();
            ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(window), message, static_cast<WPARAM>(wparam),
                                           static_cast<LPARAM>(lparam));
            const ImGuiIO& io = ImGui::GetIO();
            return (io.WantCaptureMouse && IsMouse(message)) || (io.WantCaptureKeyboard && (IsKeyboard(message)));
        }

        void DeviceLost()
        {
            if (!g_device || !g_context) return;
            MakeCurrent();
            ImGui_ImplDX9_InvalidateDeviceObjects();
        }

        void Shutdown()
        {
            if (!g_context) return;
            MakeCurrent();
            if (g_device) ImGui_ImplDX9_Shutdown();
            if (g_window) ImGui_ImplWin32_Shutdown();
            g_device  = nullptr;
            g_window  = nullptr;
            g_running = false;
        }
    }
}
