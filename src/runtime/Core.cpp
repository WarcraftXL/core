// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The core runtime: paths, the log sink, and the mod host.

#include "runtime/Core.hpp"

#include "runtime/Stages.hpp"

#include "common/Log.hpp"
#include "objects/Objects.hpp"
#include "points/Points.hpp"
#include "profile/Profile.hpp"
#include "runtime/Attachment.hpp"
#include "runtime/Code.hpp"
#include "runtime/Listening.hpp"
#include "runtime/Session.hpp"
#include "services/DataService.hpp"
#include "services/M2Service.hpp"
#include "services/FootprintService.hpp"
#include "services/ScriptService.hpp"
#include "services/Assets.hpp"
#include "services/CameraService.hpp"
#include "services/InputService.hpp"
#include "services/AnimationService.hpp"
#include "services/PoseService.hpp"
#include "services/HighlightService.hpp"
#include "services/RenderService.hpp"
#include "services/SelectionCircleService.hpp"
#include "services/CVarService.hpp"
#include "services/TerrainService.hpp"
#include "services/ShaderCode.hpp"
#include "services/TerrainLayers.hpp"
#include "services/DrawService.hpp"
#include "services/StorageService.hpp"
#include "services/Hashes.hpp"
#include "services/RelayService.hpp"

#include "wxl/core/init.h"
#include "wxl/host/mod_host.hpp"

#include <windows.h>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>

namespace
{
    constexpr wchar_t kModsRelative[] = L"Mods";

    /**
     * @brief Writes the folder of the process executable into buf, with a trailing backslash.
     * @param buf  receives the NUL-terminated path.
     * @param cap  capacity of buf in characters.
     * @return Length of the written path, or 0 when it does not fit or cannot be read.
     */
    size_t ExeDirectory(wchar_t* buf, size_t cap)
    {
        const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(cap));
        if (n == 0 || n >= cap) return 0;
        for (size_t i = n; i-- > 0;)
        {
            if (buf[i] == L'\\' || buf[i] == L'/')
            {
                buf[i + 1] = L'\0';
                return i + 1;
            }
        }
        return 0;
    }

    wxl::log::Level ToCoreLevel(wxl_log_level level)
    {
        switch (level)
        {
        case WXL_LOG_TRACE: return wxl::log::Level::Trace;
        case WXL_LOG_DEBUG: return wxl::log::Level::Debug;
        case WXL_LOG_INFO:  return wxl::log::Level::Info;
        case WXL_LOG_WARN:  return wxl::log::Level::Warn;
        case WXL_LOG_ERROR: return wxl::log::Level::Error;
        }
        return wxl::log::Level::Info;
    }

    wxl_log_level ToHostLevel(wxl::log::Level level)
    {
        switch (level)
        {
        case wxl::log::Level::Trace: return WXL_LOG_TRACE;
        case wxl::log::Level::Debug: return WXL_LOG_DEBUG;
        case wxl::log::Level::Info:  return WXL_LOG_INFO;
        case wxl::log::Level::Warn:  return WXL_LOG_WARN;
        default:                     return WXL_LOG_ERROR;
        }
    }

    /// Routes the host's and every mod's lines into the core log. Never throws.
    class CoreSink final : public wxl::host::LogSink
    {
    public:
        void Write(wxl_log_level level, std::string_view tag, std::string_view message) override
        {
            wxl::log::WriteLine(ToCoreLevel(level), tag, message);
        }
    };

    CoreSink g_sink;

    /// The host disabled a mod: the script functions it registered are withdrawn.
    void OnModDisabled(void*, wxl::host::ModHost::ModToken token, const char*)
    {
        wxl::core::services::script::OnModDisabled(token);
        // Its point handlers: on x86 jumps into its code, on both calls into it.
        wxl::core::points::ReleaseOf(token);
        // Its code is not trusted to run again, so neither is anything that jumps into it,
        // and nothing it was holding is going to be given back by it.
        wxl::core::attachment::Release(token);
        wxl::core::services::storage::CloseFilesOf(token);
        wxl::core::services::storage::RemoveSearchPathsOf(token);
        wxl::core::services::storage::RemoveRedirectsOf(token);
        // Its providers, served names and transforms: no call into its code starts again.
        wxl::core::services::assets::ReleaseOf(token);
        // Its overrides, its hashes in flight, its relayed services and requests.
        wxl::core::services::storage::RemoveOverridesOf(token);
        wxl::core::services::hashes::ForgetMod(token);
        wxl::core::services::relay::ForgetMod(token);
        wxl::core::services::data::ForgetMod(token);
        // The models it made come off what they hang on and are given back.
        wxl::core::services::m2::ForgetMod(token);
        // What it reported of its memory, and the threshold it watched, go too.
        wxl::core::services::footprint::ForgetMod(token);
        // Its watches go with it: the client stops reporting what no other mod watches.
        wxl::core::objects::Service().DropWatches(token);
        // The camera goes back to the client, the pointer is let go.
        wxl::core::services::camera::ForgetMod(token);
        wxl::core::services::input::ForgetMod(token);
        // Its heads go back to their animations, its highlights come off, the game's circles
        // get back the look it changed.
        wxl::core::services::pose::ForgetMod(token);
        wxl::core::services::highlight::ForgetMod(token);
        wxl::core::services::render::ForgetMod(token);
        wxl::core::services::selectioncircle::ForgetMod(token);
        // Its console variables are released; they stay, with their values.
        wxl::core::services::cvar::ForgetMod(token);
        // Its edits of the ground come off; another mod's, or the original, show again.
        wxl::core::services::terrain::ForgetMod(token);
        // Its layer provider goes: the chunks loaded from now on are drawn as the client draws them.
        wxl::core::services::terrain_layers::ForgetMod(token);
        wxl::core::services::shader_code::ForgetMod(token);
        // Its shapes go.
        wxl::core::services::draw::ForgetMod(token);
        // Its animations on units end; the game chooses theirs again.
        wxl::core::services::animation::ForgetMod(token);
    }

    /// An event gained its first handler or lost its last: the flag the client tests before a
    /// busy seam that raises it (runtime/Listening.hpp).
    void OnListenChanged(void*, wxl_event event, bool listening) { wxl::core::listening::ModsListen(event, listening); }

    // Heap-allocated and never freed: the wxl_host table points into it, and a mod disabled
    // rather than unloaded stays mapped with that pointer to the end of the process.
    wxl::host::ModHost* g_host = nullptr;

    LONG g_started = 0;
    bool g_up      = false;
}

