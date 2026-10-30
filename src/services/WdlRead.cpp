// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A WDL read as the client of 3.3.5a would need it (see TerrainLowDetail.hpp): portable, no client.

#include "services/TerrainLowDetail.hpp"

#include <cmath>
#include <cstring>

namespace wxl::core::services::low_detail
{
    namespace
    {
        constexpr uint32_t    kMare = 545 * 2; // 17 x 17 + 16 x 16 heights, int16

        uint32_t U32(const uint8_t* at)
        {
            uint32_t v;
            std::memcpy(&v, at, 4);
            return v;
        }

        /// A chunk id as the file stores it ("MAOF" reads "FOAM").
        constexpr uint32_t Id(const char (&four)[5])
        {
            return static_cast<uint32_t>(static_cast<uint8_t>(four[0])) << 24 | static_cast<uint32_t>(static_cast<uint8_t>(four[1])) << 16 |
                   static_cast<uint32_t>(static_cast<uint8_t>(four[2])) << 8 | static_cast<uint32_t>(static_cast<uint8_t>(four[3]));
        }
    }

    Plan Read(const uint8_t* wdl, uint32_t size)
    {
        Plan plan;
        if (!wdl || size < 16 || U32(wdl) != Id("MVER")) return plan; // the client's to fail on as it does
        const uint32_t second = 8 + U32(wdl + 4);
        if (second + 8 > size) return plan;
        const uint32_t next = U32(wdl + second);
        if (next == Id("MWMO") || next == Id("MAOF")) return plan;
        plan.clientReads = false;

        for (uint32_t at = 0; at + 8 <= size;)
        {
            const uint32_t id = U32(wdl + at), len = U32(wdl + at + 4);
            if (len > size - at - 8) break;
            if (id == Id("MAOF") && len >= 4096 * 4)
            {
                plan.maof = at + 8;
                break;
            }
            at += 8 + len;
        }
        if (!plan.maof) return plan;

        for (uint32_t index = 0; index < 4096; ++index)
        {
            const uint32_t mare = U32(wdl + plan.maof + 4 * index);
            if (!mare) continue;
            if (static_cast<uint64_t>(mare) + 8 + kMare > size || U32(wdl + mare) != Id("MARE") || U32(wdl + mare + 4) < kMare)
            {
                ++plan.skipped;
                continue;
            }
            wxl_core_area_low area{};
            area.index   = index;
            area.column  = static_cast<int32_t>(index % 64);
            area.row     = static_cast<int32_t>(index / 64);
            area.heights = mare + 8;
            // After MARE: MAOE (Legion and later) may come first, then MAHO.
            uint32_t after = mare + 8 + U32(wdl + mare + 4);
            if (after + 8 <= size && U32(wdl + after) == Id("MAOE") && U32(wdl + after + 4) <= size - after - 8)
                after += 8 + U32(wdl + after + 4);
            area.index_count = 3072;
            if (after + 8 + 32 <= size && U32(wdl + after) == Id("MAHO") && U32(wdl + after + 4) >= 32)
            {
                area.holes       = after + 8;
                area.index_count = 0;
                for (uint32_t r = 0; r < 16; ++r)
                {
                    uint16_t row;
                    std::memcpy(&row, wdl + area.holes + 2 * r, 2);
                    for (uint32_t bit = 0; bit < 16; ++bit)
                        if (!((1u << bit) & row)) area.index_count += 12;
                }
            }
            // The bounds as the client's loop works them out.
            int32_t lo = 100000, hi = -100000;
            for (uint32_t i = 0; i < 545; ++i)
            {
                int16_t h;
                std::memcpy(&h, wdl + area.heights + 2 * i, 2);
                if (h > hi) hi = h;
                if (h < lo) lo = h;
            }
            const float top  = 17066.666f - static_cast<float>(area.row * 16) * 33.333332f;
            const float left = static_cast<float>(area.column * 16) * -33.333332f + 17066.666f;
            area.top_left[0] = top;
            area.top_left[1] = left;
            area.box_max[0]  = top;
            area.box_max[1]  = left;
            area.box_max[2]  = static_cast<float>(hi);
            area.box_min[0]  = top - 533.33331f;
            area.box_min[1]  = left - 533.33331f;
            area.box_min[2]  = static_cast<float>(lo);
            area.center[0]   = (area.box_max[0] + area.box_min[0]) * 0.5f;
            area.center[1]   = (area.box_min[1] + area.box_max[1]) * 0.5f;
            area.center[2]   = 0.5f * (area.box_min[2] + area.box_max[2]);
            const float dx = area.box_max[0] - area.center[0], dy = area.box_max[1] - area.center[1], dz = area.box_max[2] - area.center[2];
            area.radius = std::sqrt(dx * dx + dy * dy + dz * dz);
            plan.areas.push_back(area);
        }
        return plan;
    }

}
