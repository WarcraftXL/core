// SPDX-License-Identifier: GPL-3.0-or-later
// A profile file: '#' comments, a [set] line per settings set, then key=value lines (true/false, a number, text).

#include "engine/script/Profiles.hpp"

#include "engine/script/LuaSettings.hpp"

#include "common/Config.hpp"
#include "common/Log.hpp"
#include "runtime/extensions/Services.hpp"
#include "wxl/abi/Profiles.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr const char* kDir           = "WTF\\WarcraftXL\\profiles";
    constexpr size_t      kMaxNameLength = 48;
    constexpr size_t      kMaxLine       = 512;
    constexpr size_t      kExtensionLength = 4; ///< ".cfg"

    std::string g_current;

    std::string PathOf(const char* name)
    {
        return std::string(kDir) + "\\" + name + ".cfg";
    }

    void Trim(std::string& s)
    {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        s.erase(0, i);
    }

    void Write(std::FILE* f, const char* key, const WXL_SettingValue& v)
    {
        switch (v.type)
        {
            case WXL_SETTING_BOOLEAN: std::fprintf(f, "%s=%s\r\n", key, v.boolean ? "true" : "false"); break;
            case WXL_SETTING_NUMBER:  std::fprintf(f, "%s=%.6g\r\n", key, v.number); break;
            case WXL_SETTING_STRING:  std::fprintf(f, "%s=%s\r\n", key, v.string ? v.string : ""); break;
            default: break;
        }
    }

    /// Text back to a value: true / false, a number, else text.
    WXL_SettingValue Parse(const std::string& text)
    {
        WXL_SettingValue v = {};
        if (_stricmp(text.c_str(), "true") == 0 || _stricmp(text.c_str(), "false") == 0)
        {
            v.type = WXL_SETTING_BOOLEAN;
            v.boolean = _stricmp(text.c_str(), "true") == 0 ? 1 : 0;
            return v;
        }
        char* end = nullptr;
        const double n = std::strtod(text.c_str(), &end);
        if (end != text.c_str() && *end == '\0')
        {
            v.type = WXL_SETTING_NUMBER;
            v.number = n;
            return v;
        }
        v.type = WXL_SETTING_STRING;
        v.string = text.c_str();
        return v;
    }
}

