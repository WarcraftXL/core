// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The launch's header, its mods and its end, as Logs\WarcraftXL.log tells them.

#include "runtime/Session.hpp"

#include "common/Log.hpp"
#include "runtime/Settings.hpp"
#include "runtime/Stamp.hpp"

#include "wxl/abi.h"
#include "wxl/host/mod_host.hpp"
#include "wxl_build_ids.h"

#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace
{
    using wxl::log::Level;

    constexpr const char* kTag = wxl::core::session::kTag;

    ULONGLONG                       g_began = 0;
    LONG                            g_ended = 0;
    wxl::core::settings::Settings   g_settings;

    /// The executable's folder, with a trailing backslash; 0 when it cannot be read.
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

    /// @p text as UTF-8 into @p out; an empty string when it does not convert.
    const char* Utf8(const wchar_t* text, char* out, size_t capacity)
    {
        if (capacity == 0) return out;
        out[0] = '\0';
        if (text && WideCharToMultiByte(CP_UTF8, 0, text, -1, out, static_cast<int>(capacity), nullptr, nullptr) == 0)
            out[0] = '\0';
        out[capacity - 1] = '\0';
        return out;
    }

    void Line(const char* fmt, ...)
    {
        char    body[1536];
        va_list args;
        va_start(args, fmt);
        const int n = vsnprintf(body, sizeof body, fmt, args);
        va_end(args);
        if (n < 0) return;
        const size_t len = static_cast<size_t>(n) < sizeof body ? static_cast<size_t>(n) : sizeof body - 1;
        wxl::log::WriteUnfiltered(Level::Info, kTag, std::string_view(body, len));
    }

    /// An export of ntdll by name, as the function type it has.
    template <class Fn>
    Fn Export(HMODULE module, const char* name)
    {
        return module ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name))) : nullptr;
    }

    const IMAGE_NT_HEADERS* ExeHeaders()
    {
        const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        if (!base) return nullptr;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
    }

#if defined(_M_IX86)
    /// FNV-1a, 64 bits: the same number tools/read_logs.py and a Python one-liner compute.
    uint64_t Fnv1a(const uint8_t* data, size_t size)
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 0x100000001B3ull;
        }
        return hash;
    }
