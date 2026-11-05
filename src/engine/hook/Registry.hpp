// SPDX-License-Identifier: GPL-3.0-or-later
// The core's features: each registers its installer before main, the boot runs them by phase.

#pragma once

namespace wxl::hook
{
    /// When an installer runs. The hook batch is enabled after each phase.
    enum class Phase
    {
        Boot,   ///< inside DllMain, before the client's own startup goes on
        Normal, ///< on the core's thread, once the graphics device exists
    };

    /// Records an installer; `enabled` false never installs it. An installer answers false on failure.
    void RegisterFeature(const char* name, bool enabled, bool (*install)(), Phase phase = Phase::Normal);

    /// Runs the enabled installers of one phase, in registration order; answers how many succeeded.
    int InstallRegisteredFeatures(Phase phase = Phase::Normal);

    struct FeatureRegistrar
    {
        FeatureRegistrar(const char* name, bool enabled, bool (*install)(), Phase phase = Phase::Normal)
        {
            RegisterFeature(name, enabled, install, phase);
        }
    };
}

#define WXL_HOOK_CONCAT_(a, b) a##b
#define WXL_HOOK_CONCAT(a, b) WXL_HOOK_CONCAT_(a, b)

/// Registers `fn` (bool()) as the Normal-phase installer of a feature; `toggle` false leaves it out.
#define WXL_REGISTER_FEATURE(name, toggle, fn)                                                  \
    namespace                                                                                   \
    {                                                                                           \
        const ::wxl::hook::FeatureRegistrar WXL_HOOK_CONCAT(g_wxlFeatureRegistrar_, __LINE__){ \
            name, toggle, fn                                                                    \
        };                                                                                      \
    }

/// The same in a given ::wxl::hook::Phase.
#define WXL_REGISTER_FEATURE_PHASED(name, toggle, fn, phase)                                    \
    namespace                                                                                   \
    {                                                                                           \
        const ::wxl::hook::FeatureRegistrar WXL_HOOK_CONCAT(g_wxlFeatureRegistrar_, __LINE__){ \
            name, toggle, fn, phase                                                             \
        };                                                                                      \
    }
