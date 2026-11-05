// SPDX-License-Identifier: GPL-3.0-or-later
// Bytes written into the client for an owner (WXL_Api::Patch): checked, recorded, put back on disable.

#pragma once

#include <cstdint>

namespace wxl::patch
{
    /// Writes `length` bytes at `address` for `owner` once the bytes there equal `expected` (null: no
    /// check). A range another owner changed is refused, so taking one owner's bytes back never undoes
    /// another's. WXL_OK, WXL_E_MISMATCH (nothing written) or WXL_E_INVALID_ARGUMENT (not writable).
    int Write(const char* name, uintptr_t address, const void* expected, const void* bytes, uint32_t length,
              uint32_t owner);

    /// Puts back the original bytes of everything `owner` wrote, newest first; answers how many.
    uint32_t DropOwner(uint32_t owner);
}