#endif

    void Executable()
    {
        wchar_t path[1024];
        const DWORD length = GetModuleFileNameW(nullptr, path, 1024);
        char        utf8[3072];
        Utf8(length > 0 && length < 1024 ? path : L"?", utf8, sizeof utf8);

        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        unsigned long long        size = 0;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
            size = (static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;

        const IMAGE_NT_HEADERS* nt = ExeHeaders();
        if (!nt)
        {
            Line("exe %s, %llu bytes, no PE headers in memory", utf8, size);
            return;
        }
        const bool large = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
        Line("exe %s, %llu bytes, PE stamp 0x%08lX, %s, %s", utf8, size,
             static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
             nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 ? "x86" : "x64",
             large ? "large-address-aware" : "not large-address-aware (2 GB of addresses at most)");

#if defined(_M_IX86)
        // The stock client: its code, as mapped, against the stock 12340's. Read before any
        // detour is written, so a difference is the file's, or something loaded before us.
        constexpr DWORD    kStockStamp    = 0x4C2452FE;
        constexpr uint64_t kStockText     = 0xAE7982DD29DB4D5Aull;
        constexpr DWORD    kStockTextSize = 0x5DD3B3;
        const auto*        base           = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            if (std::memcmp(section[i].Name, ".text", 6) != 0) continue;
            const DWORD    bytes = section[i].Misc.VirtualSize < section[i].SizeOfRawData ? section[i].Misc.VirtualSize
                                                                                            : section[i].SizeOfRawData;
            const uint64_t hash  = Fnv1a(base + section[i].VirtualAddress, bytes);
            const bool     stock = nt->FileHeader.TimeDateStamp == kStockStamp && bytes == kStockTextSize &&
                               hash == kStockText;
            Line("exe .text %lu bytes, fnv1a64 0x%016llX: %s", static_cast<unsigned long>(bytes),
                 static_cast<unsigned long long>(hash),
                 stock ? "the stock 3.3.5a 12340 code, unmodified"
                 : nt->FileHeader.TimeDateStamp == kStockStamp
                     ? "build 12340's stamp, but its code differs from the stock client's (patched in the file, or by "
                       "something loaded before WarcraftXL.dll)"
                     : "not the stock 3.3.5a 12340 client");
            break;
        }
#endif
    }

    void System()
    {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof version;
        if (auto get = Export<RtlGetVersionFn>(ntdll, "RtlGetVersion"))
            get(&version);
        char servicePack[256];
        Utf8(version.szCSDVersion, servicePack, sizeof servicePack);

        BOOL wow64 = FALSE;
        IsWow64Process(GetCurrentProcess(), &wow64);

        // Wine, and so Proton, says so through ntdll's own exports.
        using WineVersionFn = const char*(CDECL*)(void);
        using WineHostFn    = void(CDECL*)(const char**, const char**);
        auto wineVersion = Export<WineVersionFn>(ntdll, "wine_get_version");
        auto wineBuild   = Export<WineVersionFn>(ntdll, "wine_get_build_id");
        auto wineHost    = Export<WineHostFn>(ntdll, "wine_get_host_version");

        Line("windows %lu.%lu build %lu%s%s, %s process%s", version.dwMajorVersion, version.dwMinorVersion,
             version.dwBuildNumber, servicePack[0] ? " " : "", servicePack,
             sizeof(void*) == 4 ? "32-bit" : "64-bit", wow64 ? " on 64-bit Windows" : "");
        if (wineVersion)
        {
            const char* sysname = nullptr;
            const char* release = nullptr;
            if (wineHost) wineHost(&sysname, &release);
            Line("wine %s (%s) on %s %s", wineVersion(), wineBuild ? wineBuild() : "?", sysname ? sysname : "?",
                 release ? release : "?");

            // What the player's launcher set for Wine and the graphics layers, when it set it.
            static const char* const kNames[] = {"WINEDLLOVERRIDES", "WINEESYNC", "WINEFSYNC", "PROTON_USE_WINED3D",
                                                 "PROTON_NO_ESYNC", "PROTON_NO_FSYNC", "DXVK_ASYNC", "DXVK_HUD",
                                                 "SteamAppId", "SteamGameId"};
            char  list[1024];
            size_t used = 0;
            list[0]     = '\0';
            for (const char* name : kNames)
            {
                char        value[256];
                const DWORD n = GetEnvironmentVariableA(name, value, sizeof value);
                if (n == 0 || n >= sizeof value) continue;
                const int wrote = std::snprintf(list + used, sizeof list - used, "%s%s=%s", used ? ", " : "", name, value);
                if (wrote > 0 && static_cast<size_t>(wrote) < sizeof list - used) used += static_cast<size_t>(wrote);
            }
            if (used) Line("wine environment: %s", list);
        }
        else
        {
            Line("not under Wine");
        }

        int  info[4] = {};
        char brand[49] = {};
        __cpuid(info, 0x80000000);
        if (static_cast<unsigned>(info[0]) >= 0x80000004u)
        {
            for (int i = 0; i < 3; ++i)
            {
                __cpuid(info, 0x80000002 + i);
                std::memcpy(brand + i * 16, info, 16);
            }
        }
        const char* shown = brand;
        while (*shown == ' ') ++shown;
        for (size_t end = std::strlen(brand); end > 0 && brand[end - 1] == ' ';) brand[--end] = '\0';
        SYSTEM_INFO system{};
        GetNativeSystemInfo(&system);
        Line("cpu %s, %lu logical processors", *shown ? shown : "(no brand string)",
             static_cast<unsigned long>(system.dwNumberOfProcessors));

        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof memory;
        if (GlobalMemoryStatusEx(&memory))
        {
            Line("memory %.1f GB physical, %.1f GB free; address space %.0f MB for this process, %.0f MB free",
                 memory.ullTotalPhys / 1073741824.0, memory.ullAvailPhys / 1073741824.0,
                 memory.ullTotalVirtual / 1048576.0, memory.ullAvailVirtual / 1048576.0);
        }

        char command[4096];
        Line("command line %s", Utf8(GetCommandLineW(), command, sizeof command));
    }

    /// The profiler's line: off, or on with its port and interfaces, and what decided it.
    void Profiler()
    {
#if defined(WXL_CORE_PROFILE) && WXL_CORE_PROFILE
        const wxl::core::settings::ProfileChoice choice = wxl::core::settings::ResolveProfile(g_settings);
        if (choice.ignored)
            wxl::log::WriteUnfilteredF(Level::Warn, kTag, "WXL_PROFILE=%s in the environment is neither 0 nor 1; ignored",
                                       choice.ignored);
        const char* why = choice.from_env              ? "WXL_PROFILE in the environment, over the file's"
                          : g_settings.has_profile_enabled ? "[profile] enabled in WarcraftXL.ini"
                                                           : "the default";
        if (!choice.enabled)
            Line("profiler: off (%s)%s; no port opened", why,
                 choice.from_env || g_settings.has_profile_enabled ? "" : "; [profile] enabled = 1 starts it");
        else
            Line("profiler: on (%s), Tracy 0.14.1 on demand, port %u, %s", why, static_cast<unsigned>(choice.port),
                 choice.any ? "listen any: every interface, other computers can connect"
                            : "listen localhost: 127.0.0.1 only");
#else
        Line("profiler: not in this build (core built with WXL_PROFILE=OFF)%s",
             g_settings.has_profile ? "; the [profile] section of WarcraftXL.ini is ignored" : "");
#endif
    }

    /// The graphics line: the API [gx] asks for, if any, and the Vulkan settings.
    void Graphics()
    {
        using wxl::core::settings::GxApi;
        if (g_settings.gx_api == GxApi::client)
        {
            Line("graphics: the client's own choice of API; [gx] api = vulkan in WarcraftXL.ini tries Vulkan");
            return;
        }
        if (g_settings.gx_api == GxApi::d3d9)
        {
            Line("graphics: [gx] api = d3d9: D3D9, a Vulkan request of the client's included");
            return;
        }
        Line("graphics: [gx] api = vulkan: Vulkan is tried when the client makes its device, D3D9 if this machine "
             "cannot; validation %s, gpu %s, pipelines %s",
             g_settings.gx_validation ? "on" : "off", g_settings.gx_gpu[0] ? g_settings.gx_gpu : "the best one",
             g_settings.gx_async ? "compiled on workers, a draw into the scene left out until its pipeline is ready"
                                 : "compiled when first drawn, the frame waiting");
    }

    void Settings(const wxl::log::SessionFiles& files)
    {
        using wxl::log::LevelName;
        wxl::log::Level fromEnv;
        const bool      env = wxl::log::LevelFromEnvironment(fromEnv);
        if (!g_settings.found)
        {
            Line("settings: no %ls next to the executable; log level %s%s, %u earlier launches' logs kept at most",
                 wxl::core::settings::kFileName, LevelName(env ? fromEnv : Level::Info), env ? " (WXL_LOG_LEVEL)" : "",
                 g_settings.keep);
        }
        else
        {
            char   tags[1024];
            size_t used = 0;
            tags[0]     = '\0';
            for (unsigned i = 0; i < g_settings.tag_count; ++i)
            {
                const int wrote = std::snprintf(tags + used, sizeof tags - used, ", %s %s", g_settings.tags[i].name,
                                                LevelName(g_settings.tags[i].level));
                if (wrote > 0 && static_cast<size_t>(wrote) < sizeof tags - used) used += static_cast<size_t>(wrote);
            }
            Line("settings: %ls read; log level %s%s%s, %u earlier launches' logs kept at most",
                 wxl::core::settings::kFileName,
                 LevelName(env ? fromEnv : g_settings.has_level ? g_settings.level : Level::Info),
                 env ? " (WXL_LOG_LEVEL, over the file's)" : g_settings.has_level ? "" : " (the default)", tags,
                 g_settings.keep);
            for (unsigned i = 0; i < g_settings.problem_count; ++i)
                wxl::log::WriteUnfilteredF(Level::Warn, kTag, "settings: %ls %s; ignored",
                                           wxl::core::settings::kFileName, g_settings.problems[i]);
        }

        char path[1536];
        Utf8(files.path, path, sizeof path);
        if (files.kept_previous)
        {
            char previous[288];
            Line("log %s; the previous launch's is now %s; %u earlier log(s) in Logs\\%s", path,
                 Utf8(files.previous, previous, sizeof previous), files.kept,
                 files.removed ? ", the oldest removed" : "");
        }
        else
        {
            Line("log %s; %u earlier log(s) in Logs\\", path, files.kept);
        }
        Profiler();
        Graphics();
        if (files.fallback)
            wxl::log::WriteUnfilteredF(Level::Warn, kTag,
                                       "Logs\\WarcraftXL.log is held by another process (a second client from this "
                                       "folder?); this launch writes to %s",
                                       path);
    }
}

