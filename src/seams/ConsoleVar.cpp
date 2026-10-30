// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The ConsoleVar seams: what the runtime does when a console variable is set, on both builds.

#include "wxl/seams/seams.h"

#include "binding/Binding.hpp"
#include "runtime/Core.hpp"
#include "services/CVarService.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>

namespace
{
    /// The flag of a variable the client refuses to write.
    constexpr uint32_t kReadOnly = 0x4;

    /// The client's own comparison of a value against the current one: without case.
    bool SameValue(const char* a, const char* b)
    {
        for (;; ++a, ++b)
        {
            const int ca = std::tolower(static_cast<unsigned char>(*a));
            const int cb = std::tolower(static_cast<unsigned char>(*b));
            if (ca != cb) return false;
            if (!ca) return true;
        }
    }

    wxl_str Str(const char* text)
    {
        return wxl_str{text ? text : "", text ? static_cast<uint32_t>(std::strlen(text)) : 0u};
    }
}

namespace wxl::seam::ConsoleVar::InternalSet
{
    void WXL_CALL Before(void* self, const char* value, uint8_t set_value, uint8_t, uint8_t, uint8_t)
    {
        if (!self || !value || !set_value) return;
        if (wxl::core::binding::cvar_flags(self) & kReadOnly) return;

        // What the function tests before it writes: no value yet, or another one.
        const char* current = wxl::core::binding::cvar_value(self);
        if (current && SameValue(current, value)) return;

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        wxl_cvar_args args{};
        args.name     = Str(wxl::core::binding::cvar_name(self));
        args.previous = Str(current);
        args.value    = Str(value);
        host->Raise(WXL_EV_CVarChanged, &args);
    }
}

namespace wxl::seam::ConsoleVar::Initialize
{
    void WXL_CALL Before(const char*)
    {
        // The registrations made before the file is read are made now, so its lines set them.
        wxl::core::services::cvar::ConfigReading();
    }

    void WXL_CALL After(const char*)
    {
        wxl::core::services::cvar::ConfigRead();
    }
}

namespace wxl::seam::ConsoleVar::DeleteCharacterCVars
{
    void WXL_CALL After()
    {
        wxl::core::services::cvar::CategoryDeleted(0x20);
    }
}

namespace wxl::seam::ConsoleVar::DeleteAccountCVars
{
    void WXL_CALL After()
    {
        wxl::core::services::cvar::CategoryDeleted(0x10);
    }
}
