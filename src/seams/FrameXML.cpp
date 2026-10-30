// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The FrameXML seam: the client's verdict on an interface's signature, shown to mods, who may
// have the interface taken as signed.

#include "wxl/seams/seams.h"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Core.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <cstdint>
#include <cstring>

namespace
{
    constexpr const char* kTag = "interface";

    /// Set while the gate has the client judge a file: that call goes through to the client.
    thread_local bool t_judging = false;

    wxl_str Str(const char* text)
    {
        return text ? wxl_str{text, static_cast<uint32_t>(std::strlen(text))} : wxl_str{"", 0};
    }

    const char* VerdictName(int32_t verdict)
    {
        switch (verdict)
        {
        case WXL_INTERFACE_SIGNATURE_MISSING:  return "missing";
        case WXL_INTERFACE_SIGNATURE_CORRUPT:  return "corrupt";
        case WXL_INTERFACE_SIGNATURE_MODIFIED: return "modified";
        case WXL_INTERFACE_SIGNATURE_SIGNED:   return "signed";
        default:                               return "unknown";
        }
    }
}

namespace wxl::seam::FrameXML::CheckSignature
{
    bool WXL_CALL Gate(int32_t* result, const char** toc, const char** extra, const uint8_t** modulus, uint8_t** digest)
    {
        namespace binding = wxl::core::binding;
        if (t_judging) return false;
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !result || !toc || !*toc || !extra || !modulus || !digest || !*digest ||
            !host->Listening(WXL_EV_InterfaceSignature))
            return false;

        t_judging = true;
        int32_t verdict = binding::interface_check_signature(*toc, *extra, *modulus, *digest);
        t_judging = false;
        if (verdict < 0) return false;

        wxl_interface_signature_args args{};
        args.file    = Str(*toc);
        args.extra   = Str(*extra);
        args.verdict = static_cast<uint32_t>(verdict);
        wxl_bool accepted = WXL_FALSE;
        args.accepted = &accepted;
        host->Raise(WXL_EV_InterfaceSignature, &args);

        if (accepted != WXL_FALSE && verdict != WXL_INTERFACE_SIGNATURE_SIGNED)
        {
            // A missing or corrupt .sig stops the check before the digest; the caller holds what
            // it loads to that digest, so it is made as the client makes it.
            const bool hashed = verdict == WXL_INTERFACE_SIGNATURE_MODIFIED ||
                                binding::interface_signature_digest(*toc, *extra, *digest) != WXL_FALSE;
            if (hashed)
            {
                WLOG_DEBUG(kTag, "%s: %s, taken as signed at a mod's answer", *toc, VerdictName(verdict));
                verdict = WXL_INTERFACE_SIGNATURE_SIGNED;
            }
            else
            {
                WLOG_WARN(kTag, "%s: %s, accepted by a mod, but its digest could not be made; left %s", *toc,
                          VerdictName(verdict), VerdictName(verdict));
            }
        }
        else
        {
            WLOG_DEBUG(kTag, "%s: %s", *toc, VerdictName(verdict));
        }
        *result = verdict;
        return true;
    }
}
