// SPDX-License-Identifier: GPL-3.0-or-later
// The frame seams on the D3D9 device (EndScene, Present, Reset) and on GxScenePresent without one.

#pragma once

struct IDirect3DDevice9;

namespace wxl::client::frame
{
    /// Swaps EndScene, Present and Reset in the device's vtable, once per device: a recreated device
    /// comes with a fresh vtable. A vtable that already carries them is left alone.
    void EnsureDeviceHooks(IDirect3DDevice9* device);

    /// The device hooks on the live device (deferred to the first world frame without one) and the
    /// GxScenePresent seam.
    void Install();
}
