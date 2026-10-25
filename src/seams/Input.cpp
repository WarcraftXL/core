// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Input seam: what the runtime does at the client's window procedure.

#include "wxl/seams/seams.h"

#include "runtime/Core.hpp"
#include "services/ImGuiService.hpp"
#include "services/InputService.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <cstdint>

namespace
{
    constexpr uint32_t kRawInput   = 0x00FF; // WM_INPUT
    constexpr uint32_t kMouseWheel = 0x020A; // WM_MOUSEWHEEL
    constexpr float    kNotch      = 120.0f; // WHEEL_DELTA

    /// The mouse's events for a message: MouseMotion for a raw report, MouseWheel for a turn of
    /// the wheel. True when a MouseWheel handler kept the turn from the client. The window
    /// procedure passes WM_INPUT on to DefWindowProc, which frees the report.
    wxl_bool Mouse(wxl::host::ModHost& host, uint32_t message, uintptr_t wparam, intptr_t lparam)
    {
        if (message == kRawInput && host.Listening(WXL_EV_MouseMotion))
        {
            wxl_mouse_motion_args motion{};
            if (wxl::core::services::input::RawMotion(lparam, motion.dx, motion.dy) && (motion.dx || motion.dy))
                host.Raise(WXL_EV_MouseMotion, &motion);
            return WXL_FALSE;
        }
        if (message == kMouseWheel && host.Listening(WXL_EV_MouseWheel))
        {
            wxl_bool             kept = WXL_FALSE;
            wxl_mouse_wheel_args wheel{};
            wheel.delta   = static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(wparam >> 16))) / kNotch;
            wheel.handled = &kept;
            host.Raise(WXL_EV_MouseWheel, &wheel);
            return kept;
        }
        return WXL_FALSE;
    }
}

namespace wxl::seam::Input::WindowProc
{
    bool WXL_CALL Gate(intptr_t* result, void** window, uint32_t* message, uintptr_t* wparam,
                       intptr_t* lparam)
    {
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return false;

        wxl_bool        handled = WXL_FALSE;
        wxl_input_args  args{};
        args.message = *message;
        args.wparam  = static_cast<uint64_t>(*wparam);
        args.lparam  = static_cast<int64_t>(*lparam);
        args.handled = &handled;

        wxl::core::services::input::NoteWindow(*window);
        // Dear ImGui sees every message first: what it wants for a window of a mod's, the game
        // and the mods do not see.
        if (wxl::core::services::imgui::WindowMessage(*window, *message, *wparam, *lparam))
        {
            *result = 0;
            return true;
        }
        host->Raise(WXL_EV_Input, &args);
        // A message an Input handler left to the client goes on to the mouse's own events: the
        // raw motion, and the wheel, which a handler may keep from the client too.
        if (!handled) handled = Mouse(*host, *message, *wparam, *lparam);
        if (!handled) return false;

        // What a window procedure answers for a message it dealt with itself. The client's own
        // procedure never runs, so neither does its default handling of this message.
        *result = 0;
        return true;
    }
}
