// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The core runtime: paths, the log sink, and the mod host.

#include "runtime/Core.hpp"

#include "runtime/Stages.hpp"

#include "common/Log.hpp"
#include "runtime/Attachment.hpp"
#include "services/DataService.hpp"
#include "services/GxService.hpp"
#include "services/M2FileService.hpp"
#include "services/M2Service.hpp"
#include "services/ScriptService.hpp"
#include "services/StorageService.hpp"

#include "wxl/host/mod_host.hpp"

#include <windows.h>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>

namespace
{
    constexpr wchar_t kLogRelative[]  = L"Logs\\WarcraftXL.log";
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
        // Its code is not trusted to run again, so neither is anything that jumps into it,
        // and nothing it was holding is going to be given back by it.
        wxl::core::attachment::Release(token);
        wxl::core::services::storage::CloseFilesOf(token);
        wxl::core::services::data::ForgetMod(token);
        wxl::core::services::gx::ForgetMod(token);
        wxl::core::services::m2::ForgetMod(token);
        wxl::core::services::m2file::ForgetMod(token);
    }

    // Heap-allocated and never freed: the wxl_host table points into it, and a mod disabled
    // rather than unloaded stays mapped with that pointer to the end of the process.
    wxl::host::ModHost* g_host = nullptr;

    LONG g_started = 0;
    bool g_up      = false;
}

namespace wxl::core
{
    bool OpenLog()
    {
        wchar_t path[1024];
        const size_t dir = ExeDirectory(path, sizeof path / sizeof path[0]);
        if (dir == 0) return false;
        if (wcscat_s(path, kLogRelative) != 0) return false;
        return wxl::log::Open(path);
    }

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

            wxl::host::HostConfig cfg;
            cfg.mods_directory  = std::filesystem::path(dir) / kModsRelative;
            cfg.runtime_id      = kRuntimeId;
            cfg.runtime_name    = kRuntimeName;
            cfg.runtime_version = WXL_CORE_VERSION;
            cfg.log_sink        = &g_sink;
            cfg.log_minimum     = ToHostLevel(wxl::log::MinLevel());
            cfg.events          = attachment::Events();
            cfg.services        = attachment::Services();

            WLOG_INFO(kTag, "runtime %s (%s) version %u, abi %u.%u, target %s, mods in %ls",
                      kRuntimeId, kRuntimeName, static_cast<unsigned>(WXL_CORE_VERSION),
                      static_cast<unsigned>(WXL_ABI_VERSION_MAJOR),
                      static_cast<unsigned>(WXL_ABI_VERSION_MINOR), cfg.target.c_str(),
                      cfg.mods_directory.c_str());
            for (const wxl::host::PublishedService& service : cfg.services)
                WLOG_INFO(kTag, "service %s version %u", service.name.c_str(), service.version);

            g_host = new wxl::host::ModHost(std::move(cfg));
            g_host->SetDisableHandler(&OnModDisabled, nullptr);

            attachment::Install();

            g_host->LoadAll();
            stages::Restart();
            stages::Reach(WXL_STAGE_EARLY);

            for (const wxl::host::LoadedMod& mod : g_host->Loaded())
            {
                if (mod.disabled)
                    WLOG_WARN(kTag, "mod %s %s (%s) is disabled: it faulted %s", mod.id.c_str(),
                              mod.manifest.version.text.c_str(), mod.name.c_str(), mod.fault.c_str());
                else
                    WLOG_INFO(kTag, "mod %s %s (%s) is up", mod.id.c_str(),
                              mod.manifest.version.text.c_str(), mod.name.c_str());
            }
            for (const wxl::host::RefusedMod& mod : g_host->Refused())
                WLOG_INFO(kTag, "mod %s was not loaded: %s",
                          mod.id.empty() ? mod.directory_name.c_str() : mod.id.c_str(),
                          mod.reason.c_str());
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
