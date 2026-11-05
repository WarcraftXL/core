// SPDX-License-Identifier: GPL-3.0-or-later
// The input seam: the client window's messages as OnInput, and unhandled clicks as OnWorldClick.

#pragma once

#include <windows.h>

namespace wxl::input
{
    /// Moves the subclass to `hwnd` when the device made a new window (a backend without D3D9 does on a
    /// format change). Nothing for the window already subclassed, for null, or before the input feature.
    void FollowWindow(HWND hwnd);
}
