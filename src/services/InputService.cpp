// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.input. A captured pointer is clipped to one pixel of the game's window, its centre or the
// point the last mod to capture or move it chose, so the client reads it there (OsInputGetMousePosition asks Windows), picks under it every frame
// and lands its clicks there; the device stops drawing it (CGxDevice::CursorSetVisible) while the
// game's own state of the pointer is left as it is, which keeps its picking on. The motion comes
// from Windows' raw input, registered for the window the first time a mod captures the pointer or
// listens to MouseMotion. Everything here runs on the main thread: the window procedure and the
// present both do.

#include "services/InputService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Core.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <vector>

namespace
{
    using namespace wxl::core;

    /// A mod holding the pointer, and the point of the window it holds it at.
    struct Hold
    {
        uint32_t mod;
        double   x; ///< from the left edge of the client area, 0 to 1
        double   y; ///< from its top edge, 0 to 1
    };

    /// The mods holding the pointer, one entry each, the last to capture or move it last: the
    /// pointer is held at its point.
    std::vector<Hold> g_holders;

    std::vector<Hold>::iterator HoldOf(uint32_t mod)
    {
        return std::find_if(g_holders.begin(), g_holders.end(), [mod](const Hold& hold) { return hold.mod == mod; });
    }

    HWND g_window     = nullptr; ///< the game's window, from its first message
    bool g_registered = false;   ///< raw mouse input is registered for it
    bool g_held       = false;   ///< the pointer is clipped and hidden now

    /// The last absolute position a raw report gave, for a device that reports positions (a
    /// tablet, a remote desktop): its motion is the difference.
    bool g_absoluteKnown = false;
    LONG g_absoluteX     = 0;
    LONG g_absoluteY     = 0;

    uint32_t Caller()
    {
        const wxl::host::ModHost* host = Host();
        return host ? host->CurrentMod() : 0u;
    }

    bool OnGameThread(const char* call)
    {
        static std::atomic<bool> said{false};
        const wxl::host::ModHost* host = Host();
        if (!host || host->OnMainThread()) return true;
        if (!said.exchange(true))
            WLOG_WARN(kTag, "wxl.input.%s was called off the main thread, where the game's window lives; it was "
                            "refused",
                      call);
        return false;
    }

    bool Listening(wxl_event event)
    {
        const wxl::host::ModHost* host = Host();
        return host && host->Listening(event);
    }

    /// Raw mouse reports for the game's window, once: the first capture or MouseMotion listener.
    void Register()
    {
        if (g_registered || !g_window) return;
        RAWINPUTDEVICE device{};
        device.usUsagePage = 0x01; // generic desktop
        device.usUsage     = 0x02; // mouse
        device.dwFlags     = 0;    // the legacy messages keep coming: the game reads them
        device.hwndTarget  = g_window;
        g_registered       = RegisterRawInputDevices(&device, 1, sizeof device) == TRUE;
        if (!g_registered)
            WLOG_WARN(kTag, "wxl.input: raw mouse input could not be registered (error %lu); MouseMotion will not "
                            "be raised",
                      static_cast<unsigned long>(GetLastError()));
    }

