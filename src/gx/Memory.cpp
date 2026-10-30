// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The graphics memory report (Memory.hpp).

#include "gx/Memory.hpp"

#include "common/Log.hpp"
#include "gx/Vulkan.hpp"
#include "gx/vk/gxfront/Front.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace
{
    constexpr char kTag[] = "gx";

    struct Space
    {
        uint64_t total = 0;     ///< the addresses the process may use
        uint64_t committed = 0;
        uint64_t reserved = 0;
        uint64_t image = 0;     ///< of what is used: executables and libraries
        uint64_t mapped = 0;    ///< file and section views, the driver's mappings among them
        uint64_t largest = 0;   ///< the largest free block
    };

    Space Measure()
    {
        Space       space;
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        uintptr_t       at  = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
        const uintptr_t end = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
        space.total         = static_cast<uint64_t>(end - at) + 1;
        while (at < end)
        {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof region)) break;
            const uint64_t size = region.RegionSize;
            if (region.State == MEM_FREE) space.largest = std::max(space.largest, size);
            else
            {
                (region.State == MEM_COMMIT ? space.committed : space.reserved) += size;
                if (region.Type == MEM_IMAGE) space.image += size;
                else if (region.Type == MEM_MAPPED) space.mapped += size;
            }
            const uintptr_t next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
            if (next <= at) break;
            at = next;
        }
        return space;
    }

    unsigned long long MiB(uint64_t bytes) { return static_cast<unsigned long long>((bytes + (1u << 19)) >> 20); }

    ULONGLONG g_first = 0; ///< the first frame's tick
    ULONGLONG g_next  = 0; ///< when the next report is due
}

namespace wxl::core::gx::memory
{
    void Report(const char* when)
    {
        const Space space = Measure();
        char        vulkan[384] = {};
        if (wxl::vk::gxfront::Front* front = VulkanFront())
        {
            const wxl::vk::gxfront::Front::Memory m = front->MemoryUse();
            std::snprintf(vulkan, sizeof vulkan,
                          "; the Vulkan renderer maps %llu MiB (staging %llu, draw data %llu, stream pools %llu) and keeps %llu MiB of "
                          "pool copies, %llu ring chunk(s) given back; GPU memory %llu of %llu MiB, system memory the GPU reads %llu MiB",
                          MiB(m.staging + m.data + m.stream), MiB(m.staging), MiB(m.data), MiB(m.stream), MiB(m.pool_copies),
                          static_cast<unsigned long long>(m.trimmed), MiB(m.device_usage), MiB(m.device_budget), MiB(m.host_usage));
        }
        WLOG_INFO(kTag,
                  "memory %s (%s, %s): address space %llu MiB used of %llu (committed %llu, reserved %llu; images %llu, mapped views "
                  "%llu), largest free block %llu MiB%s",
                  when, sizeof(void*) == 4 ? "x86" : "x64", VulkanActive() ? "Vulkan" : "Direct3D 9", MiB(space.committed + space.reserved),
                  MiB(space.total), MiB(space.committed), MiB(space.reserved), MiB(space.image), MiB(space.mapped), MiB(space.largest),
                  vulkan);
    }

    void Present()
    {
        const ULONGLONG now = GetTickCount64();
        if (!g_first)
        {
            g_first = now;
            g_next  = now + 60'000;
            return;
        }
        if (now < g_next) return;
        g_next = now + 300'000;
        char when[64];
        std::snprintf(when, sizeof when, "after %llu min of frames", static_cast<unsigned long long>((now - g_first) / 60'000));
        Report(when);
    }
}
