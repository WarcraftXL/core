// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The wxl.m2file service: model files as the client's loader reads them.

#include "services/M2FileService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/guard.hpp"
#include "wxl/host/mod_host.hpp"

#include <windows.h>

#include <cstring>
#include <exception>
#include <mutex>
#include <vector>

namespace
{
    constexpr char kTag[] = "m2file";

    namespace binding = wxl::core::binding;

    struct Watcher
    {
        wxl_m2file_fn fn;
        void*         user;
        uint32_t      owner;
        uint32_t      token;
        bool          live;
    };

    std::vector<Watcher> g_watchers;
    uint32_t             g_live      = 0;
    uint32_t             g_nextToken = 1;
    int                  g_depth     = 0; // dispatches in progress: no compaction under them

    struct WatcherCall
    {
        const Watcher* watcher;
        wxl_m2file*    file;
        wxl_bool       answer;
    };

    void InvokeWatcher(void* context)
    {
        WatcherCall* call = static_cast<WatcherCall*>(context);
        try
        {
            call->answer = call->watcher->fn(call->file, call->watcher->user);
        }
        catch (const std::exception& e)
        {
            WLOG_ERROR(kTag, "a model file watcher threw: %s", e.what());
            call->answer = WXL_FALSE;
        }
        catch (...)
        {
            WLOG_ERROR(kTag, "a model file watcher threw");
            call->answer = WXL_FALSE;
        }
    }

    void Compact()
    {
        if (g_depth != 0) return;
        std::erase_if(g_watchers, [](const Watcher& watcher) { return !watcher.live; });
    }

    void Drop(Watcher& watcher)
    {
        if (!watcher.live) return;
        watcher.live = false;
        --g_live;
    }

    // --- Memory an M2 array can reach ---------------------------------------------------------
    //
    // An array of the client's models is a 32-bit offset from the array itself, so what it points
    // at must lie within 2 GB of it. The client's own allocator gives no such promise on x64.
    // Chunks are reserved near the anchor asked for and carved front to back; one serves every
    // later request whose anchor it is also near. Nothing is freed: a model's arrays never are.

    constexpr uintptr_t kReach     = 0x40000000;  // 1 GB each way: well inside a 32-bit offset
    constexpr uintptr_t kChunkSize = 0x100000;    // 1 MB per chunk, unless one request is larger

    struct Chunk
    {
        uint8_t* base;
        size_t   size;
        size_t   used;
    };

    std::mutex         g_memoryLock;
    std::vector<Chunk> g_chunks;

    bool Near(uintptr_t a, uintptr_t b, size_t extent)
    {
        const uintptr_t lo = a < b ? a : b;
        const uintptr_t hi = a < b ? b : a;
        return hi - lo + extent < kReach;
    }

