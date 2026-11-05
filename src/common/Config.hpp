// SPDX-License-Identifier: GPL-3.0-or-later
// The core's own settings (WXL_* keys): the environment first, then WarcraftXL.cfg.

#pragma once

#include <cstddef>

namespace wxl::config
{
    /// Copies a key's value into `buffer`; false when it is unset, empty, or does not fit.
    bool Raw(const char* name, char* buffer, size_t capacity);

    /// A key as a boolean: a value starting with 0, n or f (any case) is false, anything else true.
    bool Env(const char* name, bool fallback);
}
