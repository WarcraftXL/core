// SPDX-License-Identifier: GPL-3.0-or-later
// The extensions' redirects, providers and transforms (wxl.storage), each tried in registration order.

#pragma once

#include "wxl/abi/Storage.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace wxl::storage
{
    /// Registrations kept per kind; one more is refused.
    constexpr uint32_t kMaxRegistrations = 64;

    /// The longest name a redirect may answer with, terminator included.
    constexpr uint32_t kMaxRedirectName = 260;

    /// Registering is rare and may race the opens on loader threads; the lookups below take no lock
    /// and allocate nothing unless an extension claims the file. False when the list is full.
    bool AddRedirect(WXL_RedirectFn fn);
    bool AddProvider(WXL_ProvideFn fn);
    bool AddTransform(const char* suffix, WXL_TransformFn fn);

    /// The first redirect's answer for `name`, written to `out`; false when none claims it.
    /// A claim with an empty name is a decline.
    bool Redirect(const char* name, char (&out)[kMaxRedirectName]);

    /// The first provider's bytes for `name`; false when none claims it.
    bool Provide(const char* name, std::vector<uint8_t>& out);

    /// Whether a transform's suffix matches `name`: only then are the native bytes read.
    bool HasTransform(std::string_view name);

    /// The first matching transform's reshaped bytes; false when none claims them.
    bool Transform(const char* name, const uint8_t* raw, uint32_t size, std::vector<uint8_t>& out);

    /// Forgets every registration: for tests.
    void Reset();
}
