// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/hook/Registry.hpp"

#include "common/Log.hpp"

#include <vector>

namespace wxl::hook
{
    namespace
    {
        struct Feature
        {
            const char* name;
            bool        enabled;
            bool (*install)();
            Phase phase;
        };

        /// Made on first use: registrars run before main, in any order across files.
        std::vector<Feature>& Features()
        {
            static std::vector<Feature> features;
            return features;
        }
    }

    void RegisterFeature(const char* name, bool enabled, bool (*install)(), Phase phase)
    {
        Features().push_back(Feature{ name, enabled, install, phase });
    }

    int InstallRegisteredFeatures(Phase phase)
    {
        int installed = 0;
        for (const Feature& feature : Features())
        {
            if (feature.phase != phase) continue;
            if (!feature.enabled)
            {
                WLOG_DEBUG("feature: '%s' disabled", feature.name);
                continue;
            }
            if (!feature.install || !feature.install())
            {
                WLOG_ERROR("feature: install '%s' failed", feature.name);
                continue;
            }
            WLOG_DEBUG("feature: installed '%s'", feature.name);
            ++installed;
        }
        return installed;
    }
}
