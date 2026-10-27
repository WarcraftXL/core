// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The graphics memory report: one line in the log, the same on every device, so that a launch on
// Direct3D 9 and one on Vulkan read side by side. It says how the process's address space is used
// (the stock client has 4 GB of it at most, with its large-address-aware flag set by the patcher)
// and, under Vulkan, what the renderer holds of it and of the GPU's memory.

#pragma once

namespace wxl::core::gx::memory
{
    /// Logs the report now; @p when says at which moment ("the device made", "at exit").
    void Report(const char* when);

    /// Each present: the report a minute after the first frame, then every five minutes.
    void Present();
}
