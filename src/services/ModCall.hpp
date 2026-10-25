// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Calling a function a mod handed a service: as that mod, under the crash guard.

#pragma once

#include "common/Log.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/guard.hpp"
#include "wxl/host/mod_host.hpp"

#include <cstdint>
#include <string>
#include <type_traits>

namespace wxl::core::services
{
    namespace detail
    {
        template <class F>
        void InvokeModCall(void* context)
        {
            (*static_cast<F*>(context))();
        }
    }

    /**
     * @brief Runs @p f, a call into @p mod's code, with the host's current mod set to it and under
     *        the crash guard.
     *
     * False when it faulted: the fault is logged under @p tag, saying @p what the call was, and
     * the mod is disabled.
     */
    template <class F>
    bool CallMod(uint32_t mod, const char* tag, const char* what, F&& f)
    {
        wxl::host::ModHost* host     = wxl::core::Host();
        const uint32_t      previous = host ? host->SwapCurrentMod(mod) : 0;
        wxl::host::Fault    fault;
        const bool          returned = wxl::host::CallGuarded(&detail::InvokeModCall<std::remove_reference_t<F>>,
                                                              const_cast<void*>(static_cast<const void*>(&f)), &fault);
        if (host) host->SwapCurrentMod(previous);
        if (returned) return true;

        char described[96];
        wxl::host::DescribeFault(fault, described, sizeof described);
        const wxl::host::LoadedMod* loaded = host ? host->ModOf(mod) : nullptr;
        WLOG_ERROR(tag, "%s of mod %s faulted: %s; the mod is disabled", what, loaded ? loaded->id.c_str() : "?",
                   described);
        if (host && mod) host->Disable(mod, std::string("in ") + what);
        return false;
    }
}
