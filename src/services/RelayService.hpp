// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.relay service: services mods publish for each other, their answers delivered later on
// the main thread, the same on both builds.

#pragma once

#include "wxl/services/relay.h"

#include <cstdint>

namespace wxl::core::services
{
    /// The table, WXL_RELAY_SERVICE version WXL_RELAY_SERVICE_VERSION.
    const wxl_relay_api& RelayApi();

    namespace relay
    {
        /// Delivers what is waiting: requests to their services, answers to their callers,
        /// cancellations. The main thread, once a tick.
        void Pump();

        /// A mod goes: its services are taken back (their requests fail for their callers), its
        /// requests cancelled (their services are told). Nothing of its code is called again.
        void ForgetMod(uint32_t mod);

        /// Drops everything without calling anyone: the way out, and the harness.
        void Clear();
    }
}
