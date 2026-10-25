// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.imgui service: the runtime's Dear ImGui and ImPlot contexts, a frame of them drawn
// over the game's frame while a mod listens to ImGuiFrame, the window's input handed to them.

#pragma once

#include <cstdint>

#include "wxl/events.h"
#include "wxl/services/imgui.h"

namespace wxl::core::services
{
    /// The table, WXL_IMGUI_SERVICE version WXL_IMGUI_SERVICE_VERSION.
    const wxl_imgui_api& ImGuiApi();

    namespace imgui
    {
        /// The game is about to show a frame: while a mod listens to ImGuiFrame and the device can
        /// draw, a frame of ImGui is opened (true), for the caller to raise the event in. Main
        /// thread, in the device's scene.
        bool BeginFrame();

        /// Draws what the mods added to the frame BeginFrame opened, with the game's device.
        void EndFrame();

        /// A message reached the game's window procedure: ImGui is told, and true when it wants
        /// it for itself (a window under the pointer, a text field typed in): the game must not
        /// see it then.
        bool WindowMessage(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam);

        /// The device's resources are about to go (a reset, the device destroyed): ImGui's go too,
        /// and are made again at its next frame.
        void DeviceLost();

        /// The client is shutting down: the backends let the device go.
        void Shutdown();
    }
}
