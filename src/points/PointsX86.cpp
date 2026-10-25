// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.point on the stock client: a generated detour per point, armed at its first handler.

#include "points/PointsX86.hpp"

#include "common/Log.hpp"
#include "deinlined/Deinlined.hpp"
#include "hook/Engine.hpp"
#include "hook/Safety.hpp"
#include "points/DetourX86.hpp"
#include "runtime/Image.hpp"

#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace wxl::core::deinlined
{
    volatile uint8_t g_armed[points::kCount > 0 ? points::kCount : 1] = {};
}

namespace wxl::core::points
{
    namespace
    {
        constexpr char kTag[] = "point";

        namespace safety = ::wxl::hook::safety;

        /// In front of every seam and wxl.hook link on the address: the point's Replace decides
        /// whether they run at all, as the WXL_POINT line before the seam's call does on x64.
        constexpr int kPriority = INT_MIN;

#define WXL_POINT_X86(Ident, name, address, length, conv, ret, params) \
    using Detour_##Ident = DetourOf<k##Ident, ::wxl::seam::detail::conv_##conv, ret params>;
#define WXL_POINT_SITE(Ident, address, args)
#define WXL_POINT_X86_ABSENT(Ident, why)
#include "wxl/points/x86.def"
#undef WXL_POINT_X86_ABSENT
#undef WXL_POINT_SITE
#undef WXL_POINT_X86

        struct Detoured
        {
            uint32_t    index;
            uintptr_t   address; ///< in Wow.exe 12340
            size_t      length;
            void*       detour;
            void**      original;
        };

