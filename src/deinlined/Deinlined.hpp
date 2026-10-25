// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// De-inlined points on the stock client: what the recompiled bodies of src/deinlined/ call.

#pragma once

#include "points/DetourX86.hpp"
#include "points/Points.hpp"

#include <cstdint>

// The 2008 compiler copied some functions into their callers, so Wow.exe has no address to detour
// for them. Such a point is listed with a parent (points.txt `deinlined` lines, include/wxl/
// points/x86.def WXL_POINT_X86_DEINLINED rows): a point whose function holds the copy. Its body
// is recompiled from the fork by core/tools/extract/extract.py --runtime into this folder, where
// the inlined function is a function of its own again, and the runtime puts that body in place of
// the parent's (the last link of the parent's chain, so the parent's own point and seams still
// run in front) while the de-inlined point has handlers. The de-inlined function's head tests one
// byte, as a point line of the recompiled client does, and enters the point when it is set.

namespace wxl::core::deinlined
{
    /// One byte per point, set while a de-inlined point has handlers on this runtime.
    extern volatile uint8_t g_armed[points::kCount > 0 ? points::kCount : 1];

    inline bool Armed(uint32_t index) { return g_armed[index] != 0; }

    /// What a de-inlined point's row says of it: its detour type (the point's contract signature,
    /// cdecl, a reference as its address) and the point whose body holds it.
    template <uint32_t Index>
    struct Row;

#define WXL_POINT_X86(...)
#define WXL_POINT_SITE(...)
#define WXL_POINT_X86_ABSENT(...)
#define WXL_POINT_X86_DEINLINED(Ident, name, Parent, ret, params)                                          \
    template <>                                                                                           \
    struct Row<points::k##Ident>                                                                          \
    {                                                                                                     \
        using Detour                     = points::DetourOf<points::k##Ident, ::wxl::seam::detail::conv_cdecl, ret params>; \
        static constexpr uint32_t kParent = points::k##Parent;                                            \
    };                                                                                                    \
    void* Body_##Parent();
#include "wxl/points/x86.def"
#undef WXL_POINT_X86_DEINLINED
#undef WXL_POINT_X86_ABSENT
#undef WXL_POINT_SITE
#undef WXL_POINT_X86

    /**
     * @brief Runs de-inlined point @p Index around @p body: its Before handlers, its Replace chain
     *        ending at @p body, its After handlers.
     *
     * @p args are the contract's: a parameter the fork takes by reference is passed by its
     * address, which is also how a cdecl function taking a reference receives it, so @p body is
     * called as the detour's original unchanged.
     */
    template <uint32_t Index, class Body, class... A>
    auto Enter(Body* body, A... args)
    {
        using Detour     = typename Row<Index>::Detour;
        Detour::original = reinterpret_cast<decltype(Detour::original)>(body);
        return Detour::Detour(args...);
    }
}

/// Ends a recompiled body's file: the function the runtime puts in place of point Parent's.
#define WXL_DEINLINED_BODY(Parent)                                                                        \
    namespace wxl::core::deinlined                                                                        \
    {                                                                                                     \
        void* Body_##Parent() { return reinterpret_cast<void*>(&Parent); }                                \
    }
