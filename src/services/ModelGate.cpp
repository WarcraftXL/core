// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Placements of models the client cannot load (see ModelGate.hpp).

#include "services/ModelGate.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"

#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::core::services::model_gate
{
    namespace
    {
        constexpr const char* kTag = "terrain";
        /// The newest M2 (MD20) and WMO root versions this client reads.
        constexpr uint32_t kM2Version = 264, kWmoVersion = 17;

        std::unordered_map<std::string, Verdict> g_seen;
        uint64_t                                 g_leftOut = 0;

        uint32_t U32(const uint8_t* at)
        {
            uint32_t v;
            std::memcpy(&v, at, sizeof v);
            return v;
        }

        /// The name the client opens: lower case, '\\', ".mdx"/".mdl" as ".m2".
        std::string Normal(const char* name, bool mapObject)
        {
            std::string out(name);
            for (char& c : out) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const size_t dot = out.rfind('.');
            if (!mapObject && dot != std::string::npos && (out.compare(dot, 4, ".mdx") == 0 || out.compare(dot, 4, ".mdl") == 0) &&
                dot + 4 == out.size())
                out.replace(dot, 4, ".m2");
            return out;
        }
    }

    Verdict Judge(const uint8_t* head, size_t size, bool mapObject)
    {
        if (!head) return Verdict::loadable;
        if (mapObject)
        {
            // MVER (as the file stores it, "REVM"), its size, the version.
            if (size < 12 || std::memcmp(head, "REVM", 4) != 0) return Verdict::loadable;
            if (U32(head + 8) > kWmoVersion) return Verdict::newer;
            // The chunks of 3.3.5a's roots (CMapObj reads them in this order, by position).
            static const char* const kKnown[] = {"REVM", "DHOM", "XTOM", "TMOM", "NGOM", "IGOM", "BSOM", "VPOM", "TPOM", "RPOM",
                                                 "VVOM", "BVOM", "TLOM", "SDOM", "NDOM", "DDOM", "GOFM", "PVCM"};
            for (size_t at = 0; at + 8 <= size;)
            {
                bool known = false;
                for (const char* k : kKnown) known = known || std::memcmp(head + at, k, 4) == 0;
                if (!known) return Verdict::later;
                at += 8 + static_cast<size_t>(U32(head + at + 4));
            }
            return Verdict::loadable;
        }
        if (size >= 4 && std::memcmp(head, "MD21", 4) == 0) return Verdict::chunked;
        if (size >= 8 && std::memcmp(head, "MD20", 4) == 0 && U32(head + 4) > kM2Version) return Verdict::newer;
        return Verdict::loadable;
    }

    bool Loadable(const char* name, bool mapObject)
    {
        if (!name || !*name) return true;
        const std::string path = Normal(name, mapObject);
        const auto        seen = g_seen.find(path);
        if (seen != g_seen.end()) return seen->second == Verdict::loadable;
        // An M2's first 12 bytes; a WMO root whole (they are small: its groups are other files).
        std::vector<uint8_t> bytes(12, 0);
        size_t               got    = 0;
        bool                 opened = false;
        if (void* file = wxl::core::binding::file_open(path.c_str()))
        {
            opened = true;
            const uint64_t size = mapObject ? wxl::core::binding::file_size(file) : 12;
            if (mapObject && size > 12 && size < (32u << 20)) bytes.resize(static_cast<size_t>(size));
            got = wxl::core::binding::file_read(file, bytes.data(), static_cast<uint32_t>(bytes.size()));
            wxl::core::binding::file_close(file);
        }
        const uint8_t* head = bytes.data();
        // A map object root the client cannot open ends it (CMap::SafeOpen); an M2 it cannot open
        // it leaves undrawn.
        const Verdict verdict = !opened && mapObject ? Verdict::missing : got ? Judge(head, got, mapObject) : Verdict::loadable;
        g_seen.emplace(path, verdict);
        if (verdict == Verdict::later)
            WLOG_WARN(kTag, "%s is a map object of a later client (chunks this client's reader does not expect): its placements are left out",
                      path.c_str());
        else if (verdict == Verdict::missing)
            WLOG_WARN(kTag, "%s is a map object the client's files do not have (it would end the client): its placements are left out",
                      path.c_str());
        else if (verdict == Verdict::chunked)
            WLOG_WARN(kTag, "%s is an M2 in Legion's chunked form (MD21), which this client cannot read: its placements are left out", path.c_str());
        else if (verdict == Verdict::newer && mapObject)
            WLOG_WARN(kTag, "%s is a map object of version %u, past this client's %u: its placements are left out", path.c_str(), U32(head + 8),
                      kWmoVersion);
        else if (verdict == Verdict::newer)
            WLOG_WARN(kTag, "%s is an M2 of version %u, past this client's %u: its placements are left out", path.c_str(), U32(head + 4),
                      kM2Version);
        return verdict == Verdict::loadable;
    }

    uint64_t LeftOut() { return g_leftOut; }
    void     CountLeftOut(uint32_t count) { g_leftOut += count; }

    void Clear()
    {
        g_seen.clear();
        g_leftOut = 0;
    }
}
