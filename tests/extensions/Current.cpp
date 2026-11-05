// SPDX-License-Identifier: GPL-3.0-or-later
// Test extension: built against the current SDK; logs each frame tick it receives.

#include "wxl/wxl.hpp"

using namespace wxl;

class Ticks : public Extension
{
    void OnUpdate(float, uint32_t timeMs) override
    {
        Log::Info("tick {}", timeMs);
    }
};

Export<Ticks> extension;
