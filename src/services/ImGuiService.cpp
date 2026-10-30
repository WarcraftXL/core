// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.imgui: one Dear ImGui context and one ImPlot context, made with this runtime's allocator
// the first time anyone asks for them. While a mod listens to ImGuiFrame, every frame of the game
// that Scrn.OnPaint opened gets a frame of ImGui at its present (the device still in its scene):
// the Win32 backend and the device's renderer start it, the mods add their windows from the event,
// and the draw data is drawn with the game's own device. The window's messages reach ImGui first,
// and those it wants stop there.
//
// Two renderers, by the device the client runs on:
//   - Direct3D 9: Dear ImGui's DX9 backend on the game's device, its state saved and restored by
//     the backend and the game told to send its own again. Its objects go at every release of the
//     device's resources and come back at ImGui's next frame; a device made anew is taken up in
//     place of the old one.
//   - Vulkan (the runtime's own renderer, gx/Vulkan.cpp): Dear ImGui's Vulkan backend on the
//     runtime's device, drawn into the frame's scene before the present pass, so it shows under
//     the same gamma as the game. Its objects go when the device is lost or released and are made
//     again on the next device.

#include "services/ImGuiService.hpp"

#include <windows.h>
#include <d3d9.h>

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "gx/Vulkan.hpp"
#include "gx/vk/imgui/ImGuiVk.hpp"
#include "gx/vk/present/Display.hpp"
#include "runtime/Core.hpp"
#include "services/InputService.hpp"

#include "wxl/host/mod_host.hpp"

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

    wxl::vk::imgui::Renderer* g_vk           = nullptr; ///< the Vulkan backend, on the runtime's Vulkan device
    uint64_t                  g_vkGeneration = 0;       ///< the device it was made on
    bool                      g_vkFrame      = false;   ///< BeginFrame opened a frame drawn through it
    bool                      g_vkRefused    = false;   ///< it could not be made on this device: said once

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

    bool IsKeyRelease(uint32_t message) { return message == WM_KEYUP || message == WM_SYSKEYUP; }
}

namespace
{
    void ReleaseVulkan()
    {
        if (!g_vk) return;
        MakeCurrent();
        g_vk->Destroy();
        delete g_vk;
        g_vk = nullptr;
    }

    /// The Vulkan half of BeginFrame: the backend on the device of this frame, the font atlas
    /// uploaded in the frame, before ImGui's own frame begins.
    bool BeginVulkan(HWND window)
    {
        wxl::core::gx::VulkanFrame frame;
        if (!wxl::core::gx::VulkanOpenFrame(frame)) return false;
        Ensure();
        MakeCurrent();
        if (window != g_window)
        {
            if (g_window) ImGui_ImplWin32_Shutdown();
            g_window = ImGui_ImplWin32_Init(window) ? window : nullptr;
        }
        if (!g_window) return false;
        if (g_vk && g_vkGeneration != frame.generation) ReleaseVulkan(); // its device is gone: what it made went with it
        // Its pipeline is made for the scene's samples (gxMultisample): a new count, a new backend.
        if (g_vk && g_vk->Samples() != frame.display->Samples())
        {
            // The frames before may still draw with it.
            if (!frame.display->WaitSubmitted()) return false;
            ReleaseVulkan();
            g_vkRefused = false;
        }
        if (!g_vk)
        {
            if (g_vkRefused && g_vkGeneration == frame.generation) return false;
            g_vk           = new wxl::vk::imgui::Renderer();
            g_vkGeneration = frame.generation;
            char why[256]  = {};
            if (!g_vk->Create(*frame.context, *frame.allocator, frame.display->SceneFormat(), frame.display->Samples(), frame.frames, why,
                              sizeof why))
            {
                WLOG_WARN("core", "wxl.imgui: Dear ImGui's Vulkan backend was not made (%s); the mods' windows are not drawn", why);
                delete g_vk;
                g_vk        = nullptr;
                g_vkRefused = true;
                return false;
            }
            g_vkRefused = false;
        }
        wxl::vk::present::Display* display = frame.display;
        display->EndRendering();
        g_vk->NewFrame(display->Cmd(), [display](std::function<void()> release) { display->AfterFrame(std::move(release)); });

        ImGui_ImplWin32_NewFrame();
        Scale(window);
        ImGui::NewFrame();
        g_vkFrame = true;
        return true;
    }

    void EndVulkan()
    {
        g_vkFrame = false;
        wxl::core::gx::VulkanFrame frame;
        if (!g_vk || !wxl::core::gx::VulkanOpenFrame(frame) || frame.generation != g_vkGeneration ||
            g_vk->Samples() != frame.display->Samples())
            return;
        const VkCommandBuffer cmd = frame.display->BeginColorRendering();
        if (!cmd) return;
        g_vk->Record(cmd, ImGui::GetDrawData());
        frame.display->EndRendering();
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
        bool BeginFrame()
        {
            wxl::host::ModHost* host = Host();
            if (!host || !host->Listening(WXL_EV_ImGuiFrame))
            {
                g_running = false;
                return false;
            }
            HWND window = static_cast<HWND>(input::Window());
            if (gx::VulkanActive()) return window && binding::gx_has_context() == WXL_TRUE && BeginVulkan(window);

            auto* device = static_cast<IDirect3DDevice9*>(binding::gx_d3d9_device());
            if (!device || !window || binding::gx_has_context() != WXL_TRUE) return false;

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
            if (!g_window || !g_device) return false;

            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            Scale(window);
            ImGui::NewFrame();
            return true;
        }

        void EndFrame()
        {
            MakeCurrent(); // a mod's handler may have left its own copy current
            ImGui::Render();
            if (g_vkFrame)
            {
                EndVulkan();
                g_running = true;
                return;
            }
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
            // A key let go goes on to the game as well: its press may have reached the game before
            // Dear ImGui wanted the keyboard, and a key the game never sees let go stays held there
            // (the character walks on). The game drops a release whose press it did not see.
            if (IsKeyRelease(message)) return false;
            const ImGuiIO& io = ImGui::GetIO();
            return (io.WantCaptureMouse && IsMouse(message)) || (io.WantCaptureKeyboard && (IsKeyboard(message)));
        }

        void DeviceLost()
        {
            if (!g_context) return;
            // The Vulkan backend's objects are on the device about to go: they go now, its frames done.
            ReleaseVulkan();
            if (!g_device) return;
            MakeCurrent();
            ImGui_ImplDX9_InvalidateDeviceObjects();
        }

        void Shutdown()
        {
            if (!g_context) return;
            ReleaseVulkan();
            MakeCurrent();
            if (g_device) ImGui_ImplDX9_Shutdown();
            if (g_window) ImGui_ImplWin32_Shutdown();
            g_device  = nullptr;
            g_window  = nullptr;
            g_running = false;
        }
    }
}