namespace wxl::core::session
{
    bool Begin()
    {
        if (g_began) return true;
        g_began = GetTickCount64();

        wchar_t dir[1024];
        const size_t length = ExeDirectory(dir, 1024);
        if (length == 0) return false;

        wchar_t ini[1100];
        swprintf(ini, 1100, L"%ls%ls", dir, settings::kFileName);
        settings::Load(ini, g_settings);
        settings::ApplyToLog(g_settings);

        wchar_t logs[1100];
        swprintf(logs, 1100, L"%lsLogs", dir);
        wxl::log::SessionFiles files;
        const bool open = wxl::log::OpenSession(logs, L"WarcraftXL", g_settings.keep, &files);

        // The first line names the launch: the next launch reads it back to name this file.
        SYSTEMTIME local{}, utc{};
        GetLocalTime(&local);
        GetSystemTime(&utc);
        Line("launch %04u-%02u-%02u %02u:%02u:%02u local (%02u:%02u:%02u UTC), pid %lu", local.wYear, local.wMonth,
             local.wDay, local.wHour, local.wMinute, local.wSecond, utc.wHour, utc.wMinute, utc.wSecond,
             GetCurrentProcessId());

        char linked[stamp::kDescriptionSize];
        stamp::Describe(linked, sizeof linked);
        Line("runtime core %s, WarcraftXL.dll %u.%u.%u linked %s, abi %u.%u.%u",
             sizeof(void*) == 4 ? "x86 (the stock Wow.exe)" : "x64 (the recompiled client)",
             static_cast<unsigned>(WXL_CORE_VERSION / 10000), static_cast<unsigned>(WXL_CORE_VERSION / 100 % 100),
             static_cast<unsigned>(WXL_CORE_VERSION % 100), linked, static_cast<unsigned>(WXL_ABI_VERSION_MAJOR),
             static_cast<unsigned>(WXL_ABI_VERSION_MINOR), static_cast<unsigned>(WXL_ABI_VERSION_FEATURE));
        Line("built from core %s, sdk %s%s%s", WXL_BUILD_ID_CORE, WXL_BUILD_ID_SDK,
             WXL_BUILD_ID_CLIENT[0] ? ", client " : "", WXL_BUILD_ID_CLIENT);

        Executable();
        System();
        Settings(files);
        return open;
    }

