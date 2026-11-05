// SPDX-License-Identifier: GPL-3.0-or-later
// The wxl.lua service; a Boot feature, since the glue screen's interface loads long before the Normal phase.

#include "engine/hook/Registry.hpp"
#include "engine/script/LuaRegistry.hpp"
#include "engine/script/LuaSettings.hpp"
#include "runtime/extensions/Services.hpp"
#include "runtime/settings/SettingsService.hpp"

#include "common/Log.hpp"
#include "wxl/abi/Lua.h"
#include "wxl/game/Script.hpp"
#include "wxl/offsets/free/lua.hpp"

namespace
{
    namespace gs = wxl::game::script;
    namespace off = wxl::offsets;

    int __cdecl ApiRegisterFunction(const char* name, WXL_LuaFunction fn) { return wxl::script::AddFunction(name, fn) ? 1 : 0; }
    int __cdecl ApiRegisterSettings(const WXL_SettingsSet* set) { return wxl::script::AddSettings(set) ? 1 : 0; }

    int __cdecl ApiArgCount(void* state) { return gs::ArgCount(state); }
    int __cdecl ApiType(void* state, int i) { return gs::Type(state, i); }
    double __cdecl ApiToNumber(void* state, int i) { return gs::ToNumber(state, i); }
    const char* __cdecl ApiToString(void* state, int i) { return gs::ToString(state, i); }
    int __cdecl ApiToBoolean(void* state, int i) { return gs::ToBoolean(state, i) ? 1 : 0; }
    void __cdecl ApiPushNil(void* state) { gs::PushNil(state); }
    void __cdecl ApiPushNumber(void* state, double v) { gs::PushNumber(state, v); }
    void __cdecl ApiPushString(void* state, const char* v) { gs::PushString(state, v ? v : ""); }
    void __cdecl ApiPushBoolean(void* state, int v) { gs::PushBoolean(state, v != 0); }
    void __cdecl ApiNewTable(void* state) { wxl::game::Native<off::lua_createtableFn>(off::lua_createtable)(state, 0, 0); }

    /// t[key] = value on the table at the top: key and value pushed above it, then a raw set at -3.
    void __cdecl ApiSetFieldNumber(void* state, const char* key, double v)
    {
        gs::PushString(state, key);
        gs::PushNumber(state, v);
        gs::RawSet(state, -3);
    }
    void __cdecl ApiSetFieldString(void* state, const char* key, const char* v)
    {
        gs::PushString(state, key);
        gs::PushString(state, v ? v : "");
        gs::RawSet(state, -3);
    }
    void __cdecl ApiSetFieldBoolean(void* state, const char* key, int v)
    {
        gs::PushString(state, key);
        gs::PushBoolean(state, v != 0);
        gs::RawSet(state, -3);
    }
    void __cdecl ApiError(void* state, const char* message) { gs::Error(state, "%s", message ? message : "error"); }

    const WXL_LuaApi g_luaApi = {
        sizeof(WXL_LuaApi),
        WXL_LUA_API_VERSION,
        &ApiRegisterFunction,
        &ApiRegisterSettings,
        &ApiArgCount,
        &ApiType,
        &ApiToNumber,
        &ApiToString,
        &ApiToBoolean,
        &ApiPushNil,
        &ApiPushNumber,
        &ApiPushString,
        &ApiPushBoolean,
        &ApiNewTable,
        &ApiSetFieldNumber,
        &ApiSetFieldString,
        &ApiSetFieldBoolean,
        &ApiError,
    };

    bool InstallLuaService()
    {
        if (!wxl::script::InstallSeams() || !wxl::script::InstallSettingsFunctions())
        {
            WLOG_WARN("lua: %s is not published", WXL_LUA_API_NAME);
            return false;
        }
        wxl::runtime::extensions::PublishInterface(WXL_LUA_API_NAME, WXL_LUA_API_VERSION, const_cast<WXL_LuaApi*>(&g_luaApi));
        wxl::runtime::settings::InstallInterface();
        return true;
    }
}

WXL_REGISTER_FEATURE_PHASED("lua-service", true, InstallLuaService, ::wxl::hook::Phase::Boot)
