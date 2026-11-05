// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/extensions/Extensions.hpp"

#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/patch/Patch.hpp"
#include "engine/script/Profiles.hpp"
#include "engine/storage/StorageHook.hpp"
#include "runtime/HookPoints.hpp"
#include "runtime/Owners.hpp"
#include "runtime/extensions/Api.hpp"
#include "runtime/extensions/Loader.hpp"
#include "runtime/extensions/Manifest.hpp"
#include "runtime/extensions/Services.hpp"
#include "runtime/objects/Objects.hpp"
#include "runtime/settings/SettingsService.hpp"
#include "wxl/game/Boot.hpp"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

namespace wxl::runtime::extensions
{
    namespace
    {
        /// The loader's place in the chains it shares: ahead of every other party.
        constexpr int kLoaderPriority = -0x40000000;

        int g_loaded = 0; ///< extensions loaded, both stages

        /// Opens, checks and initialises one extension; false when it was refused or declined.
        bool LoadOne(const char* folder)
        {
            loader::Opened ext;
            if (!loader::Open(folder, std::string("Extensions\\") + folder, ext)) return false;

            const owners::Id id = owners::Add(ext.name, folder);
            if (id == owners::kCore)
            {
                WLOG_ERROR("extensions: '%s' refused: too many extensions", ext.name);
                return false;
            }
            if (ext.lowLevel) owners::GrantLowLevel(id);
            int loaded = 0;
            if (!owners::Run(id, "WXL_Load", [&] { loaded = ext.load(&Api()); })) return false;
            if (!loaded)
            {
                // Left loaded, its code no longer called: a detour it attached may be on a stack.
                owners::Disable(id, "it declined to initialise (WXL_Load returned 0)");
                return false;
            }
            WLOG_INFO("extensions: loaded '%s' %s", ext.name, ext.version.empty() ? "(no version)" : ext.version.c_str());
            return true;
        }

        /// The folders under Extensions\, sorted: load order must not depend on the file system.
        std::vector<std::string> Folders()
        {
            std::vector<std::string> folders;
            WIN32_FIND_DATAA         entry{};
            HANDLE                   search = FindFirstFileA("Extensions\\*", &entry);
            if (search == INVALID_HANDLE_VALUE) return folders;
            do
            {
                if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || entry.cFileName[0] == '.') continue;
                folders.emplace_back(entry.cFileName);
            } while (FindNextFileA(search, &entry));
            FindClose(search);
            std::sort(folders.begin(), folders.end());
            return folders;
        }

        /// Loads the extensions of one stage. A folder without its DLL is reported at the first stage;
        /// a manifest that cannot be read goes to the engine stage, where the loader refuses it.
        void LoadStage(manifest::Stage stage)
        {
            const bool first  = stage == manifest::Stage::BeforeArchives;
            int        loaded = 0;
            for (const std::string& folder : Folders())
            {
                const std::string dir = "Extensions\\" + folder;
                if (GetFileAttributesA((dir + "\\" + folder + ".dll").c_str()) == INVALID_FILE_ATTRIBUTES)
                {
                    if (first) WLOG_WARN("extensions: '%s' holds no %s.dll", folder.c_str(), folder.c_str());
                    continue;
                }
                const manifest::Manifest m  = manifest::Read(dir + "\\wxl.json");
                const manifest::Stage    at = m.status == manifest::Status::Ok ? m.stage : manifest::Stage::Engine;
                if (at == stage && LoadOne(folder.c_str())) ++loaded;
            }
            g_loaded += loaded;
            if (loaded) WLOG_INFO("extensions: %d loaded %s", loaded, first ? "before the archives" : "at engine start");
        }

        game::boot::EngineInitFn g_origEngineInit = nullptr;

        using InitializeWowConfigFn = void(__cdecl*)();
        InitializeWowConfigFn g_origInitializeWowConfig = nullptr;

        /// The archives are about to be mounted: the before-archives stage loads, and what it attaches
        /// to this function runs in this same call.
        void __cdecl InitializeWowConfigDetour()
        {
            static bool done = false;
            if (!done)
            {
                done = true;
                LoadStage(manifest::Stage::BeforeArchives);
                hook::EnableAll();
            }
            g_origInitializeWowConfig();
        }

        uint32_t __cdecl EngineInitDetour()
        {
            EnsureLoaded();
            return g_origEngineInit();
        }
    }

    void EnsureLoaded()
    {
        static bool loaded = false;
        if (loaded) return;
        loaded = true;
        LoadStage(manifest::Stage::Engine);
        hook::EnableAll();
        const events::ExtensionsLoadedArgs args{ uint32_t(g_loaded) };
        events::Emit<events::Event::OnExtensionsLoaded>(args);
    }

    bool InstallLoader()
    {
        owners::OnDisable("subscriptions", &events::DropOwner);
        owners::OnDisable("hooks", &hook::DropOwner, &hook::Resident);
        owners::OnDisable("patches", &patch::DropOwner);
        owners::OnDisable("services", &WithdrawServices);
        storage::Publish();
        objects::Publish();
        settings::Publish();
        script::profiles::Publish();

        const bool early = hookpoints::Attach("OpenArchives", &InitializeWowConfigDetour, &g_origInitializeWowConfig,
                                              kLoaderPriority);
        return hookpoints::Attach("InitializeEngineCallback", &EngineInitDetour, &g_origEngineInit) && early;
    }
}
