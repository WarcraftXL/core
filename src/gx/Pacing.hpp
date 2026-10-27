// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// How the frames came, on every device, so that a launch on Direct3D 9 and one on Vulkan read side
// by side: the time between two frames the client painted (the loading screen's presents left out,
// a pause of a second or more too), its percentiles, and the hitches (a frame at least twice the
// median of the frames before it and 8 ms over it). Under Vulkan a hitch is put down to what the
// renderer's main thread spent most of it on (pipelines compiled, shaders translated, uploads,
// memory made, waiting for the GPU, a readback, the present), or to the client's own work when the
// renderer took less than half of the excess; and the lines add the bytes uploaded per frame, the
// main thread's time per draw and the GPU's per pass.
//
// The lines come at exit for the whole session, and at each set of the console variable gxStats
// (`/console gxStats Dalaran` in the chat box) for the segment since the previous one, under the
// label it was given: a test route's steps, each with its numbers.

#pragma once

namespace wxl::core::gx::pacing
{
    /// Each present; @p painted when the frame was the client's paint (not the loading screen's).
    void Present(bool painted);

    /// The segment's lines under @p label, then a new segment.
    void Mark(const char* label);

    /// The session's lines; @p when says at which moment ("of the session").
    void Report(const char* when);

    /// The console variable gxStats, once the client's variables exist.
    void RegisterVariable();
}
