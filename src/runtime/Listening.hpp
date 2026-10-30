// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The byte per event the x64 client tests before a busy seam (wxl_core_listening, wxl/core/init.h):
// set while a mod handles the event, or while the runtime's own work rides on the seam that raises
// it (wxl.highlight keeps the model batches of M2.DrawBatch while a highlight is set, whether or not
// a mod listens to ModelBatchDrawn).

#pragma once

#include "wxl/events.h"

namespace wxl::core::listening
{
    /// The host's word: @p event gained its first handler (true) or lost its last (false).
    void ModsListen(wxl_event event, bool listening);

    /// The runtime needs the seam that raises @p event called (true), or no longer (false).
    void RuntimeWants(wxl_event event, bool wanted);

    /// Whether the runtime itself holds the flag of @p event.
    bool RuntimeWanted(wxl_event event);
}
