// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.script service: the SDK's script table over the client's own Lua, on both builds.

#pragma once

#include "wxl/host/guard.hpp"
#include "wxl/services/script.h"

#include <cstdint>
#include <type_traits>

namespace wxl::core::services
{
    /// The table published under WXL_SCRIPT_SERVICE, version WXL_SCRIPT_SERVICE_VERSION.
    const wxl_script_api& ScriptApi();

    // What the FrameScript seams and the host drive on the service. Everything a mod registered
    // lives exactly as long as the context it was registered on: the client's Lua state is
    // closed with the context, which takes every global and every event listener with it.
    namespace script
    {
        /// A context exists: registrations are accepted on it until OnContextClosing.
        void OnContextReady(uint32_t context, void* state);

        /// The context is about to be closed. Forgets every registration made on it.
        void OnContextClosing();

        /// The client installed its own event table on the live context. The service installs
        /// it again with the events mods declared appended, so each keeps an index to signal.
        void OnClientEventsInstalled(const char** names, uint32_t count);

        /// True while the service itself is installing the event table.
        bool Reissuing();

        /// The name at an index of the installed table -- the client's events, then the ones
        /// mods declared behind them -- or null for an index outside it.
        const char* EventName(uint32_t index);

        /// Withdraws every function a mod registered: the host's disable handler.
        void OnModDisabled(uint32_t token);

        bool     Live();
        uint32_t Context();
        void*    State();

        /**
         * @brief Calls fn(context) on the client's script state and leaves the state as it was.
         *
         * Between frames the client ends the process when its script stack is not empty
         * (CGGameUI::Idle, error #134), and a hardware fault inside a call into its Lua abandons
         * that call with its values and its calls still on the state. So every path of the
         * runtime that calls into the state goes through here: the top and a mark are taken
         * before; after, calls left unfinished are dropped (binding script_unwind), and a top that
         * moved is put back. Both are told once per path, by `path`, the place that called.
         *
         * Without a live context, fn is called as it is.
         * @return whether fn returned; when it did not, @p fault says why and the state is back
         *         where it was.
         */
        bool Keep(const char* path, wxl::host::GuardedFn fn, void* context, wxl::host::Fault* fault);

        /// Keep for a callable.
        template <typename F>
        bool Keep(const char* path, F& body, wxl::host::Fault* fault)
        {
            return Keep(
                path, [](void* context) { (*static_cast<F*>(context))(); }, &body, fault);
        }
    }
}
