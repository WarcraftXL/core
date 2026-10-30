// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL

// Before windows.h, which would otherwise bring the first Windows Sockets in.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "profile/Profile.hpp"

#include "common/Log.hpp"
#include "runtime/Core.hpp"
#include "services/FootprintService.hpp"
#include "services/RenderService.hpp"

#include "wxl/core/profile.h"
#include "wxl/host/mod_host.hpp"
#include "wxl/host/profile.hpp"

#include <client/TracyProfiler.hpp>
#include <tracy/TracyC.h>

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>

static_assert(sizeof(wxl_core_zone_site) == sizeof(___tracy_source_location_data) &&
                  offsetof(wxl_core_zone_site, name) == offsetof(___tracy_source_location_data, name) &&
                  offsetof(wxl_core_zone_site, function) == offsetof(___tracy_source_location_data, function) &&
                  offsetof(wxl_core_zone_site, file) == offsetof(___tracy_source_location_data, file) &&
                  offsetof(wxl_core_zone_site, line) == offsetof(___tracy_source_location_data, line) &&
                  offsetof(wxl_core_zone_site, color) == offsetof(___tracy_source_location_data, color),
              "the client's zone sites are handed to the profiler as its source locations");

namespace
{
    constexpr const char* kProfileTag = "profile";

    /// Set once the profiler is up: before, no Tracy call may be made at all.
    std::atomic<bool> g_up{false};
    LONG              g_starting = 0;

    // --- what is kept for the process's life ----------------------------------------------------
    //
    // The profiler reads a location's strings, and a plot's or frame set's name, whenever a viewer
    // asks, long after they were given: everything it is handed lives here, never freed, and the
    // same values answer the same entry, so a mod that reloads does not add more.

    std::mutex                                      g_keptLock;
    std::deque<std::string>                         g_strings;
    std::deque<___tracy_source_location_data>       g_locations;
    std::map<std::tuple<std::string, std::string, std::string, uint32_t, uint32_t>,
             const ___tracy_source_location_data*>  g_locationOf;
    std::map<std::string, const char*, std::less<>> g_names;

    /// Held by g_keptLock.
    const char* Keep(std::string_view text)
    {
        g_strings.emplace_back(text);
        return g_strings.back().c_str();
    }

    /// A plot's or a frame set's name, kept. Null for an empty one.
    const char* Name(std::string_view name)
    {
        if (name.empty()) return nullptr;
        std::lock_guard<std::mutex> lock(g_keptLock);
        const auto found = g_names.find(name);
        if (found != g_names.end()) return found->second;
        const char* kept = Keep(name);
        g_names.emplace(std::string(name), kept);
        return kept;
    }

    bool Live() { return g_up.load(std::memory_order_acquire) && ___tracy_connected() != 0; }

