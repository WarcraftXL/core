// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The dropped calls: see Drops.hpp.

#include "tuning/Drops.hpp"

#include "common/Log.hpp"
#include "runtime/Session.hpp"
#include "runtime/Settings.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace wxl::core::tuning::drops
{
    namespace
    {
        constexpr const char* kTag = "engine";
        /// The five-byte NOP (`nop dword ptr [eax + eax*1 + 0]`).
        constexpr uint8_t kNop[5] = {0x0F, 0x1F, 0x44, 0x00, 0x00};
    }

    Outcome Drop(uint8_t* code, uintptr_t at, uintptr_t callee)
    {
        if (std::memcmp(code, kNop, sizeof kNop) == 0) return Outcome::already;
        const int32_t rel = static_cast<int32_t>(static_cast<uint32_t>(callee) - static_cast<uint32_t>(at + 5));
        uint8_t       call[5] = {0xE8};
        std::memcpy(call + 1, &rel, sizeof rel);
        if (std::memcmp(code, call, sizeof call) != 0) return Outcome::foreign;
        std::memcpy(code, kNop, sizeof kNop);
        return Outcome::dropped;
    }

    void Install()
    {
#if defined(_M_IX86)
        const settings::Settings& s = session::CurrentSettings();
        unsigned dropped = 0, already = 0, rows = 0;
        char     foreign[160] = {};
#define WXL_DROP_CALL_X86(setting, caller, at, callee, callee_name)                                                            \
    if (s.setting)                                                                                                            \
    {                                                                                                                         \
        ++rows;                                                                                                               \
        auto* code = reinterpret_cast<uint8_t*>(uintptr_t{at});                                                               \
        DWORD was  = 0;                                                                                                       \
        if (VirtualProtect(code, 5, PAGE_EXECUTE_READWRITE, &was))                                                            \
        {                                                                                                                     \
            const Outcome outcome = Drop(code, at, callee);                                                                   \
            VirtualProtect(code, 5, was, &was);                                                                               \
            FlushInstructionCache(GetCurrentProcess(), code, 5);                                                              \
            if (outcome == Outcome::dropped) ++dropped;                                                                       \
            else if (outcome == Outcome::already) ++already;                                                                  \
            else if (!foreign[0]) std::snprintf(foreign, sizeof foreign, "%s's call to %s at 0x%08X", caller, callee_name, at); \
        }                                                                                                                     \
        else if (!foreign[0]) std::snprintf(foreign, sizeof foreign, "%s at 0x%08X (not writable)", caller, at);             \
    }
#include "wxl/tuning/drops_x86.def"
#undef WXL_DROP_CALL_X86
        if (!s.trim_alloc)
            WLOG_INFO(kTag, "trim_alloc off: SMemAlloc and SMemFree keep the heap size query whose answer they drop");
        else if (!foreign[0])
            WLOG_INFO(kTag, "trim_alloc: SMemAlloc and SMemFree no longer ask the heap the size of each block, an answer they "
                            "drop (%u call(s) taken out%s)",
                      dropped, already ? ", some already" : "");
        else
            WLOG_WARN(kTag, "trim_alloc: %u of %u call(s) taken out; %s is not the stock call, left as it is", dropped, rows, foreign);
#endif
    }
}
