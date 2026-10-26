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
}