    /// Whether a listening socket can take @p port where the profiler will listen: Tracy, given
    /// a port, tries it once and otherwise stays silent for the rest of the process.
    bool PortFree(uint16_t port, bool any)
    {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return true; // cannot tell: Tracy will try
        bool   free = false;
        SOCKET sock = INVALID_SOCKET;
        if (any)
        {
            // As Tracy listens: IPv6 taking IPv4 too, IPv4 alone where IPv6 is missing.
            sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
            if (sock != INVALID_SOCKET)
            {
                unsigned long both = 0;
                setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&both), sizeof both);
                sockaddr_in6 address{};
                address.sin6_family = AF_INET6;
                address.sin6_port   = htons(port);
                address.sin6_addr   = in6addr_any;
                free = bind(sock, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
                closesocket(sock);
                WSACleanup();
                return free;
            }
        }
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock != INVALID_SOCKET)
        {
            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_port        = htons(port);
            address.sin_addr.s_addr = htonl(any ? INADDR_ANY : INADDR_LOOPBACK);
            free = bind(sock, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
            closesocket(sock);
        }
        else free = true;
        WSACleanup();
        return free;
    }

    // --- the host's zones --------------------------------------------------------------------

    const void* HostLocation(void*, std::string_view name, std::string_view function, uint32_t color)
    {
        return wxl::core::profile::Location(name, function, "", 0, color);
    }
    uint64_t HostBegin(void*, const void* location) { return wxl::core::profile::Begin(location); }
    void     HostEnd(void*, uint64_t zone) { wxl::core::profile::End(zone); }

    const wxl::host::Profiler kHostProfiler{nullptr, &HostLocation, &HostBegin, &HostEnd};

    wxl::host::ModHost* g_host = nullptr;

    // --- the frame -----------------------------------------------------------------------------

    uint64_t g_connection = 0; ///< the connection the plots were last configured for
    bool     g_named      = false;

    constexpr const char kStormBytes[]  = "storm bytes";
    constexpr const char kStormBlocks[] = "storm blocks";
    constexpr const char kTableBytes[]  = "accounting tables";

    // --- the service ---------------------------------------------------------------------------

    std::string_view View(wxl_str text) { return std::string_view(text.data ? text.data : "", text.data ? text.size : 0); }

    wxl_bool WXL_CALL SvcConnected() { return Live() ? WXL_TRUE : WXL_FALSE; }

    uintptr_t WXL_CALL SvcLocation(wxl_str name, wxl_str function, wxl_str file, uint32_t line, uint32_t color)
    {
        return reinterpret_cast<uintptr_t>(
            wxl::core::profile::Location(View(name), View(function), View(file), line, color));
    }

    // A zone is the connection it was opened under: 0 is none, and a zone left over from an
    // earlier connection is closed by nobody, which is what the profiler asks.
    uintptr_t WXL_CALL SvcBegin(uintptr_t location)
    {
        return static_cast<uintptr_t>(wxl::core::profile::Begin(reinterpret_cast<const void*>(location)));
    }
    void WXL_CALL SvcEnd(uintptr_t zone) { wxl::core::profile::End(zone); }

    void WXL_CALL SvcText(uintptr_t zone, wxl_str text)
    {
        if (!zone || !text.size || !g_up.load(std::memory_order_acquire)) return;
        TracyCZoneCtx ctx{};
        ctx.active       = 1;
        ctx.connectionId = zone;
        ___tracy_emit_zone_text(ctx, text.data, text.size < 0xFFFF ? text.size : 0xFFFE);
    }

    void WXL_CALL SvcPlot(wxl_str name, double value)
    {
        if (!Live()) return;
        if (const char* kept = Name(View(name))) ___tracy_emit_plot(kept, value);
    }

    void WXL_CALL SvcMessage(wxl_str text)
    {
        if (!Live() || !text.size) return;
        ___tracy_emit_logString(TracyMessageSeverityInfo, 0, 0, text.size, text.data);
    }

    void WXL_CALL SvcFrameMark(wxl_str name)
    {
        if (!Live()) return;
        if (const char* kept = Name(View(name))) ___tracy_emit_frame_mark(kept);
    }
    wxl_result WXL_CALL SvcGpuZoneBegin(wxl_str name)
    {
        return wxl::core::services::render::GpuZoneBegin(std::string_view(name.data ? name.data : "", name.size));
    }

    void WXL_CALL SvcGpuZoneEnd() { wxl::core::services::render::GpuZoneEnd(); }
} // namespace

namespace wxl::core::profile
{
    void Start(const settings::ProfileChoice& choice)
    {
        if (!choice.enabled) return; // the launch header says so
        if (InterlockedCompareExchange(&g_starting, 1, 0) != 0) return;

        // Tracy 0.14.1 reads these through GetEnvironmentVariableA, the port when the profiler
        // is made (___tracy_startup_profiler) and the other two when it opens its socket: set
        // here, they decide, whatever the process was started with. 127.0.0.1 alone is IPv4
        // alone: an IPv6 loopback socket would not take a viewer connecting to 127.0.0.1.
        char port[8];
        std::snprintf(port, sizeof port, "%u", static_cast<unsigned>(choice.port));
        SetEnvironmentVariableA("TRACY_PORT", port);
        SetEnvironmentVariableA("TRACY_ONLY_LOCALHOST", choice.any ? "0" : "1");
        SetEnvironmentVariableA("TRACY_ONLY_IPV4", choice.any ? nullptr : "1");

        const uint16_t number = choice.port;
        const bool     any    = choice.any;
        std::thread([number, any] {
            if (!PortFree(number, any))
                WLOG_WARN(kProfileTag,
                          "port %u is taken%s: no viewer can connect to this client; set another port under "
                          "[profile] in WarcraftXL.ini (another client from this machine may hold it)",
                          static_cast<unsigned>(number), any ? "" : " on 127.0.0.1");
            ___tracy_startup_profiler();
            char        info[128];
            std::snprintf(info, sizeof info, "WarcraftXL %s, abi %u.%u.%u", sizeof(void*) == 8 ? "x64" : "x86",
                          static_cast<unsigned>(WXL_ABI_VERSION_MAJOR), static_cast<unsigned>(WXL_ABI_VERSION_MINOR),
                          static_cast<unsigned>(WXL_ABI_VERSION_FEATURE));
            ___tracy_emit_message_appinfo(info, std::strlen(info));
            g_up.store(true, std::memory_order_release);
            WLOG_INFO(kProfileTag, "profiler up (Tracy 0.14.1, on demand): a viewer connects to %s port %u",
                      any ? "this machine's address on" : "127.0.0.1", static_cast<unsigned>(number));
        }).detach();
    }