    const wxl::core::settings::Settings& CurrentSettings() { return g_settings; }

    void ReportMods(const wxl::host::ModHost& host)
    {
        const auto& loaded  = host.Loaded();
        const auto& refused = host.Refused();
        size_t      skipped = 0;
        for (const wxl::host::RefusedMod& mod : refused)
            if (mod.verdict == wxl::host::Verdict::Skip) ++skipped;
        Line("mods: %zu found in Mods\\, %zu loaded, %zu refused, %zu skipped (Lua mods, which wxl.lua runs, or "
             "mods for another runtime)",
             loaded.size() + refused.size(), loaded.size(), refused.size() - skipped, skipped);

        for (const wxl::host::LoadedMod& mod : loaded)
        {
            const std::string asks  = wxl::host::AbiText(mod.manifest.abi);
            const std::string built = wxl::host::AbiText(mod.built_abi);
            std::error_code   ignored;
            const auto        relative = std::filesystem::relative(mod.binary, host.Config().mods_directory, ignored);
            const auto        shown    = (ignored ? mod.binary : relative).u8string();
            const std::string binary(shown.begin(), shown.end());
            if (mod.disabled)
                wxl::log::WriteUnfilteredF(Level::Warn, kTag, "mod %s %s (%s) loaded and disabled: %s; abi %s, built %s, %s",
                                           mod.id.c_str(), mod.manifest.version.text.c_str(), mod.name.c_str(),
                                           mod.fault.c_str(), asks.c_str(), built.c_str(), binary.c_str());
            else
                Line("mod %s %s (%s) loaded; abi %s, built %s, %s", mod.id.c_str(), mod.manifest.version.text.c_str(),
                     mod.name.c_str(), asks.c_str(), built.c_str(), binary.c_str());
        }
        for (const wxl::host::RefusedMod& mod : refused)
        {
            const char* id = mod.id.empty() ? mod.directory_name.c_str() : mod.id.c_str();
            if (mod.verdict == wxl::host::Verdict::Skip)
                Line("mod %s not loaded here: %s", id, mod.reason.c_str());
            else
                wxl::log::WriteUnfilteredF(Level::Warn, kTag, "mod %s refused (folder %s): %s", id,
                                           mod.directory_name.c_str(), mod.reason.c_str());
        }
    }

    void End(const char* how)
    {
        if (InterlockedExchange(&g_ended, 1) != 0) return;
        char up[32];
        FormatDuration(UptimeMs(), up, sizeof up);
        Line("session end: %s, after %s", how ? how : "?", up);
        wxl::log::Flush();
    }

    uint64_t UptimeMs() { return g_began ? GetTickCount64() - g_began : 0; }

    void FormatDuration(uint64_t ms, char* out, size_t capacity)
    {
        const uint64_t s = ms / 1000;
        if (s >= 3600)
            std::snprintf(out, capacity, "%lluh %02llum %02llus", s / 3600, s / 60 % 60, s % 60);
        else if (s >= 60)
            std::snprintf(out, capacity, "%llum %02llus", s / 60, s % 60);
        else
            std::snprintf(out, capacity, "%.1f s", static_cast<double>(ms) / 1000.0);
    }
}
