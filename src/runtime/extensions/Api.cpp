// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/extensions/Api.hpp"

#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/patch/Patch.hpp"
#include "runtime/HookPoints.hpp"
#include "runtime/Owners.hpp"
#include "runtime/extensions/Services.hpp"

#include <cstdarg>
#include <cstdio>

namespace wxl::runtime::extensions
{
    namespace
    {
        constexpr size_t kMaxLogLine = 1024;

        void __cdecl ApiLog(int level, const char* tag, const char* fmt, ...)
        {
            const int clamped = level < WXL_LOG_TRACE ? WXL_LOG_TRACE : level > WXL_LOG_ERROR ? WXL_LOG_ERROR : level;
            const log::Level severity = log::Level(clamped);
            if (!fmt || !log::Enabled(severity)) return;

            // Formatted here, so a foreign format string never reaches the sink unbounded.
            char    line[kMaxLogLine];
            va_list args;
            va_start(args, fmt);
            std::vsnprintf(line, sizeof line, fmt, args);
            va_end(args);
            log::Write(severity, "%s: %s", tag ? tag : "extension", line);
        }

        int __cdecl ApiConnect(uint32_t event, WXL_Handle subject, int32_t priority, WXL_EventFn handler, void* user,
                               WXL_ReleaseFn release, WXL_Connection* out)
        {
            if (event < uint32_t(events::Event::Count))
                return events::Connect(events::Event(event), subject, priority, handler, user, release, out);
            WLOG_ERROR("extensions: '%s' connected to unknown event %u", owners::Name(owners::Current()), event);
            if (out) *out = WXL_Connection{};
            return WXL_E_INVALID_ARGUMENT;
        }

        int __cdecl ApiDisconnect(WXL_Connection connection) { return events::Disconnect(connection); }

        int __cdecl ApiConnected(WXL_Connection connection) { return events::Connected(connection); }

        void __cdecl ApiEmit(uint32_t event, const void* args)
        {
            if (event < uint32_t(events::Event::Count)) return events::EmitRaw(events::Event(event), args);
            WLOG_ERROR("extensions: emit of unknown event %u ignored", event);
        }

        /// Raw detours and patches: the core's own, or an extension granted permissions.low_level.
        bool MayHook(const char* what)
        {
            const owners::Id caller = owners::Current();
            if (owners::LowLevel(caller)) return true;
            WLOG_ERROR("extensions: '%s' may not change '%s' of the client: add \"permissions\": { \"low_level\": true } "
                       "to its wxl.json (for a call, wxl::Intercept needs none)",
                       owners::Name(caller), what ? what : "?");
            return false;
        }

        int __cdecl ApiHookAttach(const char* name, uintptr_t target, void* detour, void** original, int priority)
        {
            if (!target || !detour || !original || !MayHook(name)) return 0;
            return hook::Install(name ? name : "extension", target, detour, original, priority) ? 1 : 0;
        }

        int __cdecl ApiHookAttachEx(const char* name, uintptr_t target, void* detour, void** original, int priority,
                                    uint32_t flags)
        {
            if (!target || !detour || !original || !MayHook(name)) return 0;
            return hook::Install(name ? name : "extension", target, detour, original, priority,
                                 (flags & WXL_HOOK_RESIDENT) != 0) ? 1 : 0;
        }

        int __cdecl ApiHookAttachByName(const char* pointName, void* detour, void** original, int priority)
        {
            if (!detour || !original || !MayHook(pointName)) return 0;
            return hookpoints::AttachByName(pointName, detour, original, priority);
        }

        /// An interceptor's detour: no permission needed, its handlers run under the guard (ApiRun).
        int __cdecl ApiInterceptAttach(const char* name, uintptr_t target, void* detour, void** original)
        {
            if (!target || !detour || !original) return 0;
            if (!hook::Install(name ? name : "interceptor", target, detour, original, hook::kDefaultPriority)) return 0;
            // One attached after the extensions loaded is enabled now.
            hook::EnableAll();
            return 1;
        }

        int __cdecl ApiPatch(const char* name, uintptr_t address, const void* expected, const void* bytes, uint32_t length)
        {
            if (!MayHook(name ? name : "a patch")) return WXL_E_DENIED;
            return patch::Write(name, address, expected, bytes, length, owners::Current());
        }

        uint32_t __cdecl ApiSelf() { return owners::Current(); }

        int __cdecl ApiRun(uint32_t self, void(__cdecl* fn)(void*), void* context)
        {
            if (!fn) return WXL_E_INVALID_ARGUMENT;
            if (!owners::Running(self)) return WXL_E_EXPIRED;
            return owners::Call(self, "an interceptor", fn, context) ? WXL_OK : WXL_E_EXPIRED;
        }

        void __cdecl ApiPublishInterface(const char* name, uint32_t version, void* iface)
        {
            PublishInterface(name, version, iface);
        }

        void* __cdecl ApiGetInterface(const char* name, uint32_t version) { return GetInterface(name, version); }

        const WXL_Api g_api = {
            sizeof(WXL_Api),
            WXL_API_VERSION,
            &ApiLog,
            &ApiConnect,
            &ApiDisconnect,
            &ApiConnected,
            &ApiEmit,
            &ApiHookAttach,
            &ApiHookAttachByName,
            &ApiInterceptAttach,
            &ApiSelf,
            &ApiRun,
            &ApiPublishInterface,
            &ApiGetInterface,
            &ApiPatch,
            &ApiHookAttachEx,
        };
    }

    const WXL_Api& Api() { return g_api; }
}
