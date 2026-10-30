// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The floor and the choice among the devices that meet it.

#include "gx/vk/device/Floor.hpp"

#include <cstdio>
#include <cstring>

namespace
{
    /// The device type as a rank: the one criterion that is a preference rather than a tie-break.
    unsigned TypeRank(VkPhysicalDeviceType type)
    {
        switch (type)
        {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
        default: return 0;
        }
    }

    char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

    bool ContainsNoCase(const char* text, const char* part)
    {
        const size_t length = std::strlen(part);
        if (length == 0) return false;
        for (; *text; ++text)
        {
            size_t i = 0;
            while (i < length && text[i] && Lower(text[i]) == Lower(part[i])) ++i;
            if (i == length) return true;
        }
        return false;
    }
}

namespace wxl::vk::device
{
    const char* TypeName(VkPhysicalDeviceType type)
    {
        switch (type)
        {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
        default: return "other device";
        }
    }

    void FormatDriver(uint32_t vendor, uint32_t driver, char* out, uint32_t size)
    {
        if (vendor == 0x10DE)
            std::snprintf(out, size, "%u.%u.%u", driver >> 22, (driver >> 14) & 0xFF, (driver >> 6) & 0xFF);
#if defined(_WIN32)
        else if (vendor == 0x8086)
            std::snprintf(out, size, "%u.%u", driver >> 14, driver & 0x3FFF);
#endif
        else
            FormatVersion(driver, out, size);
    }

    void Judge(Candidate& candidate, const Floor& floor)
    {
        candidate.reject[0] = '\0';
        char have[32], need[32];
        if (candidate.api < floor.api)
        {
            FormatVersion(candidate.api, have, sizeof have);
            FormatVersion(floor.api, need, sizeof need);
            std::snprintf(candidate.reject, sizeof candidate.reject, "Vulkan %s, below the %u.%u floor", have,
                          VK_API_VERSION_MAJOR(floor.api), VK_API_VERSION_MINOR(floor.api));
            return;
        }
        if (candidate.family == UINT32_MAX)
        {
            std::snprintf(candidate.reject, sizeof candidate.reject, "no graphics queue");
            return;
        }
#define WXL_VK_FEATURE_REQUIRED(member, version, required, where)                                                    \
    if (required && !candidate.features.member)                                                               \
    {                                                                                                         \
        std::snprintf(candidate.reject, sizeof candidate.reject, "missing feature %s (Vulkan %s)", #member, version); \
        return;                                                                                               \
    }
        WXL_VK_FEATURES(WXL_VK_FEATURE_REQUIRED)
#undef WXL_VK_FEATURE_REQUIRED
        if (floor.present)
        {
            if (!candidate.extensions.swapchain)
            {
                std::snprintf(candidate.reject, sizeof candidate.reject, "missing extension %s",
                              VK_KHR_SWAPCHAIN_EXTENSION_NAME);
                return;
            }
            if (!candidate.presents)
            {
                std::snprintf(candidate.reject, sizeof candidate.reject, "its graphics queue cannot present to a window");
                return;
            }
        }
    }

    const char* WhyBetter(const Candidate& a, const Candidate& b)
    {
        // A discrete GPU beats an integrated one, both beat a software device: on a laptop with
        // switchable graphics the first one enumerated is often the integrated part.
        if (TypeRank(a.type) != TypeRank(b.type))
            return TypeRank(a.type) > TypeRank(b.type) ? "the better device type" : nullptr;
        if (a.api != b.api) return a.api > b.api ? "a higher Vulkan version" : nullptr;
        // Among two of one type, the larger device memory stands in for the bigger GPU.
        if (a.device_local != b.device_local) return a.device_local > b.device_local ? "more device memory" : nullptr;
        if (a.index != b.index) return a.index < b.index ? "the lower index, the two being alike" : nullptr;
        return nullptr;
    }

    bool Names(const Candidate& candidate, const char* preference)
    {
        if (!preference || !*preference) return false;
        bool     digits = true;
        unsigned index  = 0;
        for (const char* p = preference; *p; ++p)
        {
            if (*p < '0' || *p > '9' || index > 1000)
            {
                digits = false;
                break;
            }
            index = index * 10 + static_cast<unsigned>(*p - '0');
        }
        if (digits) return candidate.index == index;
        return ContainsNoCase(candidate.name, preference);
    }

    Choice Choose(const Candidate* candidates, uint32_t count, const char* preference)
    {
        Choice choice;
        if (preference && *preference)
        {
            int named = -1;
            for (uint32_t i = 0; i < count && named < 0; ++i)
                if (Names(candidates[i], preference)) named = static_cast<int>(i);
            if (named >= 0 && !candidates[named].reject[0])
            {
                choice.index = named;
                std::snprintf(choice.why, sizeof choice.why, "named by [gx] gpu = %.60s", preference);
                return choice;
            }
            if (named >= 0)
                std::snprintf(choice.note, sizeof choice.note, "[gx] gpu = %.40s names %.80s, which does not qualify (%.60s)",
                              preference, candidates[named].name, candidates[named].reject);
            else
                std::snprintf(choice.note, sizeof choice.note, "[gx] gpu = %.60s names no device", preference);
        }

        int         best = -1;
        const char* why  = "the only one that qualifies";
        for (uint32_t i = 0; i < count; ++i)
        {
            if (candidates[i].reject[0]) continue;
            if (best < 0)
            {
                best = static_cast<int>(i);
                continue;
            }
            if (const char* better = WhyBetter(candidates[i], candidates[best]))
            {
                best = static_cast<int>(i);
                why  = better;
            }
            else if (std::strcmp(why, "the only one that qualifies") == 0)
                why = "nothing beat it";
        }
        choice.index = best;
        if (best >= 0) std::snprintf(choice.why, sizeof choice.why, "%s", why);
        return choice;
    }

    void Refusals(const Candidate* candidates, uint32_t count, char* out, uint32_t size)
    {
        if (!size) return;
        out[0]      = '\0';
        size_t used = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (!candidates[i].reject[0]) continue;
            const int wrote = std::snprintf(out + used, size - used, "%s%s: %s", used ? "; " : "", candidates[i].name,
                                            candidates[i].reject);
            if (wrote < 0 || static_cast<size_t>(wrote) >= size - used) break;
            used += static_cast<size_t>(wrote);
        }
    }
}
