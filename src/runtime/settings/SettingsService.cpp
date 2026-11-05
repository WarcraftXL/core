// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/settings/SettingsService.hpp"

#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "runtime/extensions/Services.hpp"
#include "runtime/Owners.hpp"
#include "runtime/settings/Store.hpp"

#include <windows.h>

#include <cstring>
#include <fstream>
#include <sstream>

namespace wxl::runtime::settings
{
    namespace
    {
        /// The folder of the extension named `ext`.
        std::string FolderOf(std::string_view ext)
        {
            for (owners::Id id = 1;; ++id)
            {
                const char* name = owners::Name(id);
                if (std::strcmp(name, "?") == 0) break;
                if (ext == name) return owners::Folder(id);
            }
            return std::string(ext);
        }

        std::string PathOf(std::string_view ext)
        {
            const std::string folder = FolderOf(ext);
            return "Extensions\\" + folder + "\\" + folder + ".ini";
        }

        bool ReadFile(std::string_view ext, std::string& text)
        {
            std::ifstream file(PathOf(ext), std::ios::binary);
            if (!file) return false;
            std::stringstream content;
            content << file.rdbuf();
            text = content.str();
            return true;
        }

        /// Written aside, then moved over: a crash mid-write never leaves half a file.
        bool WriteFile(std::string_view ext, const std::string& text)
        {
            const std::string path = PathOf(ext), temp = path + ".new";
            {
                std::ofstream file(temp, std::ios::binary | std::ios::trunc);
                if (!file) return false;
                file << text;
                if (!file) return false;
            }
            return MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        }

        void LogLine(std::string_view ext, int level, const std::string& line)
        {
            log::Write(log::Level(level), "%.*s: %s", int(ext.size()), ext.data(), line.c_str());
        }

        void Changed(const std::string& ext, const std::string& key, const std::string& value)
        {
            const events::SettingChangedArgs args{ ext.c_str(), key.c_str(), value.c_str() };
            events::Emit<events::Event::OnSettingChanged>(args);
        }

        const char* CallerName() { return owners::Name(owners::Current()); }

        int __cdecl ApiDeclare(const WXL_SettingDecl* decls, uint32_t count)
        {
            const std::string ext    = CallerName();
            const int         status = Shared().Declare(ext, decls, count);
            if (status == WXL_OK) PublishSet(ext);
            return status;
        }

        int __cdecl ApiGet(const char* key, char* buffer, uint32_t capacity, uint32_t* length)
        {
            if (!key || !length) return WXL_E_INVALID_ARGUMENT;
            std::string value;
            if (const int status = Shared().Get(CallerName(), key, value)) return status;
            *length = uint32_t(value.size());
            if (!buffer || capacity <= value.size()) return WXL_E_INVALID_ARGUMENT;
            std::memcpy(buffer, value.c_str(), value.size() + 1);
            return WXL_OK;
        }

        int __cdecl ApiSet(const char* key, const char* value)
        {
            if (!key || !value) return WXL_E_INVALID_ARGUMENT;
            return Shared().Set(CallerName(), key, value);
        }

        int __cdecl ApiVisit(WXL_SettingVisitFn fn, void* user)
        {
            if (!fn) return WXL_E_INVALID_ARGUMENT;
            Shared().Visit(fn, user);
            return WXL_OK;
        }

        int __cdecl ApiSetOf(const char* extension, const char* key, const char* value)
        {
            if (!extension || !key || !value) return WXL_E_INVALID_ARGUMENT;
            return Shared().Set(extension, key, value);
        }

        const WXL_SettingsApi g_api = { sizeof(WXL_SettingsApi), WXL_SETTINGS_API_VERSION, &ApiDeclare, &ApiGet,
                                        &ApiSet, &ApiVisit, &ApiSetOf };
    }

    Store& Shared()
    {
        static Store store(Store::Hooks{ &ReadFile, &WriteFile, &PathOf, &LogLine, &Changed });
        return store;
    }

    void Publish()
    {
        extensions::PublishInterface(WXL_SETTINGS_API_NAME, WXL_SETTINGS_API_VERSION, const_cast<WXL_SettingsApi*>(&g_api));
    }
}
