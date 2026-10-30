// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Event seams: what the runtime does at the scheduler's tick, on both builds.

#include "wxl/seams/seams.h"

#include "runtime/Core.hpp"
#include "runtime/Stages.hpp"
#include "services/Hashes.hpp"
#include "services/RelayService.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <cstdint>

namespace wxl::seam::Event::DispatchUpdate
{
    void WXL_CALL Before(void*, uint32_t, float elapsed_sec)
    {
        // The scheduler is dispatching, so the client is running its own loop: the first
        // tick is the last moment that still counts as before the first frame.
        wxl::core::stages::Reach(WXL_STAGE_READY);

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        // What other threads and other mods left for the main thread: hashes done, relayed
        // requests, answers and cancellations.
        wxl::core::services::hashes::Pump();
        wxl::core::services::relay::Pump();

        wxl_update_args args{};
        args.delta = elapsed_sec;
        host->Raise(WXL_EV_Update, &args);
    }
}
