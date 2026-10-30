// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The map's far terrain from a WDL of later clients (see TerrainLowDetail.hpp).

#include "services/TerrainLowDetail.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace wxl::core::services::low_detail
{
    namespace
    {
        constexpr const char* kTag = "terrain";
    }

    bool Load(void* detail, const char* mapPath, const char* mapName)
    {
        if (!detail || !mapPath || !mapName) return false;
        char name[300];
        std::snprintf(name, sizeof name, "%s\\%s.wdl", mapPath, mapName);
        void* file = wxl::core::binding::file_open(name);
        if (!file) return false; // the client finds none either
        std::vector<uint8_t> bytes;
        const uint64_t       size = wxl::core::binding::file_size(file);
        if (size >= 16 && size < (256u << 20))
        {
            bytes.resize(static_cast<size_t>(size));
            if (wxl::core::binding::file_read(file, bytes.data(), static_cast<uint32_t>(size)) != size) bytes.clear();
        }
        wxl::core::binding::file_close(file);
        const Plan plan = Read(bytes.data(), static_cast<uint32_t>(bytes.size()));
        if (plan.clientReads) return false;
        if (!plan.maof)
        {
            WLOG_WARN(kTag, "%s has no MAOF the client can read: the map has no far terrain", name);
            return true; // as for a missing WDL
        }
        if (!wxl::core::binding::map_low_detail_set(detail, bytes.data(), static_cast<uint32_t>(bytes.size()), plan.maof))
        {
            WLOG_WARN(kTag, "%s: no memory for it: the map has no far terrain", name);
            return true;
        }
        uint32_t made = 0;
        for (const wxl_core_area_low& area : plan.areas) made += wxl::core::binding::map_area_low_add(detail, &area) ? 1u : 0u;
        WLOG_INFO(kTag, "%s is a WDL of a later client (MAOF after MVER's other chunks): its far terrain read natively, %u area(s)%s, its map "
                        "objects (by FileDataID) left out",
                  name, made, plan.skipped || made < plan.areas.size() ? " (some left out: no whole MARE, or no room)" : "");
        return true;
    }
}