extern "C" WXL_CORE_API volatile uint8_t wxl_core_listening[WXL_EV_COUNT] = {};

namespace wxl::core
{
    bool OpenLog() { return session::Begin(); }

    wxl::host::ModHost* Host() { return g_host; }

    bool Start()
    {
        if (InterlockedCompareExchange(&g_started, 1, 0) != 0)
        {
            WLOG_WARN(kTag, "Start called twice; the second call is ignored");
            return g_up;
        }

        try
        {
            wchar_t dir[1024];
            if (ExeDirectory(dir, sizeof dir / sizeof dir[0]) == 0)
            {
                WLOG_ERROR(kTag, "cannot resolve the client folder; no mods loaded");
                return false;
            }

            attachment::Prepare();
            // First: it calibrates its clock on a thread of its own while the mods load.
            profile::Start(settings::ResolveProfile(session::CurrentSettings()));

            wxl::host::HostConfig cfg;
            cfg.mods_directory  = std::filesystem::path(dir) / kModsRelative;
            cfg.runtime_id      = kRuntimeId;
            cfg.runtime_name    = kRuntimeName;
            cfg.runtime_version = WXL_CORE_VERSION;
            cfg.log_sink        = &g_sink;
            cfg.log_minimum     = ToHostLevel(wxl::log::MinLevel());
            cfg.events          = attachment::Events();
            cfg.services        = attachment::Services();
            // Every binary the host maps is code the client may be handed, as is the runtime's own.
            cfg.loader          = &code::HostLoader();
            code::AddOwnImage();

            WLOG_INFO(kTag, "runtime %s (%s) version %u, abi %u.%u.%u, target %s, mods in %ls",
                      kRuntimeId, kRuntimeName, static_cast<unsigned>(WXL_CORE_VERSION),
                      static_cast<unsigned>(WXL_ABI_VERSION_MAJOR),
                      static_cast<unsigned>(WXL_ABI_VERSION_MINOR),
                      static_cast<unsigned>(WXL_ABI_VERSION_FEATURE), cfg.target.c_str(),
                      cfg.mods_directory.c_str());
            for (const wxl::host::PublishedService& service : cfg.services)
                WLOG_INFO(kTag, "service %s version %u", service.name.c_str(), service.version);

            g_host = new wxl::host::ModHost(std::move(cfg));
            g_host->SetDisableHandler(&OnModDisabled, nullptr);
            profile::Attach(*g_host);
            g_host->SetListenHandler(&OnListenChanged, nullptr);

            attachment::Install();
            points::Describe();

            g_host->LoadAll();
            stages::Restart();
            stages::Reach(WXL_STAGE_EARLY);

            session::ReportMods(*g_host);
            WLOG_INFO(kTag, "core ready: %zu mod(s) loaded", g_host->Loaded().size());
            g_up = true;
        }
        catch (const std::exception& e)
        {
            WLOG_ERROR(kTag, "mod host failed: %s", e.what());
        }
        catch (...)
        {
            WLOG_ERROR(kTag, "mod host failed: unknown exception");
        }
        return g_up;
    }
}
