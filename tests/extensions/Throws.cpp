// SPDX-License-Identifier: GPL-3.0-or-later
// Test extension: reads a position without testing the Result; Value() throws on the first tick.

#include "wxl/objects/Object.hpp"
#include "wxl/wxl.hpp"

using namespace wxl;

class Throws : public Extension
{
    void OnUpdate(float, uint32_t) override
    {
        const Object somebody(WXL_Handle{ 0, 1 });
        Log::Info("x {}", somebody.Position().Value().x);
    }
};

Export<Throws> extension;
