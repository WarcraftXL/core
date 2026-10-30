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

        // --- De-inlined points: recompiled parent bodies in which they are functions ------------

        /// After every seam and wxl.hook link and the parent's own point: the recompiled body
        /// takes the place of Wow.exe's, and whatever ran in front of it still does.
        constexpr int kBodyPriority = INT_MAX;

        /// A function whose body the runtime recompiled: a point of its own, or a function of
        /// Wow.exe that is no point. Per parent: the body in place (a link of its chain), how many
        /// armed points need it, and the body the harness put in place of the generated one.
        struct ParentRow
        {
            const char*   name;
            uintptr_t     address; ///< in Wow.exe 12340, unless placed
            size_t        length;
            void*       (*body)();
            uint32_t      point  = kCount; ///< the point it is, or kCount
            bool          placed = false;
            wxl::hook::Id link   = wxl::hook::kNone;
            uint32_t      users  = 0;
            void*         next   = nullptr;
            void*         substitute = nullptr;
        };

        ParentRow g_parents[deinlined::kParentCount > 0 ? deinlined::kParentCount : 1] = {
#define WXL_POINT_X86(Ident, name, address, length, conv, ret, params)
#define WXL_POINT_SITE(Ident, address, args)
#define WXL_POINT_X86_ABSENT(Ident, why)
#define WXL_POINT_X86_PARENT(Parent, name, address, length, conv, ret, params) \
    {name, address, length, &deinlined::Body_##Parent},
#include "wxl/points/x86.def"
#undef WXL_POINT_X86_PARENT
#undef WXL_POINT_X86_ABSENT
#undef WXL_POINT_SITE
#undef WXL_POINT_X86
        };

        /// One function a de-inlined point is inlined into.
        struct DeinlinedIn
        {
            uint32_t index;
            uint32_t parent;
        };

        const DeinlinedIn kDeinlinedIn[] = {
            {kCount, deinlined::kParentCount},
#define WXL_POINT_X86(Ident, name, address, length, conv, ret, params)
#define WXL_POINT_SITE(Ident, address, args)
#define WXL_POINT_X86_ABSENT(Ident, why)
#define WXL_POINT_X86_DEINLINED_IN(Ident, Parent) {k##Ident, deinlined::kParent_##Parent},
#include "wxl/points/x86.def"
#undef WXL_POINT_X86_DEINLINED_IN
#undef WXL_POINT_X86_ABSENT
#undef WXL_POINT_SITE
#undef WXL_POINT_X86
        };

        bool IsDeinlined(uint32_t index)
        {
            for (const DeinlinedIn& row : kDeinlinedIn)
                if (row.index == index) return true;
            return false;
        }

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
            for (uint32_t p = 0; p < deinlined::kParentCount; ++p)
                for (uint32_t i = 0; i < kCount; ++i)
                    if (strcmp(RowOf(i).name, g_parents[p].name) == 0) g_parents[p].point = i;
        }

        /// Where a parent's body is: its point's (which the harness may move), or its own.
        void ParentAt(const ParentRow& parent, uintptr_t& address, size_t& length)
        {
            if (parent.point < kCount && g_byIndex[parent.point])
            {
                address = g_byIndex[parent.point]->address;
                length  = g_byIndex[parent.point]->length;
                return;
            }
            address = parent.address;
            length  = parent.length;
        }

        void ReleaseBody(ParentRow& parent)
        {
            if (parent.users && --parent.users == 0 && parent.link != wxl::hook::kNone)
            {
                wxl::hook::Detach(parent.link);
                parent.link = wxl::hook::kNone;
                WLOG_INFO(kTag, "%s runs Wow.exe's own body again", parent.name);
            }
        }

        /// The parent's recompiled body put in place, or one more point counted on it.
        bool HoldBody(ParentRow& parent, uint32_t index, char* why, size_t capacity)
        {
            if (parent.users == 0)
            {
                uintptr_t address = 0;
                size_t    length  = 0;
                ParentAt(parent, address, length);
                char name[200];
                snprintf(name, sizeof name, "%s (recompiled)", parent.name);
                void* const         body = parent.substitute ? parent.substitute : parent.body();
                const wxl::hook::Id link = wxl::hook::Attach(name, reinterpret_cast<void*>(address), body, &parent.next,
                                                             kBodyPriority, wxl::hook::Backend::SafetyHook, length, why,
                                                             capacity);
                if (link == wxl::hook::kNone) return false;
                parent.link = link;
                WLOG_INFO(kTag, "%s runs its recompiled body: %s is a function of its own in it", parent.name,
                          RowOf(index).name);
            }
            ++parent.users;
            return true;
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
        if (IsDeinlined(index))
        {
            for (const DeinlinedIn& row : kDeinlinedIn)
            {
                if (row.index != index) continue;
                const ParentRow& parent = g_parents[row.parent];
                if (parent.point < kCount)
                {
                    if (!Has(parent.point, why)) return false;
                }
                else if (!parent.placed && !image::IsClient())
                {
                    if (why) *why = "the executable is not Wow.exe build 12340, whose addresses the points are";
                    return false;
                }
            }
            return true;
        }
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
        if (IsDeinlined(index))
        {
            if (deinlined::g_armed[index]) return true;
            for (const DeinlinedIn& row : kDeinlinedIn)
            {
                if (row.index != index) continue;
                ParentRow& parent = g_parents[row.parent];
                if (parent.point < kCount && !g_byIndex[parent.point])
                    snprintf(why, capacity, "its parent %s has no x86 detour", parent.name);
                else if (HoldBody(parent, index, why, capacity))
                    continue;
                // All of its parents or none: those held so far are given back.
                for (const DeinlinedIn& held : kDeinlinedIn)
                {
                    if (&held == &row) break;
                    if (held.index == index) ReleaseBody(g_parents[held.parent]);
                }
                return false;
            }
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
        if (IsDeinlined(index))
        {
            if (!deinlined::g_armed[index]) return;
            deinlined::g_armed[index] = 0;
            for (const DeinlinedIn& row : kDeinlinedIn)
                if (row.index == index) ReleaseBody(g_parents[row.parent]);
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

        void PlaceParent(uint32_t parent, uintptr_t address, size_t length)
        {
            if (parent >= deinlined::kParentCount) return;
            g_parents[parent].address = address;
            g_parents[parent].length  = length;
            g_parents[parent].placed  = true;
        }

        void PlaceBody(uint32_t parent, void* body)
        {
            if (parent < deinlined::kParentCount) g_parents[parent].substitute = body;
        }

        bool BodyInPlace(uint32_t parent)
        {
            return parent < deinlined::kParentCount && g_parents[parent].link != wxl::hook::kNone;
        }
    }
}