        Detoured g_rows[] = {
#define WXL_POINT_X86(Ident, name, address, length, conv, ret, params)                   \
    {k##Ident, address, length, reinterpret_cast<void*>(&Detour_##Ident::Detour),        \
     reinterpret_cast<void**>(&Detour_##Ident::original)},
#define WXL_POINT_SITE(Ident, address, args)
#define WXL_POINT_X86_ABSENT(Ident, why)
#include "wxl/points/x86.def"
#undef WXL_POINT_X86_ABSENT
#undef WXL_POINT_SITE
#undef WXL_POINT_X86
        };

        struct Absent
        {
            uint32_t    index;
            const char* why;
        };

        // --- De-inlined points: a recompiled parent body in which they are functions ------------

        /// After every seam and wxl.hook link and the parent's own point: the recompiled body
        /// takes the place of Wow.exe's, and whatever ran in front of it still does.
        constexpr int kBodyPriority = INT_MAX;

        struct Deinlined
        {
            uint32_t index;
            uint32_t parent;
            void*  (*body)();
        };

        const Deinlined kDeinlined[] = {
            {kCount, kCount, nullptr},
#define WXL_POINT_X86(Ident, name, address, length, conv, ret, params)
#define WXL_POINT_SITE(Ident, address, args)
#define WXL_POINT_X86_ABSENT(Ident, why)
#define WXL_POINT_X86_DEINLINED(Ident, name, Parent, ret, params) {k##Ident, k##Parent, &deinlined::Body_##Parent},
#include "wxl/points/x86.def"
#undef WXL_POINT_X86_DEINLINED
#undef WXL_POINT_X86_ABSENT
#undef WXL_POINT_SITE
#undef WXL_POINT_X86
        };

        const Deinlined* DeinlinedOf(uint32_t index)
        {
            for (const Deinlined& row : kDeinlined)
                if (row.index == index && row.body) return &row;
            return nullptr;
        }

        /// Per parent: the body in place (a link of its chain), how many armed points need it, and
        /// the body the harness put in place of the generated one.
        wxl::hook::Id g_bodyLinks[kCount > 0 ? kCount : 1] = {};
        uint32_t      g_bodyUsers[kCount > 0 ? kCount : 1] = {};
        void*         g_bodyNext[kCount > 0 ? kCount : 1]  = {};
        void*         g_bodyPlaced[kCount > 0 ? kCount : 1] = {};

        const Absent kAbsent[] = {
            {kCount, ""},
#define WXL_POINT_X86(Ident, name, address, length, conv, ret, params)
#define WXL_POINT_SITE(Ident, address, args)
#define WXL_POINT_X86_ABSENT(Ident, why) {k##Ident, why},
#include "wxl/points/x86.def"
#undef WXL_POINT_X86_ABSENT
#undef WXL_POINT_SITE
#undef WXL_POINT_X86
        };

        // --- Sites: inlined copies, reached by a mid-function hook -------------------------

        /// Where an argument lives at a site: a register of the context, or a stack slot off one.
        struct Source
        {
            size_t   reg    = 0;     ///< offsetof(Registers, <reg>)
            bool     memory = false; ///< [reg + offset] rather than reg
            intptr_t offset = 0;
        };

        constexpr size_t kMaxArgs = 16;

        struct Site
        {
            uint32_t     index;
            uintptr_t    address;
            const char*  args;               ///< "ecx,[esp+8]"
            uint32_t     count = 0;
            Source       sources[kMaxArgs];
            safety::Mid* hook = nullptr;
        };

        std::vector<Site>& Sites()
        {
            static std::vector<Site> sites = [] {
                std::vector<Site> made;
#define WXL_POINT_X86(Ident, name, address, length, conv, ret, params)
#define WXL_POINT_SITE(Ident, address, args) made.push_back(Site{k##Ident, address, args});
#define WXL_POINT_X86_ABSENT(Ident, why)
#include "wxl/points/x86.def"
#undef WXL_POINT_X86_ABSENT
#undef WXL_POINT_SITE
#undef WXL_POINT_X86
                return made;
            }();
            return sites;
        }

        bool Register(const char* name, size_t length, size_t& offset)
        {
            static const struct
            {
                const char* name;
                size_t      offset;
            } kRegisters[] = {
                {"eax", offsetof(safety::Registers, eax)}, {"ecx", offsetof(safety::Registers, ecx)},
                {"edx", offsetof(safety::Registers, edx)}, {"ebx", offsetof(safety::Registers, ebx)},
                {"esi", offsetof(safety::Registers, esi)}, {"edi", offsetof(safety::Registers, edi)},
                {"ebp", offsetof(safety::Registers, ebp)}, {"esp", offsetof(safety::Registers, esp)},
            };
            for (const auto& reg : kRegisters)
                if (length == 3 && strncmp(reg.name, name, 3) == 0)
                {
                    offset = reg.offset;
                    return true;
                }
            return false;
        }

        bool Parse(Site& site)
        {
            site.count = 0;
            for (const char* at = site.args; *at;)
            {
                if (site.count == kMaxArgs) return false;
                const char* end = strchr(at, ',');
                if (!end) end = at + strlen(at);
                Source& source = site.sources[site.count++];
                if (*at == '[')
                {
                    source.memory = true;
                    if (!Register(at + 1, 3, source.reg)) return false;
                    const char sign = at[4];
                    if (sign != '+' && sign != '-') return false;
                    source.offset = static_cast<intptr_t>(strtol(at + 5, nullptr, 0)) * (sign == '-' ? -1 : 1);
                }
                else if (!Register(at, static_cast<size_t>(end - at), source.reg))
                    return false;
                at = *end ? end + 1 : end;
            }
            return true;
        }

        /// A mid-function hook is handed the registers and nothing else, so each site gets a
        /// function of its own: one instance per position in Sites(), up to kSiteSlots.
        constexpr size_t kSiteSlots = 64;

        template <size_t S>
        void SiteHit(safety::Registers& registers)
        {
            const Site& site = Sites()[S];
            void*       args[kMaxArgs];
            for (uint32_t i = 0; i < site.count; ++i)
            {
                const Source& source = site.sources[i];
                uintptr_t*    reg    = reinterpret_cast<uintptr_t*>(reinterpret_cast<char*>(&registers) + source.reg);
                args[i]              = source.memory ? reinterpret_cast<void*>(*reg + source.offset) : reg;
            }
            EnterInlined(site.index, args);
        }

        template <size_t... S>
        constexpr std::array<safety::MidFn, sizeof...(S)> SiteFns(std::index_sequence<S...>)
        {
            return {&SiteHit<S>...};
        }

        constexpr std::array<safety::MidFn, kSiteSlots> kSiteFns = SiteFns(std::make_index_sequence<kSiteSlots>{});

        // --- The rows of this runtime -----------------------------------------------------

        Detoured* g_byIndex[kCount > 0 ? kCount : 1] = {};
        wxl::hook::Id g_links[kCount > 0 ? kCount : 1] = {};
        bool          g_placed[kCount > 0 ? kCount : 1] = {};

        void IndexRows()
        {
            static bool done = false;
            if (done) return;
            done = true;
            for (Detoured& row : g_rows) g_byIndex[row.index] = &row;
        }

        bool ArmSites(uint32_t index, char* why, size_t capacity)
        {
            for (size_t slot = 0; slot < Sites().size(); ++slot)
            {
                Site& site = Sites()[slot];
                if (site.index != index) continue;
                if (slot >= kSiteSlots)
                {
                    snprintf(why, capacity, "it has a site past the %zu the runtime serves", kSiteSlots);
                    return false;
                }
                if (!site.hook)
                {
                    if (!Parse(site) || site.count != RowOf(index).arg_count)
                    {
                        snprintf(why, capacity, "its site at 0x%08zX names its arguments as `%s`, which is not %u "
                                                "register(s) or stack slot(s)",
                                 static_cast<size_t>(site.address), site.args, RowOf(index).arg_count);
                        return false;
                    }
                    site.hook = safety::CreateMid(reinterpret_cast<void*>(site.address), kSiteFns[slot], why, capacity);
                    if (!site.hook) return false;
                }
                if (!safety::ArmMid(site.hook, why, capacity)) return false;
            }
            return true;
        }

        void DisarmSites(uint32_t index)
        {
            for (Site& site : Sites())
            {
                if (site.index != index || !site.hook) continue;
                char why[160] = "";
                if (!safety::DisarmMid(site.hook, why, sizeof why))
                    WLOG_WARN(kTag, "%s: its site at 0x%08zX stays hooked: %s", RowOf(index).name,
                              static_cast<size_t>(site.address), why);
            }
        }
    }

    bool Has(uint32_t index, const char** why)
    {
        IndexRows();
        if (index >= kCount) return false;
        if (const Deinlined* row = DeinlinedOf(index))
            return Has(row->parent, why);
        if (!g_byIndex[index])
        {
            if (why)
            {
                *why = "x64 only";
                for (const Absent& absent : kAbsent)
                    if (absent.index == index) *why = absent.why;
            }
            return false;
        }
        if (!g_placed[index] && !image::IsClient())
        {
            if (why) *why = "the executable is not Wow.exe build 12340, whose addresses the points are";
            return false;
        }
        return true;
    }

    bool Arm(uint32_t index, char* why, size_t capacity)
    {
        IndexRows();
        if (const Deinlined* deinlined = DeinlinedOf(index))
        {
            const uint32_t parent = deinlined->parent;
            Detoured*      host   = g_byIndex[parent];
            if (!host)
            {
                snprintf(why, capacity, "its parent %s has no x86 detour", RowOf(parent).name);
                return false;
            }
            if (g_bodyUsers[parent] == 0)
            {
                char name[160];
                snprintf(name, sizeof name, "%s (recompiled)", RowOf(parent).name);
                void* const body = g_bodyPlaced[parent] ? g_bodyPlaced[parent] : deinlined->body();
                const wxl::hook::Id link =
                    wxl::hook::Attach(name, reinterpret_cast<void*>(host->address), body, &g_bodyNext[parent],
                                      kBodyPriority, wxl::hook::Backend::SafetyHook, host->length, why, capacity);
                if (link == wxl::hook::kNone) return false;
                g_bodyLinks[parent] = link;
                WLOG_INFO(kTag, "%s runs its recompiled body: %s is a function of its own in it", RowOf(parent).name,
                          RowOf(index).name);
            }
            ++g_bodyUsers[parent];
            deinlined::g_armed[index] = 1;
            return true;
        }
        Detoured* row = index < kCount ? g_byIndex[index] : nullptr;
        if (!row)
        {
            snprintf(why, capacity, "it has no x86 detour");
            return false;
        }
        const wxl::hook::Id link =
            wxl::hook::Attach(RowOf(index).name, reinterpret_cast<void*>(row->address), row->detour, row->original,
                              kPriority, wxl::hook::Backend::SafetyHook, row->length, why, capacity);
        if (link == wxl::hook::kNone) return false;
        if (!ArmSites(index, why, capacity))
        {
            wxl::hook::Detach(link);
            DisarmSites(index);
            return false;
        }
        g_links[index] = link;
        return true;
    }

    void Disarm(uint32_t index)
    {
        if (const Deinlined* deinlined = DeinlinedOf(index))
        {
            const uint32_t parent = deinlined->parent;
            if (!deinlined::g_armed[index]) return;
            deinlined::g_armed[index] = 0;
            if (g_bodyUsers[parent] && --g_bodyUsers[parent] == 0 && g_bodyLinks[parent] != wxl::hook::kNone)
            {
                wxl::hook::Detach(g_bodyLinks[parent]);
                g_bodyLinks[parent] = wxl::hook::kNone;
                WLOG_INFO(kTag, "%s runs Wow.exe's own body again", RowOf(parent).name);
            }
            return;
        }
        if (index >= kCount || g_links[index] == wxl::hook::kNone) return;
        wxl::hook::Detach(g_links[index]);
        g_links[index] = wxl::hook::kNone;
        DisarmSites(index);
    }

    namespace x86
    {
        void Place(uint32_t index, uintptr_t address, size_t length)
        {
            IndexRows();
            if (index >= kCount || !g_byIndex[index]) return;
            g_byIndex[index]->address = address;
            g_byIndex[index]->length  = length;
            g_placed[index]           = true;
        }

        void AddSite(uint32_t index, uintptr_t address, const char* args)
        {
            Sites().push_back(Site{index, address, args});
        }

        size_t SiteCount() { return Sites().size(); }

        void* DetourOf(uint32_t index)
        {
            IndexRows();
            return index < kCount && g_byIndex[index] ? g_byIndex[index]->detour : nullptr;
        }

        void PlaceBody(uint32_t parent, void* body)
        {
            if (parent < kCount) g_bodyPlaced[parent] = body;
        }

        bool BodyInPlace(uint32_t parent) { return parent < kCount && g_bodyLinks[parent] != wxl::hook::kNone; }
    }
}
