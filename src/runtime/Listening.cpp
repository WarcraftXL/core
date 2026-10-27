// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The client's flag per event, from the mods' handlers and the runtime's own needs (see
// Listening.hpp).

#include "runtime/Listening.hpp"

#include "wxl/core/init.h"

#include <atomic>
#include <cstdint>

namespace
{
    std::atomic<uint8_t> g_mods[WXL_EV_COUNT]    = {};
    std::atomic<uint8_t> g_runtime[WXL_EV_COUNT] = {};

    bool Valid(wxl_event event) { return static_cast<uint32_t>(event) < static_cast<uint32_t>(WXL_EV_COUNT); }

    void Publish(wxl_event event)
    {
        wxl_core_listening[event] = (g_mods[event].load() || g_runtime[event].load()) ? 1 : 0;
    }
}

namespace wxl::core::listening
{
    void ModsListen(wxl_event event, bool listening)
    {
        if (!Valid(event)) return;
        g_mods[event].store(listening ? 1 : 0);
        Publish(event);
    }

    void RuntimeWants(wxl_event event, bool wanted)
    {
        if (!Valid(event)) return;
        if (g_runtime[event].exchange(wanted ? 1 : 0) == (wanted ? 1 : 0)) return;
        Publish(event);
    }

    bool RuntimeWanted(wxl_event event) { return Valid(event) && g_runtime[event].load() != 0; }
}
