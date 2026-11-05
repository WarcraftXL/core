// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/input/Input.hpp"

#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "wxl/game/Pick.hpp"

namespace
{
    namespace ev    = wxl::events;
    namespace world = wxl::game::world;

    HWND    g_hwnd        = nullptr;
    WNDPROC g_origWndProc = nullptr;

    /// The process's visible top-level window.
    BOOL CALLBACK PickWindow(HWND window, LPARAM out)
    {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process != GetCurrentProcessId() || GetWindow(window, GW_OWNER) || !IsWindowVisible(window)) return TRUE;
        *reinterpret_cast<HWND*>(out) = window;
        return FALSE;
    }

    HWND FindGameWindow()
    {
        HWND window = nullptr;
        EnumWindows(&PickWindow, reinterpret_cast<LPARAM>(&window));
        return window ? window : FindWindowA("GxWindowClass", nullptr);
    }

    /// A click no handler took: the world point or object under the cursor.
    void RaiseWorldClick(UINT message)
    {
        world::WorldHit hit;
        if (!world::PickCursor(hit)) return;
        const ev::WorldClickArgs args{ message, hit.type, hit.pos.x, hit.pos.y, hit.pos.z, hit.objLo, hit.objHi };
        ev::Emit<ev::Event::OnWorldClick>(args);
    }

    /// Every message as OnInput; one a handler marked handled goes no further.
    LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        bool                 handled = false;
        const ev::InputArgs  args{ message, static_cast<uintptr_t>(wParam), static_cast<uintptr_t>(lParam), &handled };
        ev::Emit<ev::Event::OnInput>(args);
        if (handled) return 0;
        if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) RaiseWorldClick(message);
        return CallWindowProcA(g_origWndProc, window, message, wParam, lParam);
    }

    /// Without a window OnInput stays silent; that is not a failed install.
    bool InstallInput()
    {
        if (g_origWndProc) return true;
        g_hwnd = FindGameWindow();
        if (!g_hwnd)
        {
            WLOG_WARN("input: game window not found, OnInput inactive");
            return true;
        }
        g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
        if (!g_origWndProc)
        {
            WLOG_WARN("input: SetWindowLongPtr failed (%lu)", GetLastError());
            return true;
        }
        WLOG_INFO("input: window subclassed (hwnd=%p), OnInput live", g_hwnd);
        return true;
    }
}

namespace wxl::input
{
    void FollowWindow(HWND hwnd)
    {
        if (!hwnd || hwnd == g_hwnd || !g_origWndProc) return;
        const auto previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
        if (!previous)
        {
            WLOG_WARN("input: SetWindowLongPtr on the new window %p failed (%lu), OnInput stays on %p", hwnd,
                      GetLastError(), g_hwnd);
            return;
        }
        if (previous != &WndProc) g_origWndProc = previous;
        WLOG_INFO("input: window subclass follows the device window %p -> %p", g_hwnd, hwnd);
        g_hwnd = hwnd;
    }
}

WXL_REGISTER_FEATURE("input", true, InstallInput)
