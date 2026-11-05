// SPDX-License-Identifier: GPL-3.0-or-later
// None of these functions raises: a bad set, key or value answers nil or false.

#include "engine/script/LuaSettings.hpp"

#include "common/Log.hpp"
#include "engine/script/LuaRegistry.hpp"
#include "wxl/game/Script.hpp"
#include "wxl/offsets/free/lua.hpp"

#include <cstdio>
#include <cstring>

namespace
{
    namespace gs  = wxl::game::script;
    namespace off = wxl::offsets;

    constexpr int    kTypeBoolean = 1;
    constexpr int    kTypeNumber  = 3;
    constexpr int    kTypeString  = 4;
    constexpr size_t kMaxKey      = 64;
    constexpr size_t kMaxText     = 128;

    constexpr int kMaxSets = 32;
    const WXL_SettingsSet* g_sets[kMaxSets] = {};
    int g_setCount = 0;

    /// The set the first argument names, or null.
    const WXL_SettingsSet* SetArg(void* state)
    {
        if (gs::ArgCount(state) < 1 || gs::Type(state, 1) != kTypeString) return nullptr;
        const char* name = gs::ToString(state, 1);
        if (!name) return nullptr;
        for (int i = 0; i < g_setCount; ++i)
            if (std::strcmp(g_sets[i]->name, name) == 0) return g_sets[i];
        return nullptr;
    }

    void PushValue(void* state, const WXL_SettingValue& v)
    {
        switch (v.type)
        {
            case WXL_SETTING_BOOLEAN: gs::PushBoolean(state, v.boolean != 0); break;
            case WXL_SETTING_NUMBER:  gs::PushNumber(state, v.number); break;
            case WXL_SETTING_STRING:  gs::PushString(state, v.string ? v.string : ""); break;
            default:                  gs::PushNil(state); break;
        }
    }

    void NewTable(void* state, int arrayCount, int recordCount)
    {
        wxl::game::Native<off::lua_createtableFn>(off::lua_createtable)(state, arrayCount, recordCount);
    }

    bool Available(const WXL_SettingsSet* s) { return !s->IsAvailable || s->IsAvailable(s->user) != 0; }

    int __cdecl LuaList(void* state)
    {
        NewTable(state, g_setCount, 0);
        for (int i = 0; i < g_setCount; ++i)
        {
            gs::PushNumber(state, i + 1);
            gs::PushString(state, g_sets[i]->name);
            gs::RawSet(state, -3);
        }
        return 1;
    }

    int __cdecl LuaIsAvailable(void* state)
    {
        const WXL_SettingsSet* s = SetArg(state);
        gs::PushBoolean(state, s && Available(s));
        return 1;
    }

    int __cdecl LuaGet(void* state)
    {
        const WXL_SettingsSet* s = SetArg(state);
        const char* key = s && gs::ArgCount(state) >= 2 && gs::Type(state, 2) == kTypeString ? gs::ToString(state, 2) : nullptr;
        WXL_SettingValue v = {};
        if (key && s->Get(s->user, key, &v)) PushValue(state, v);
        else gs::PushNil(state);
        return 1;
    }

    int __cdecl LuaSet(void* state)
    {
        const WXL_SettingsSet* s = SetArg(state);
        bool ok = false;
        if (s && s->Set && gs::ArgCount(state) >= 3 && gs::Type(state, 2) == kTypeString)
        {
            char key[kMaxKey] = {};
            std::snprintf(key, sizeof key, "%s", gs::ToString(state, 2));
            WXL_SettingValue v = {};
            char text[kMaxText] = {};
            switch (gs::Type(state, 3))
            {
                case kTypeBoolean: v.type = WXL_SETTING_BOOLEAN; v.boolean = gs::ToBoolean(state, 3) ? 1 : 0; break;
                case kTypeNumber: v.type = WXL_SETTING_NUMBER;  v.number = gs::ToNumber(state, 3); break;
                case kTypeString:
                    v.type = WXL_SETTING_STRING;
                    std::snprintf(text, sizeof text, "%s", gs::ToString(state, 3));
                    v.string = text;
                    break;
                default: v.type = WXL_SETTING_NIL; break;
            }
            ok = v.type != WXL_SETTING_NIL && s->Set(s->user, key, &v) != 0;
        }
        gs::PushBoolean(state, ok);
        return 1;
    }

    int __cdecl LuaGetAll(void* state)
    {
        const WXL_SettingsSet* s = SetArg(state);
        if (!s)
        {
            gs::PushNil(state);
            return 1;
        }
        NewTable(state, 0, static_cast<int>(s->keyCount));
        for (uint32_t i = 0; i < s->keyCount; ++i)
        {
            WXL_SettingValue v = {};
            if (!s->keys[i] || !s->Get(s->user, s->keys[i], &v)) continue;
            gs::PushString(state, s->keys[i]);
            PushValue(state, v);
            gs::RawSet(state, -3);
        }
        return 1;
    }

    int __cdecl LuaApplyPreset(void* state)
    {
        const WXL_SettingsSet* s = SetArg(state);
        bool ok = false;
        if (s && s->ApplyPreset && gs::ArgCount(state) >= 2 && gs::Type(state, 2) == kTypeString)
        {
            char name[kMaxKey] = {};
            std::snprintf(name, sizeof name, "%s", gs::ToString(state, 2));
            ok = s->ApplyPreset(s->user, name) != 0;
        }
        gs::PushBoolean(state, ok);
        return 1;
    }

    int __cdecl LuaSave(void* state)
    {
        const WXL_SettingsSet* s = SetArg(state);
        gs::PushBoolean(state, s && s->Save && s->Save(s->user) != 0);
        return 1;
    }
}

namespace wxl::script
{
    bool InstallSettingsFunctions()
    {
        return AddFunction("WXL_Settings_List", &LuaList) && AddFunction("WXL_Settings_IsAvailable", &LuaIsAvailable)
               && AddFunction("WXL_Settings_Get", &LuaGet) && AddFunction("WXL_Settings_Set", &LuaSet)
               && AddFunction("WXL_Settings_GetAll", &LuaGetAll)
               && AddFunction("WXL_Settings_ApplyPreset", &LuaApplyPreset) && AddFunction("WXL_Settings_Save", &LuaSave);
    }

    bool AddSettings(const WXL_SettingsSet* set)
    {
        if (!set || set->structSize < sizeof(WXL_SettingsSet) || !set->name || !*set->name || !set->Get) return false;
        for (int i = 0; i < g_setCount; ++i)
            if (std::strcmp(g_sets[i]->name, set->name) == 0)
            {
                g_sets[i] = set;
                return true;
            }
        if (g_setCount == kMaxSets) return false;
        g_sets[g_setCount++] = set;
        WLOG_INFO("lua: settings set \"%s\" registered (%u key(s))", set->name, set->keyCount);
        return true;
    }

    uint32_t SettingsCount() { return static_cast<uint32_t>(g_setCount); }
    const WXL_SettingsSet* SettingsAt(uint32_t index)
    { return index < static_cast<uint32_t>(g_setCount) ? g_sets[index] : nullptr; }
}
