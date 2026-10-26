// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// What the renderer's test files share: the checks and their count.

#pragma once

namespace wxl::vk::test
{
    /// Counts a check; prints the ones that fail.
    void Check(bool ok, const char* what);

    /// Whether an environment variable is set to something.
    bool Environment(const char* name);

    /// The pure cases of the present module and the frame's pieces: no device needed.
    void PresentPureCases();

    /// The frame capture's reader on captures made up in the client's format.
    void CaptureCases();

    /// The live cases on the machine's driver: the present pass, the frame loop, the swapchain on a
    /// headless surface, Dear ImGui drawn offscreen. False when no device could be made.
    bool PresentLiveCases();

    /// The engine's draws through the renderer: the tables and the engine reading (pure), then
    /// formats, blends, stages, programs, pools, uploads and a capture replayed on the driver.
    void DrawPureCases();
    bool DrawLiveCases();

    /// The translator of the client's DX9 programs: what it refuses, its cache and pack (pure); and
    /// programs assembled here drawn on the driver against D3D9's formulas (live).
    void TranslatePureCases();
    bool TranslateLiveCases();

    /// Every program the renderer makes held to spirv-val's Vulkan 1.3 rules; false when the
    /// build has no validator (SPIRV-Tools is built in the native test tree only).
    bool SpirvValidation();
}
