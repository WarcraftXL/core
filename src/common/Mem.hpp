// SPDX-License-Identifier: GPL-3.0-or-later
// Writes into protected code or data: unprotect, write, restore, flush the instruction cache.

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstring>

namespace wxl::mem
{
    /// Runs `write` while [destination, destination + length) is writable; false when it could not be.
    template <class Fn>
    bool WithWritable(void* destination, size_t length, Fn&& write)
    {
        DWORD old = 0;
        if (!VirtualProtect(destination, length, PAGE_EXECUTE_READWRITE, &old)) return false;
        write();
        DWORD restored = 0;
        VirtualProtect(destination, length, old, &restored);
        FlushInstructionCache(GetCurrentProcess(), destination, length);
        return true;
    }

    /// Swaps one pointer slot (a vtable entry); `previous` receives the old value when not null.
    inline bool SwapPointer(void** slot, void* value, void** previous)
    {
        return WithWritable(slot, sizeof(void*), [&] {
            if (previous) *previous = *slot;
            *slot = value;
        });
    }

    inline bool Patch(void* destination, const void* source, size_t length)
    {
        return WithWritable(destination, length, [&] { std::memcpy(destination, source, length); });
    }
}
