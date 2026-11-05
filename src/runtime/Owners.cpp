// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/Owners.hpp"

#include "common/Log.hpp"
#include "wxl/Common.hpp"

#include <windows.h>
#include <malloc.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wxl::runtime::owners
{
    namespace
    {
        struct Owner
        {
            std::string       name;
            std::string       folder;
            bool              lowLevel = false;
            std::atomic<bool> running{ true };
        };

        struct Kind
        {
            const char* what;
            TakeBack    takeBack;
            Kept        kept;
        };

        constexpr DWORD  kCppException = 0xE06D7363;

        /// A fixed table: another thread reading it never sees it move.
        constexpr Id     kMaxOwners = 256;
        Owner*           g_owners[kMaxOwners] = {};
        std::atomic<Id>  g_count{ 0 };

        std::vector<Kind>& Kinds()
        {
            static std::vector<Kind> kinds;
            return kinds;
        }

        thread_local Id g_current = kCore;

        Owner* Find(Id owner)
        {
            return owner >= 1 && owner <= g_count.load(std::memory_order_acquire) ? g_owners[owner - 1] : nullptr;
        }

        /// "0x00000000 (Name.dll+0x1234)" for an address inside a module, the bare address otherwise.
        void DescribeAddress(uintptr_t address, char* out, size_t capacity)
        {
            HMODULE module = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(address), &module) &&
                module)
            {
                char path[MAX_PATH] = {};
                GetModuleFileNameA(module, path, MAX_PATH);
                const char* name = std::strrchr(path, '\\');
                name             = name ? name + 1 : path;
                std::snprintf(out, capacity, "0x%08X (%s+0x%X)", unsigned(address), name,
                              unsigned(address - reinterpret_cast<uintptr_t>(module)));
                return;
            }
            std::snprintf(out, capacity, "0x%08X", unsigned(address));
        }

        void Describe(const seh::Fault& fault, char* out, size_t capacity)
        {
            char where[160];
            DescribeAddress(fault.address, where, sizeof where);
            switch (fault.code)
            {
            case EXCEPTION_ACCESS_VIOLATION:
                std::snprintf(out, capacity, "an access violation %s 0x%08X at %s", fault.write ? "writing" : "reading",
                              unsigned(fault.target), where);
                return;
            case EXCEPTION_INT_DIVIDE_BY_ZERO: std::snprintf(out, capacity, "a division by zero at %s", where); return;
            case EXCEPTION_ILLEGAL_INSTRUCTION: std::snprintf(out, capacity, "an illegal instruction at %s", where); return;
            case EXCEPTION_STACK_OVERFLOW: std::snprintf(out, capacity, "a stack overflow at %s", where); return;
            case kCppException: std::snprintf(out, capacity, "a C++ exception nothing caught, thrown at %s", where); return;
            default: std::snprintf(out, capacity, "exception 0x%08X at %s", unsigned(fault.code), where); return;
            }
        }

        /// Holds no object with a destructor: the guard may leave it through a longjmp.
        __declspec(noinline) bool Guarded(void (*fn)(void*), void* context, seh::Fault* fault)
        {
            WXL_SEH_TRY(seh::Catch::Any)
            {
                fn(context);
                return true;
            }
            WXL_SEH_EXCEPT
            {
                *fault = WXL_SEH_FAULT();
                return false;
            }
        }
    }

    Id Add(const char* name, const char* folder)
    {
        const Id count = g_count.load();
        if (count == kMaxOwners) return kCore;
        auto* owner = new Owner();
        owner->name   = name ? name : "extension";
        owner->folder = folder && *folder ? folder : owner->name;
        g_owners[count] = owner;
        g_count.store(count + 1, std::memory_order_release);
        return count + 1;
    }

    const char* Name(Id owner)
    {
        if (owner == kCore) return "core";
        const Owner* o = Find(owner);
        return o ? o->name.c_str() : "?";
    }

    void GrantLowLevel(Id owner)
    {
        if (Owner* o = Find(owner)) o->lowLevel = true;
    }

    bool LowLevel(Id owner)
    {
        if (owner == kCore) return true;
        const Owner* o = Find(owner);
        return o && o->lowLevel;
    }

    const char* Folder(Id owner)
    {
        const Owner* o = Find(owner);
        return o ? o->folder.c_str() : "";
    }

    bool Running(Id owner)
    {
        if (owner == kCore) return true;
        const Owner* o = Find(owner);
        return o && o->running.load(std::memory_order_acquire);
    }

    Id Current() { return g_current; }

    bool Call(Id owner, const char* what, void (*fn)(void*), void* context)
    {
        const Id previous = g_current;
        g_current         = owner;
        if (owner == kCore)
        {
            fn(context);
            g_current = previous;
            return true;
        }
        seh::Fault fault;
        const bool returned = Guarded(fn, context, &fault);
        g_current           = previous;
        if (returned) return true;

        if (fault.code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        char described[256];
        Describe(fault, described, sizeof described);
        char reason[320];
        std::snprintf(reason, sizeof reason, "%s in %s", described, what ? what : "a call");
        Disable(owner, reason);
        return false;
    }

    void OnDisable(const char* what, TakeBack takeBack, Kept kept)
    {
        if (takeBack) Kinds().push_back(Kind{ what, takeBack, kept });
    }

    void Disable(Id owner, const char* reason)
    {
        Owner* o = Find(owner);
        if (!o || !o->running.exchange(false)) return;

        std::string taken, kept;
        for (const Kind& kind : Kinds())
        {
            const uint32_t n = kind.takeBack(owner);
            if (n)
            {
                if (!taken.empty()) taken += ", ";
                taken += std::to_string(n) + " " + kind.what;
            }
            const uint32_t k = kind.kept ? kind.kept(owner) : 0;
            if (k)
            {
                if (!kept.empty()) kept += ", ";
                kept += std::to_string(k) + " resident " + kind.what;
            }
        }
        std::string held = taken.empty() ? "it held nothing to take back" : "taken back: " + taken;
        if (!kept.empty()) held += "; kept: " + kept;
        WLOG_ERROR("extensions: '%s' is disabled: %s; %s", o->name.c_str(), reason ? reason : "no reason given",
                   held.c_str());
    }
}
