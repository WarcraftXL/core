// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The AddOns seam: the client moving loose interface folders aside, which a mod may refuse.

#include "wxl/seams/seams.h"

#include "common/Log.hpp"
#include "runtime/Core.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

namespace wxl::seam::AddOns::DeleteInterfaceFiles
{
    bool WXL_CALL Gate()
    {
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !host->Listening(WXL_EV_InterfaceCleanupRequest)) return false;

        wxl_interface_cleanup_args args{};
        wxl_bool refused = WXL_FALSE;
        args.refused     = &refused;
        host->Raise(WXL_EV_InterfaceCleanupRequest, &args);
        if (refused == WXL_FALSE) return false;
        WLOG_DEBUG("interface", "loose interface folders left in place at a mod's answer");
        return true;
    }
}