    /// The motion in a raw report, or false for a report of something else.
    bool Motion(LPARAM handle, int32_t& dx, int32_t& dy)
    {
        RAWINPUT raw{};
        UINT     size = sizeof raw;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(handle), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) ==
                static_cast<UINT>(-1) ||
            raw.header.dwType != RIM_TYPEMOUSE)
            return false;
        const RAWMOUSE& mouse = raw.data.mouse;
        if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE)
        {
            // 0 to 65535 across the screen, or the whole desktop: the difference, in pixels.
            const bool virtualDesk = (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
            const int  width  = GetSystemMetrics(virtualDesk ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
            const int  height = GetSystemMetrics(virtualDesk ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
            const LONG x      = static_cast<LONG>((static_cast<long long>(mouse.lLastX) * width) / 65535);
            const LONG y      = static_cast<LONG>((static_cast<long long>(mouse.lLastY) * height) / 65535);
            dx                = g_absoluteKnown ? static_cast<int32_t>(x - g_absoluteX) : 0;
            dy                = g_absoluteKnown ? static_cast<int32_t>(y - g_absoluteY) : 0;
            g_absoluteX       = x;
            g_absoluteY       = y;
            g_absoluteKnown   = true;
            return true;
        }
        dx = static_cast<int32_t>(mouse.lLastX);
        dy = static_cast<int32_t>(mouse.lLastY);
        return true;
    }

    /// The pixel of the window's client area the pointer is held on, in screen coordinates.
    bool HeldPixel(RECT& pixel)
    {
        RECT client{};
        if (g_holders.empty() || !GetClientRect(g_window, &client) || client.right <= 0 || client.bottom <= 0)
            return false;
        const Hold& hold = g_holders.back();
        POINT       point{};
        wxl::core::services::input::PixelAt(client.right, client.bottom, hold.x, hold.y, point.x, point.y);
        if (!ClientToScreen(g_window, &point)) return false;
        pixel = RECT{point.x, point.y, point.x + 1, point.y + 1};
        return true;
    }

    /// Lets the pointer go: free to move, and drawn as the game's state says.
    void LetGo()
    {
        if (!g_held) return;
        ClipCursor(nullptr);
        binding::input_cursor_restore();
        g_held = false;
    }

    // --- the table --------------------------------------------------------------------------

    /// Captures the pointer for the calling mod at a point, or moves its hold there.
    wxl_result HoldAt(double x, double y)
    {
        const uint32_t mod  = Caller();
        const auto     held = HoldOf(mod);
        if (held != g_holders.end()) g_holders.erase(held);
        g_holders.push_back(Hold{mod, x, y});
        Register();
        return WXL_OK;
    }

    wxl_result WXL_CALL CaptureCursor(wxl_bool captured)
    {
        if (!OnGameThread("CaptureCursor")) return WXL_E_INVALID;
        if (captured == WXL_TRUE) return HoldAt(0.5, 0.5);
        const auto held = HoldOf(Caller());
        if (held != g_holders.end())
        {
            g_holders.erase(held);
            if (g_holders.empty()) LetGo();
        }
        return WXL_OK;
    }

    wxl_result WXL_CALL CaptureCursorAt(double x, double y)
    {
        if (!OnGameThread("CaptureCursorAt")) return WXL_E_INVALID;
        if (!(x >= 0.0 && x <= 1.0 && y >= 0.0 && y <= 1.0)) return WXL_E_INVALID;
        return HoldAt(x, y);
    }

    wxl_result WXL_CALL CapturePoint(wxl_vec3* out)
    {
        if (!out) return WXL_E_INVALID;
        if (!OnGameThread("CapturePoint")) return WXL_E_INVALID;
        if (g_holders.empty()) return WXL_E_NOT_FOUND;
        *out = wxl_vec3{static_cast<float>(g_holders.back().x), static_cast<float>(g_holders.back().y), 0.0f};
        return WXL_OK;
    }

    wxl_bool WXL_CALL CursorCaptured() { return g_holders.empty() ? WXL_FALSE : WXL_TRUE; }

    wxl_bool WXL_CALL PointerOverWorld()
    {
        if (!OnGameThread("PointerOverWorld")) return WXL_FALSE;
        return binding::input_pointer_over_world();
    }

    // --- the game's mouse look, kept out --------------------------------------------------------

    /// The mods keeping the game out of its mouse look, one entry each.
    std::vector<uint32_t> g_blockers;

    // The game's look bits: the right button's INPUT_TURN_PLAYER, the left one's INPUT_TURN_CAMERA.
    constexpr uint32_t kTurnPlayer = 0x1;
    constexpr uint32_t kTurnCamera = 0x2;
    constexpr uint32_t kLookBits   = kTurnPlayer | kTurnCamera;

    // CGInputControl::IsMouseDrag: a press held 0.8 s or longer is no click.
    constexpr uint32_t kClickMs = 800;

    // CGInputControl::m_pendingDefaultAction, and the buttons PerformDefaultAction takes.
    constexpr uint32_t kReleaseSelect = 1;
    constexpr uint32_t kButtonLeft    = 1;
    constexpr uint32_t kButtonRight   = 4;

    /// The look bits the gate kept from the game, which the buttons still hold, and when each was
    /// pressed (index 0 the right button, 1 the left).
    uint32_t g_down        = 0;
    uint32_t g_downTime[2] = {};

    wxl_result WXL_CALL BlockMouseLook(wxl_bool blocked)
    {
        if (!OnGameThread("BlockMouseLook")) return WXL_E_INVALID;
        const uint32_t mod  = Caller();
        const auto     held = std::find(g_blockers.begin(), g_blockers.end(), mod);
        if (blocked == WXL_TRUE)
        {
            if (held != g_blockers.end()) return WXL_OK;
            // A look the game is in already ends, with no click.
            if (g_blockers.empty()) binding::input_mouse_look_stop();
            g_blockers.push_back(mod);
        }
        else if (held != g_blockers.end())
            g_blockers.erase(held);
        return WXL_OK;
    }

    wxl_bool WXL_CALL MouseLookBlocked() { return g_blockers.empty() ? WXL_FALSE : WXL_TRUE; }
}

namespace wxl::core::services
{
    const wxl_input_api& InputApi()
    {
        static const wxl_input_api api = [] {
            wxl_input_api table{};
            table.struct_size        = sizeof table;
            table.capture_cursor     = &CaptureCursor;
            table.cursor_captured    = &CursorCaptured;
            table.pointer_over_world = &PointerOverWorld;
            table.block_mouse_look   = &BlockMouseLook;
            table.mouse_look_blocked = &MouseLookBlocked;
            table.capture_cursor_at  = &CaptureCursorAt;
            table.capture_point      = &CapturePoint;
            return table;
        }();
        return api;
    }

    namespace input
    {
        void NoteWindow(void* window)
        {
            if (!g_window && window) g_window = static_cast<HWND>(window);
        }

        bool RawMotion(intptr_t report, int32_t& dx, int32_t& dy)
        {
            return Motion(static_cast<LPARAM>(report), dx, dy);
        }

        void Present()
        {
            if (!g_window) return;
            if (!g_registered && Listening(WXL_EV_MouseMotion)) Register();

            const bool want = !g_holders.empty() && GetForegroundWindow() == g_window && !IsIconic(g_window);
            if (!want)
            {
                LetGo();
                return;
            }

            RECT pixel{};
            if (!HeldPixel(pixel))
            {
                LetGo();
                return;
            }
            RECT clip{};
            if (!g_held || !GetClipCursor(&clip) || !EqualRect(&clip, &pixel))
            {
                SetCursorPos(pixel.left, pixel.top);
                ClipCursor(&pixel);
            }
            // The client shows the pointer again whenever its own state of it changes (a spell's
            // targeting, the focus coming back): hidden again before this frame draws it.
            if (binding::input_cursor_visible()) binding::input_cursor_set_visible(WXL_FALSE);
            g_held = true;
        }

        bool SetControlBit(int32_t* result, void* control, uint32_t* bit, uint32_t time)
        {
            if (g_blockers.empty() || !(*bit & kLookBits)) return false;
            const uint32_t look = *bit & kLookBits;
            if (look & kTurnPlayer) g_downTime[0] = time;
            if (look & kTurnCamera) g_downTime[1] = time;
            g_down |= look;
            // Both buttons held: the game makes no click of either, nor will this.
            if ((g_down & kLookBits) == kLookBits) binding::input_clear_pending_action(control);
            const uint32_t rest = *bit & ~kLookBits;
            if (rest)
            {
                *bit = rest;
                return false;
            }
            *result = 0; // the bit is not set: the caller does not update the player for it
            return true;
        }

        bool UnsetControlBit(int32_t* result, void* control, uint32_t* bit, uint32_t time)
        {
            const uint32_t mine = *bit & g_down;
            if (!mine) return false;
            g_down &= ~mine;
            if (!g_down)
            {
                // The last button the gate held is up: a click if it was short, as the game's own.
                const uint32_t pressed  = (mine & kTurnCamera) ? g_downTime[1] : g_downTime[0];
                const uint32_t pending  = binding::input_pending_action(control);
                binding::input_clear_pending_action(control);
                if (pending && time - pressed < kClickMs)
                    binding::input_default_action(pending == kReleaseSelect ? kButtonLeft : kButtonRight);
            }
            const uint32_t rest = *bit & ~mine;
            if (rest)
            {
                *bit = rest;
                return false;
            }
            *result = 0;
            return true;
        }

        void ForgetMod(uint32_t mod)
        {
            const auto blocked = std::find(g_blockers.begin(), g_blockers.end(), mod);
            if (blocked != g_blockers.end()) g_blockers.erase(blocked);
            const auto held = HoldOf(mod);
            if (held == g_holders.end()) return;
            g_holders.erase(held);
            if (g_holders.empty()) LetGo();
        }

        void Clear()
        {
            g_holders.clear();
            g_blockers.clear();
            LetGo();
        }

        void PixelAt(long width, long height, double x, double y, long& px, long& py)
        {
            // floor(x * width), on the window's last pixel at 1: the centre is width / 2.
            px = std::clamp(static_cast<long>(x * static_cast<double>(width)), 0L, width > 0 ? width - 1 : 0L);
            py = std::clamp(static_cast<long>(y * static_cast<double>(height)), 0L, height > 0 ? height - 1 : 0L);
        }

        void* Window() { return g_window; }
    }
}
