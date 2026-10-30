// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.world over the binding: World::Intersect on the main thread, where the map it walks is
// loaded and changed.

#include "services/WorldService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/mod_host.hpp"

#include <atomic>
#include <cmath>

namespace
{
    using namespace wxl::core;

    bool Finite(const wxl_vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

    wxl_bool WXL_CALL Trace(wxl_vec3 start, wxl_vec3 finish, uint32_t flags, wxl_vec3* hit)
    {
        static std::atomic<bool> said{false};
        const wxl::host::ModHost* host = Host();
        if (host && !host->OnMainThread())
        {
            if (!said.exchange(true))
                WLOG_WARN(kTag, "wxl.world.Trace was called off the main thread, where the world is loaded and "
                                "changed; it answered that nothing was met");
            return WXL_FALSE;
        }
        if (!hit || !Finite(start) || !Finite(finish)) return WXL_FALSE;
        return binding::world_trace(start, finish, flags ? flags : WXL_TRACE_CAMERA, hit);
    }
}

namespace wxl::core::services
{
    const wxl_world_api& WorldApi()
    {
        static const wxl_world_api api = [] {
            wxl_world_api table{};
            table.struct_size = sizeof table;
            table.trace       = &Trace;
            return table;
        }();
        return api;
    }
}
