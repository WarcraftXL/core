// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.point on the stock client: what the harness reaches of the x86 realisation.

#pragma once

#include "points/Points.hpp"

#include <cstddef>
#include <cstdint>

namespace wxl::core::points::x86
{
    /// Moves a point's detour to another function of its shape, which then counts as the
    /// client's: the harness arms points on stand-ins in its own image.
    void Place(uint32_t index, uintptr_t address, size_t length);

    /// Adds a site of a point the same way, before the point is first attached.
    void AddSite(uint32_t index, uintptr_t address, const char* args);

    /// How many sites there are: x86.def's and those added.
    size_t SiteCount();

    /// The generated detour of a point, or null when x86 has none.
    void* DetourOf(uint32_t index);

    /// Moves a parent that is no point (deinlined::kParent_<Parent>) to another function of its
    /// shape, which then counts as the client's, as Place does for a point (a parent that is a
    /// point follows its point).
    void PlaceParent(uint32_t parent, uintptr_t address, size_t length);

    /// Puts another body in place of a parent's recompiled one (src/deinlined/), for the next time
    /// a de-inlined point of it is armed: the harness runs one of its own on a stand-in.
    void PlaceBody(uint32_t parent, void* body);

    /// Whether a parent (deinlined::kParent_<Parent>) runs its recompiled body right now.
    bool BodyInPlace(uint32_t parent);
}
