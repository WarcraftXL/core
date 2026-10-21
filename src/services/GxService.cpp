// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The wxl.gx service: the client's D3D9 device and the model batches it draws.

#include "services/GxService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "objects/Objects.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/guard.hpp"
#include "wxl/host/mod_host.hpp"

#include <exception>
#include <string>
#include <vector>

namespace
{
    constexpr char kTag[] = "gx";

    namespace binding = wxl::core::binding;

    struct Watcher
    {
        wxl_gx_model_draw_fn fn;
        void*                user;
        uint32_t             owner;
        uint32_t             token;
        bool                 live;
    };

    std::vector<Watcher> g_watchers;
    uint32_t             g_live      = 0;
    uint32_t             g_nextToken = 1;
    int                  g_depth     = 0; // dispatches in progress: no compaction under them

    struct WatcherCall
    {
        const Watcher*           watcher;
        const wxl_gx_model_draw* draw;
    };

    void InvokeWatcher(void* context)
    {
        const WatcherCall* call = static_cast<const WatcherCall*>(context);
        try
        {
            call->watcher->fn(call->draw, call->watcher->user);
        }
        catch (const std::exception& e)
        {
            WLOG_ERROR(kTag, "a model draw watcher threw: %s", e.what());
        }
        catch (...)
        {
            WLOG_ERROR(kTag, "a model draw watcher threw");
        }
    }

    void Compact()
    {
        if (g_depth != 0) return;
        std::erase_if(g_watchers, [](const Watcher& watcher) { return !watcher.live; });
    }

    void Drop(Watcher& watcher)
    {
        if (!watcher.live) return;
        watcher.live = false;
        --g_live;
    }

    // --- The table ----------------------------------------------------------------------------

    void* WXL_CALL D3d9Device(void) { return binding::gx_d3d9_device(); }

    void* WXL_CALL ObjectModel(wxl_handle object)
    {
        if (object == WXL_HANDLE_NONE) return nullptr;
        void* native = wxl::core::objects::Service().Handles().Resolve(object);
        return native ? binding::gx_object_model(native) : nullptr;
    }

    wxl_result WXL_CALL WatchModelDraws(wxl_gx_model_draw_fn fn, void* user, uint32_t* out_token)
    {
        if (!fn) return WXL_E_INVALID;
        wxl::host::ModHost* host = wxl::core::Host();
        const uint32_t      token = g_nextToken++;
        g_watchers.push_back(Watcher{fn, user, host ? host->CurrentMod() : 0, token, true});
        ++g_live;
        if (out_token) *out_token = token;
        return WXL_OK;
    }

    void WXL_CALL UnwatchModelDraws(uint32_t token)
    {
        for (Watcher& watcher : g_watchers)
            if (watcher.token == token) Drop(watcher);
        Compact();
    }

    void WXL_CALL InvalidateState(void) { binding::gx_invalidate_state(); }

    const wxl_gx_api kApi = {
        sizeof(wxl_gx_api), &D3d9Device, &ObjectModel, &WatchModelDraws, &UnwatchModelDraws,
        &InvalidateState,
    };
}

namespace wxl::core::services
{
    const wxl_gx_api& GxApi() { return kApi; }

    namespace gx
    {
        bool Watching() { return g_live != 0; }

        void DispatchModelDraw(const wxl_gx_model_draw& draw)
        {
            wxl::host::ModHost* host = wxl::core::Host();
            ++g_depth;
            // By index, and up to the size at the start: a watcher may watch again, which grows
            // the vector, and one added during a draw starts with the next one.
            const size_t count = g_watchers.size();
            for (size_t i = 0; i < count; ++i)
            {
                if (!g_watchers[i].live) continue;
                const Watcher    watcher  = g_watchers[i];
                WatcherCall      call{&watcher, &draw};
                const uint32_t   previous = host ? host->SwapCurrentMod(watcher.owner) : 0;
                wxl::host::Fault fault;
                const bool       returned = wxl::host::CallGuarded(&InvokeWatcher, &call, &fault);
                if (host) host->SwapCurrentMod(previous);
                if (returned) continue;

                char what[96];
                wxl::host::DescribeFault(fault, what, sizeof what);
                WLOG_ERROR(kTag, "a model draw watcher faulted: %s", what);
                if (host && watcher.owner != 0)
                    host->Disable(watcher.owner, "in its model draw watcher");
                else
                    Drop(g_watchers[i]);
            }
            --g_depth;
            Compact();
        }

        void ForgetMod(uint32_t mod)
        {
            for (Watcher& watcher : g_watchers)
                if (watcher.owner == mod) Drop(watcher);
            Compact();
        }

        void Clear()
        {
            for (Watcher& watcher : g_watchers) Drop(watcher);
            Compact();
        }
    }
}
