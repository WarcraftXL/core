// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/HookPoints.hpp"

#include "common/Log.hpp"
#include "engine/hook/Hook.hpp"

#include <cstring>
#include <iterator>
#include <string_view>

namespace wxl::runtime::hookpoints
{
    namespace
    {
        struct Point
        {
            const char* name;
            uintptr_t   address;
        };

        constexpr Point kPoints[] = {
#define WXL_HOOK_POINT(name, address) { name, address },
#include "runtime/HookPoints.def"
#undef WXL_HOOK_POINT
        };

        /// The SDK publishes the names: this table lists the same ones, in the same order.
        constexpr const char* kSdkNames[] = {
#define WXL_HOOK_POINT_NAME(name) name,
#include "wxl/HookPoints.def"
#undef WXL_HOOK_POINT_NAME
        };

        constexpr bool SameNames()
        {
            if (std::size(kPoints) != std::size(kSdkNames)) return false;
            for (size_t i = 0; i < std::size(kPoints); ++i)
                if (std::string_view(kPoints[i].name) != std::string_view(kSdkNames[i])) return false;
            return true;
        }
        static_assert(SameNames(), "runtime/HookPoints.def and the SDK's wxl/HookPoints.def list different names");

        const Point* Find(const char* name)
        {
            if (!name) return nullptr;
            for (const Point& p : kPoints)
                if (std::strcmp(p.name, name) == 0) return &p;
            return nullptr;
        }
    }

    int AttachByName(const char* pointName, void* detour, void** original, int priority)
    {
        const Point* p = Find(pointName);
        if (!p)
        {
            WLOG_ERROR("hookpoints: '%s' is not a registered hook point", pointName ? pointName : "(null)");
            return 0;
        }
        return wxl::hook::Install(pointName, p->address, detour, original, priority) ? 1 : 0;
    }
}