namespace wxl::script::profiles
{
    bool ValidName(const char* name)
    {
        if (!name || !*name || name[0] == '.') return false;
        const size_t n = std::strlen(name);
        if (n > kMaxNameLength) return false;
        for (size_t i = 0; i < n; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(name[i]);
            if (!(std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.')) return false;
        }
        return name[n - 1] != ' ' && name[n - 1] != '.';
    }

    bool Save(const char* name)
    {
        if (!ValidName(name)) return false;
        CreateDirectoryA("WTF", nullptr);
        CreateDirectoryA("WTF\\WarcraftXL", nullptr);
        CreateDirectoryA(kDir, nullptr);
        std::FILE* f = nullptr;
        if (fopen_s(&f, PathOf(name).c_str(), "wb") != 0 || !f)
        {
            WLOG_WARN("profiles: %s could not be written", PathOf(name).c_str());
            return false;
        }
        std::fprintf(f, "# WarcraftXL profile \"%s\", written by the game.\r\n", name);
        uint32_t keys = 0;
        for (uint32_t s = 0; s < SettingsCount(); ++s)
        {
            const WXL_SettingsSet* set = SettingsAt(s);
            std::fprintf(f, "\r\n[%s]\r\n", set->name);
            for (uint32_t k = 0; k < set->keyCount; ++k)
            {
                WXL_SettingValue v = {};
                if (!set->keys[k] || !set->Get(set->user, set->keys[k], &v)) continue;
                Write(f, set->keys[k], v);
                ++keys;
            }
        }
        std::fclose(f);
        g_current = name;
        WLOG_INFO("profiles: \"%s\" saved (%u set(s), %u value(s))", name, SettingsCount(), keys);
        return true;
    }

    bool Load(const char* name)
    {
        if (!ValidName(name)) return false;
        std::FILE* f = nullptr;
        if (fopen_s(&f, PathOf(name).c_str(), "rb") != 0 || !f) return false;
        const WXL_SettingsSet* set = nullptr;
        uint32_t applied = 0, skipped = 0;
        char line[kMaxLine];
        while (std::fgets(line, sizeof line, f))
        {
            std::string s(line);
            Trim(s);
            if (s.empty() || s[0] == '#' || s[0] == ';') continue;
            if (s.front() == '[' && s.back() == ']')
            {
                const std::string setName = s.substr(1, s.size() - 2);
                set = nullptr;
                for (uint32_t i = 0; i < SettingsCount() && !set; ++i)
                    if (std::strcmp(SettingsAt(i)->name, setName.c_str()) == 0) set = SettingsAt(i);
                continue;
            }
            const size_t eq = s.find('=');
            if (!set || !set->Set || eq == std::string::npos)
            {
                ++skipped;
                continue;
            }
            std::string key = s.substr(0, eq), value = s.substr(eq + 1);
            Trim(key);
            Trim(value);
            const WXL_SettingValue v = Parse(value);
            if (set->Set(set->user, key.c_str(), &v)) ++applied;
            else ++skipped;
        }
        std::fclose(f);
        g_current = name;
        WLOG_INFO("profiles: \"%s\" loaded (%u value(s) applied, %u skipped)", name, applied, skipped);
        return true;
    }

    bool Delete(const char* name)
    {
        if (!ValidName(name)) return false;
        const bool ok = DeleteFileA(PathOf(name).c_str()) != 0;
        if (ok && g_current == name) g_current.clear();
        return ok;
    }

    std::vector<std::string> List()
    {
        std::vector<std::string> out;
        WIN32_FIND_DATAA data;
        const HANDLE h = FindFirstFileA((std::string(kDir) + "\\*.cfg").c_str(), &data);
        if (h == INVALID_HANDLE_VALUE) return out;
        do
        {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string n(data.cFileName);
            if (n.size() > kExtensionLength) n.resize(n.size() - kExtensionLength);
            if (ValidName(n.c_str())) out.push_back(n);
        } while (FindNextFileA(h, &data));
        FindClose(h);
        std::sort(out.begin(), out.end());
        return out;
    }

    const char* Current() { return g_current.c_str(); }

    void LoadStartupOnce()
    {
        static bool done = false;
        if (done) return;
        done = true;
        char name[kMaxNameLength + 1] = {};
        if (!wxl::config::Raw("WXL_PROFILE", name, sizeof name)) return;
        if (!Load(name)) WLOG_WARN("profiles: WXL_PROFILE=%s could not be loaded", name);
    }

    namespace
    {
        bool Exists(const char* name)
        {
            return ValidName(name) && GetFileAttributesA(PathOf(name).c_str()) != INVALID_FILE_ATTRIBUTES;
        }

        int __cdecl ApiValidName(const char* name) { return ValidName(name) ? WXL_OK : WXL_E_INVALID_ARGUMENT; }
        int __cdecl ApiSave(const char* name) { return Save(name) ? WXL_OK : WXL_E_INVALID_ARGUMENT; }

        int __cdecl ApiLoad(const char* name)
        {
            if (!ValidName(name)) return WXL_E_INVALID_ARGUMENT;
            if (!Exists(name)) return WXL_E_NOT_FOUND;
            return Load(name) ? WXL_OK : WXL_E_NOT_FOUND;
        }

        int __cdecl ApiDelete(const char* name)
        {
            if (!ValidName(name)) return WXL_E_INVALID_ARGUMENT;
            return Delete(name) ? WXL_OK : WXL_E_NOT_FOUND;
        }

        int __cdecl ApiList(WXL_ProfileVisitFn fn, void* user)
        {
            if (!fn) return WXL_E_INVALID_ARGUMENT;
            for (const std::string& name : List()) fn(user, name.c_str());
            return WXL_OK;
        }

        const char* __cdecl ApiCurrent() { return Current(); }

        const WXL_ProfilesApi g_api = { sizeof(WXL_ProfilesApi), WXL_PROFILES_API_VERSION, &ApiValidName, &ApiSave,
                                        &ApiLoad, &ApiDelete, &ApiList, &ApiCurrent };
    }

    void Publish()
    {
        wxl::runtime::extensions::PublishInterface(WXL_PROFILES_API_NAME, WXL_PROFILES_API_VERSION,
                                                   const_cast<WXL_ProfilesApi*>(&g_api));
    }
}