    /// Reserves and commits `bytes` (a multiple of the allocation granularity) within kReach of
    /// `anchor`: the free region nearest above it first, then the nearest below.
    uint8_t* ReserveNear(uintptr_t anchor, size_t bytes)
    {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        const uintptr_t granularity = info.dwAllocationGranularity;
        uintptr_t       minimum     = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
        uintptr_t       maximum     = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
        if (anchor > minimum + kReach) minimum = anchor - kReach;
        if (anchor + kReach < maximum) maximum = anchor + kReach;

        MEMORY_BASIC_INFORMATION region;
        for (uintptr_t at = anchor; at < maximum && VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof region);)
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(region.BaseAddress);
            const uintptr_t end  = base + region.RegionSize;
            if (region.State == MEM_FREE)
            {
                const uintptr_t candidate = ((base > at ? base : at) + granularity - 1) & ~(granularity - 1);
                if (candidate + bytes <= end && candidate + bytes <= maximum)
                    if (void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                        return static_cast<uint8_t*>(p);
            }
            at = end;
        }
        for (uintptr_t at = anchor; at > minimum && VirtualQuery(reinterpret_cast<void*>(at - 1), &region, sizeof region);)
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(region.BaseAddress);
            uintptr_t       end  = base + region.RegionSize;
            if (end > at) end = at;
            if (region.State == MEM_FREE && end - base >= bytes)
            {
                const uintptr_t candidate = (end - bytes) & ~(granularity - 1);
                if (candidate >= base && candidate >= minimum)
                    if (void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                        return static_cast<uint8_t*>(p);
            }
            at = base;
        }
        return nullptr;
    }

    // --- The table ----------------------------------------------------------------------------

    wxl_result WXL_CALL WatchFiles(wxl_m2file_fn fn, void* user, uint32_t* out_token)
    {
        if (!fn) return WXL_E_INVALID;
        wxl::host::ModHost* host  = wxl::core::Host();
        const uint32_t      token = g_nextToken++;
        g_watchers.push_back(Watcher{fn, user, host ? host->CurrentMod() : 0, token, true});
        ++g_live;
        if (out_token) *out_token = token;
        return WXL_OK;
    }

    void WXL_CALL UnwatchFiles(uint32_t token)
    {
        for (Watcher& watcher : g_watchers)
            if (watcher.token == token) Drop(watcher);
        Compact();
    }

    void* WXL_CALL AllocNear(const void* anchor, uint32_t size)
    {
        if (!anchor || size == 0) return nullptr;
        const uintptr_t at    = reinterpret_cast<uintptr_t>(anchor);
        const size_t    bytes = (static_cast<size_t>(size) + 15) & ~size_t(15);

        std::lock_guard lock(g_memoryLock);
        for (Chunk& chunk : g_chunks)
        {
            if (chunk.size - chunk.used < bytes) continue;
            uint8_t* p = chunk.base + chunk.used;
            if (!Near(at, reinterpret_cast<uintptr_t>(p), bytes)) continue;
            chunk.used += bytes;
            return p; // VirtualAlloc zeroed it, and nothing gave it back
        }

        const size_t reserve = bytes > kChunkSize ? (bytes + 0xFFFF) & ~size_t(0xFFFF) : kChunkSize;
        uint8_t*     base    = ReserveNear(at, reserve);
        if (!base)
        {
            WLOG_ERROR(kTag, "no memory within reach of %p for %u bytes", anchor, size);
            return nullptr;
        }
        g_chunks.push_back(Chunk{base, reserve, bytes});
        return base;
    }

    void WXL_CALL AnimateBones(void* file)
    {
        if (file) binding::m2_file_animate_bones(file);
    }

    const wxl_m2file_api kApi = {
        sizeof(wxl_m2file_api), &WatchFiles, &UnwatchFiles, &AllocNear, &AnimateBones,
    };
}

namespace wxl::core::services
{
    const wxl_m2file_api& M2FileApi() { return kApi; }

    namespace m2file
    {
        bool Watching() { return g_live != 0; }

        bool Dispatch(wxl_m2file& file)
        {
            wxl::host::ModHost* host     = wxl::core::Host();
            bool                accepted = true;
            ++g_depth;
            // By index, and up to the size at the start: a watcher may watch again, which grows
            // the vector, and one added during a dispatch starts with the next file.
            const size_t count = g_watchers.size();
            for (size_t i = 0; i < count; ++i)
            {
                if (!g_watchers[i].live) continue;
                const Watcher    watcher = g_watchers[i];
                WatcherCall      call{&watcher, &file, WXL_TRUE};
                const uint32_t   previous = host ? host->SwapCurrentMod(watcher.owner) : 0;
                wxl::host::Fault fault;
                const bool       returned = wxl::host::CallGuarded(&InvokeWatcher, &call, &fault);
                if (host) host->SwapCurrentMod(previous);
                if (returned)
                {
                    if (!call.answer) accepted = false;
                    continue;
                }

                accepted = false;
                char what[96];
                wxl::host::DescribeFault(fault, what, sizeof what);
                WLOG_ERROR(kTag, "a model file watcher faulted on '%s': %s", file.name ? file.name : "?", what);
                if (host && watcher.owner != 0)
                    host->Disable(watcher.owner, "in its model file watcher");
                else
                    Drop(g_watchers[i]);
            }
            --g_depth;
            Compact();
            return accepted;
        }

        void ForgetMod(uint32_t mod)
        {
            for (Watcher& watcher : g_watchers)
                if (watcher.owner == mod) Drop(watcher);
            Compact();
        }

        void Clear()
        {
            for (Watcher& watcher : g_watchers) Drop(watcher);
            Compact();
        }
    }
}
