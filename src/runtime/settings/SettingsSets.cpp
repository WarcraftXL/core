// SPDX-License-Identifier: GPL-3.0-or-later
// Each extension's settings as a WXL_SettingsSet (profiles, the client's Lua) and /wxl in the chat.

#include "runtime/settings/SettingsService.hpp"

#include "engine/script/LuaRegistry.hpp"
#include "engine/script/LuaSettings.hpp"
#include "runtime/settings/Store.hpp"
#include "wxl/game/Script.hpp"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

namespace wxl::runtime::settings
{
    namespace
    {
        namespace gs = game::script;

        struct LuaSet
        {
            std::string              ext;
            std::vector<std::string> keys;
            std::vector<const char*> keyPtrs;
            std::string              scratch; ///< the last string Get handed out
            WXL_SettingsSet          set{};
        };

        /// Never moves: the sets are registered by address.
        std::deque<LuaSet>& Sets()
        {
            static std::deque<LuaSet> sets;
            return sets;
        }

        int __cdecl SetGet(void* user, const char* key, WXL_SettingValue* out)
        {
            auto*          s       = static_cast<LuaSet*>(user);
            const Setting* setting = Shared().Find(s->ext, key ? key : "");
            if (!setting || !out) return 0;
            switch (setting->kind)
            {
            case Kind::Bool:
                out->type    = WXL_SETTING_BOOLEAN;
                out->boolean = setting->value == "true";
                break;
            case Kind::Integer:
            case Kind::Number:
                out->type   = WXL_SETTING_NUMBER;
                out->number = std::strtod(setting->value.c_str(), nullptr);
                break;
            case Kind::Text:
                s->scratch  = setting->value;
                out->type   = WXL_SETTING_STRING;
                out->string = s->scratch.c_str();
                break;
            }
            return 1;
        }

        int __cdecl SetSet(void* user, const char* key, const WXL_SettingValue* value)
        {
            auto* s = static_cast<LuaSet*>(user);
            if (!key || !value) return 0;
            std::string text;
            char        number[32];
            switch (value->type)
            {
            case WXL_SETTING_BOOLEAN: text = value->boolean ? "true" : "false"; break;
            case WXL_SETTING_NUMBER:
                std::snprintf(number, sizeof number, "%.17g", value->number);
                text = number;
                break;
            case WXL_SETTING_STRING: text = value->string ? value->string : ""; break;
            default: return 0;
            }
            return Shared().Set(s->ext, key, text) == WXL_OK;
        }

        LuaSet& SetOf(const std::string& ext)
        {
            for (LuaSet& existing : Sets())
                if (existing.ext == ext) return existing;
            LuaSet& made = Sets().emplace_back();
            made.ext     = ext;
            return made;
        }

        /// WXL_Command(text): the lines to show, as the function's results.
        int __cdecl LuaCommand(void* state)
        {
            const char* text = gs::ArgCount(state) >= 1 && gs::IsString(state, 1) ? gs::ToString(state, 1) : "";
            const std::vector<std::string> lines = Shared().Command(text ? text : "");
            for (const std::string& line : lines) gs::PushString(state, line.c_str());
            return int(lines.size());
        }

        constexpr const char* kSlash =
            "if SlashCmdList then SLASH_WXL1 = '/wxl'; SlashCmdList['WXL'] = function(msg) "
            "for _, line in ipairs({ WXL_Command(msg) }) do DEFAULT_CHAT_FRAME:AddMessage('|cff66bbffwxl|r ' .. line) end "
            "end end";
    }

    void PublishSet(const std::string& ext)
    {
        LuaSet& s = SetOf(ext);
        s.keys.clear();
        s.keyPtrs.clear();
        for (const Setting& setting : Shared().All().at(ext)) s.keys.push_back(setting.key);
        for (const std::string& key : s.keys) s.keyPtrs.push_back(key.c_str());
        s.set            = WXL_SettingsSet{};
        s.set.structSize = sizeof(WXL_SettingsSet);
        s.set.name       = s.ext.c_str();
        s.set.user       = &s;
        s.set.keys       = s.keyPtrs.data();
        s.set.keyCount   = uint32_t(s.keyPtrs.size());
        s.set.Get        = &SetGet;
        s.set.Set        = &SetSet;
        script::AddSettings(&s.set);
    }

    void InstallInterface()
    {
        script::AddFunction("WXL_Command", &LuaCommand);
        script::AddChunk("WarcraftXL /wxl", kSlash);
    }
}