    void Attach(wxl::host::ModHost& host)
    {
        g_host = &host;
        host.SetProfiler(kHostProfiler);
    }

    bool Connected() { return Live(); }

    bool Started() { return g_up.load(std::memory_order_acquire); }

    void Present()
    {
        if (!g_up.load(std::memory_order_acquire)) return;
        const bool live = ___tracy_connected() != 0;
        if (g_host) g_host->SetProfiling(live);
        if (!live) return;

        if (!g_named)
        {
            ___tracy_set_thread_name("Main thread");
            g_named = true;
        }
        ___tracy_emit_frame_mark(nullptr);

        uint64_t bytes = 0, count = 0, tables = 0;
        if (!services::footprint::Totals(&bytes, &count, &tables)) return;
        // A plot's format reaches only the viewer connected when it is sent: again for each one.
        const uint64_t connection = tracy::GetProfiler().ConnectionId();
        if (connection != g_connection)
        {
            g_connection = connection;
            ___tracy_emit_plot_config(kStormBytes, TracyPlotFormatMemory, 0, 1, 0);
            ___tracy_emit_plot_config(kTableBytes, TracyPlotFormatMemory, 0, 1, 0);
            ___tracy_emit_plot_config(kStormBlocks, TracyPlotFormatNumber, 0, 1, 0);
        }
        ___tracy_emit_plot(kStormBytes, static_cast<double>(bytes));
        ___tracy_emit_plot(kStormBlocks, static_cast<double>(count));
        ___tracy_emit_plot(kTableBytes, static_cast<double>(tables));
    }

    const void* Location(std::string_view name, std::string_view function, std::string_view file,
                         uint32_t line, uint32_t color)
    {
        if (name.empty()) return nullptr;
        std::lock_guard<std::mutex> lock(g_keptLock);
        auto key   = std::make_tuple(std::string(name), std::string(function), std::string(file), line, color);
        auto found = g_locationOf.find(key);
        if (found != g_locationOf.end()) return found->second;
        ___tracy_source_location_data& made = g_locations.emplace_back();
        made.name     = Keep(name);
        made.function = Keep(function.empty() ? name : function);
        made.file     = Keep(file);
        made.line     = line;
        made.color    = color;
        g_locationOf.emplace(std::move(key), &made);
        return &made;
    }

    uint64_t Begin(const void* location)
    {
        if (!location || !g_up.load(std::memory_order_acquire)) return 0;
        const TracyCZoneCtx ctx =
            ___tracy_emit_zone_begin(static_cast<const ___tracy_source_location_data*>(location), 1);
        return ctx.active ? ctx.connectionId : 0;
    }

    void End(uint64_t zone)
    {
        if (!zone) return;
        TracyCZoneCtx ctx{};
        ctx.active       = 1;
        ctx.connectionId = zone;
        ___tracy_emit_zone_end(ctx);
    }

    const wxl_profile_api& Api()
    {
        static const wxl_profile_api api = [] {
            wxl_profile_api t{};
            t.struct_size = sizeof t;
            t.connected   = &SvcConnected;
            t.location    = &SvcLocation;
            t.zone_begin  = &SvcBegin;
            t.zone_end    = &SvcEnd;
            t.zone_text   = &SvcText;
            t.plot        = &SvcPlot;
            t.message     = &SvcMessage;
            t.frame_mark  = &SvcFrameMark;
            t.gpu_zone_begin = &SvcGpuZoneBegin;
            t.gpu_zone_end   = &SvcGpuZoneEnd;
            return t;
        }();
        return api;
    }
} // namespace wxl::core::profile

uint64_t WXL_CALL WarcraftXLZoneBegin(const wxl_core_zone_site* site)
{
    return wxl::core::profile::Begin(site);
}

void WXL_CALL WarcraftXLZoneEnd(uint64_t zone) { wxl::core::profile::End(zone); }
