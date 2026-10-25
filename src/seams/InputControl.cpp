// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The InputControl seams: the game's control bits, where wxl.input keeps its mouse look out.

#include "wxl/seams/seams.h"

#include "services/InputService.hpp"

namespace wxl::seam::InputControl::SetControlBit
{
    bool WXL_CALL Gate(int32_t* result, void** self, uint32_t* bit, uint32_t* time)
    {
        return wxl::core::services::input::SetControlBit(result, *self, bit, *time);
    }
}

namespace wxl::seam::InputControl::UnsetControlBit
{
    bool WXL_CALL Gate(int32_t* result, void** self, uint32_t* bit, uint32_t* time, int32_t*)
    {
        return wxl::core::services::input::UnsetControlBit(result, *self, bit, *time);
    }
}
