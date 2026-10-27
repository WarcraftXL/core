// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The seams as functions: the entry points of every row of the seam table.

#ifndef WXL_SEAMS_H
#define WXL_SEAMS_H

// Every seam of seams.def declared as its entry points, wxl::seam::<domain>::<name>::Before,
// ::After or ::Gate, taking the row's parameters. Both builds of the runtime define and export them;
// what differs is who calls them: on x86 the runtime's own detour at the function's address, on
// x64 the recompiled client from inside the function, which is why this header is C++ and the
// client includes it. A seam's body raises the events the call justifies, and nothing else.
//
// An observing seam, WXL_SEAM, is told what happened:
//
//   Before takes the row's parameters. After takes them too, and when the row returns a value,
//   that value first: a row `WXL_SEAM(MapArea, PrepareChunk, ..., void*, thiscall, (void* self,
//   int32_t x, int32_t y))` declares Before(self, x, y) and After(result, self, x, y). A
//   thiscall row's first parameter is `this`. WXL_SEAM_BEFORE declares only Before and
//   WXL_SEAM_AFTER only After; the fork calls only what is declared.
//
// A gated seam, WXL_SEAM_GATE, stands in the doorway and has one entry point:
//
//   Gate takes every parameter by address and answers whether it has taken the call. True: the
//   function does not run, and what it answers instead has been written through the extra
//   first parameter, a pointer to the return value, which a void row does not have. False: the
//   function runs, with whatever the seam left at those addresses, which is how an argument is
//   rewritten.
//
//   WXL_SEAM_GATE(NetClient, Send, ..., void, thiscall, (void* self, void* msg)) declares
//   bool Gate(void** self, void** msg). Taking the object by address is of no use to a seam,
//   but the shape is uniform, and a uniform shape is what lets one declaration drive both
//   realisations.
//
// A gated seam has no After. On x64 the call sites are the client's own source, and an After
// would have to be written at every return of the function rather than once at its head; a
// seam that wants to watch a function watches it with the observing shape.
//
// On x86 both shapes are one detour, differing in what it does around the original call. On
// x64 an observing seam is two statements wrapped around the function's body, a gated one is a
// single test at its head.
//
// C++17: the recompiled client compiles this header with its own language level.

#include <stdint.h>

#include <type_traits>

#include "wxl/abi.h"
#include "wxl/core/init.h"
#include "wxl/events.h"

#ifndef __cplusplus
#error "wxl/seams/seams.h declares C++ functions; include it from C++"
#endif

namespace wxl::seam::detail
{
    /// The calling conventions a row may declare, by the token of its conv column.
    struct conv_cdecl
    {
    };
    struct conv_thiscall
    {
    };
    struct conv_stdcall
    {
    };
    /// A function the compiler gave a private register convention. No generated detour can serve
    /// it; the x86 build attaches it through a thunk of src/seams/RegparmX86.cpp.
    struct conv_regparm
    {
    };

    /// The types of Before and After for an observing row: After takes a non-void result first.
    template <class Signature>
    struct Fn;

    template <class R, class... A>
    struct Fn<R(A...)>
    {
        using Before = void WXL_CALL(A...);
        using After  = void WXL_CALL(R, A...);
    };

    template <class... A>
    struct Fn<void(A...)>
    {
        using Before = void WXL_CALL(A...);
        using After  = void WXL_CALL(A...);
    };

    /**
     * @brief The type of Gate for a gated row.
     *
     * Every parameter crosses by address, so the seam can rewrite what the function will see.
     * Gate answers whether it has taken the call, writing what to answer instead through the
     * leading result pointer, which a void row does not have.
     */
    template <class Signature>
    struct GateFn;

    template <class R, class... A>
    struct GateFn<R(A...)>
    {
        using Gate = bool WXL_CALL(R*, A*...);
    };

    template <class... A>
    struct GateFn<void(A...)>
    {
        using Gate = bool WXL_CALL(A*...);
    };

    /// Whether a return type comes back in a register: nothing, or a scalar of at most 8 bytes.
    template <class R>
    struct RegisterReturn
    {
        static constexpr bool ok = std::is_scalar_v<R> && sizeof(R) <= 8;
    };

    template <>
    struct RegisterReturn<void>
    {
        static constexpr bool ok = true;
    };

    /// What the x86 detour can carry for a convention: a thiscall row takes `this` first, as a
    /// pointer, and returns nothing or a value that fits a register.
    template <class Conv, class Signature>
    struct Shape
    {
        static constexpr bool ok = true;
    };

    template <class R, class This, class... A>
    struct Shape<conv_thiscall, R(This, A...)>
    {
        static constexpr bool ok = std::is_pointer_v<This> && RegisterReturn<R>::ok;
    };

    template <class R>
    struct Shape<conv_thiscall, R()>
    {
        static constexpr bool ok = false;
    };
} // namespace wxl::seam::detail

namespace wxl::seam
{
    /**
     * @brief Whether the seam that raises @p event is wanted: some mod listens to it, or the
     *        runtime's own work rides on that seam. The flag the runtime keeps per event
     *        (wxl_core_listening).
     *
     * A call site in the client tests it before a busy seam that raises that event, so the seam
     * costs a load and a branch while nobody needs it, not a call into the runtime:
     * `if (wxl::seam::Listening(WXL_EV_TextureLoad)) wxl::seam::Texture::Create::Before(...)`.
     * The seam bodies ask the same question of the host themselves, so a call made anyway is
     * still correct.
     */
    inline bool Listening(wxl_event event)
    {
        return wxl_core_listening[event] != 0;
    }
}

namespace wxl::seam
{
// What every row's namespace holds before its entry points: the signature, and the check of it
// against the convention. The convention itself is pasted by each shape: handed on to this macro,
// a conv column such as `cdecl` would be expanded first, and the Windows headers define it.
#define WXL_SEAM_HEAD(domain, name, ret, params)                                             \
    using Signature = ret params;                                                            \
    static_assert(::wxl::seam::detail::Shape<Convention, Signature>::ok,                     \
                  "seam " #domain "." #name ": a thiscall row takes `this` first, as a "     \
                  "pointer, and returns void or a scalar");
#define WXL_SEAM(domain, name, symbol, ret, conv, params)                                    \
    namespace domain::name                                                                   \
    {                                                                                        \
        using Convention = ::wxl::seam::detail::conv_##conv;                                 \
        WXL_SEAM_HEAD(domain, name, ret, params)                                             \
        WXL_CORE_API ::wxl::seam::detail::Fn<Signature>::Before Before;                      \
        WXL_CORE_API ::wxl::seam::detail::Fn<Signature>::After After;                        \
    }
#define WXL_SEAM_BEFORE(domain, name, symbol, ret, conv, params)                             \
    namespace domain::name                                                                   \
    {                                                                                        \
        using Convention = ::wxl::seam::detail::conv_##conv;                                 \
        WXL_SEAM_HEAD(domain, name, ret, params)                                             \
        WXL_CORE_API ::wxl::seam::detail::Fn<Signature>::Before Before;                      \
    }
#define WXL_SEAM_AFTER(domain, name, symbol, ret, conv, params)                              \
    namespace domain::name                                                                   \
    {                                                                                        \
        using Convention = ::wxl::seam::detail::conv_##conv;                                 \
        WXL_SEAM_HEAD(domain, name, ret, params)                                             \
        WXL_CORE_API ::wxl::seam::detail::Fn<Signature>::After After;                        \
    }
#define WXL_SEAM_GATE(domain, name, symbol, ret, conv, params)                               \
    namespace domain::name                                                                   \
    {                                                                                        \
        using Convention = ::wxl::seam::detail::conv_##conv;                                 \
        WXL_SEAM_HEAD(domain, name, ret, params)                                             \
        WXL_CORE_API ::wxl::seam::detail::GateFn<Signature>::Gate Gate;                      \
    }
#include "wxl/seams/seams.def"
#undef WXL_SEAM_GATE
#undef WXL_SEAM_AFTER
#undef WXL_SEAM_BEFORE
#undef WXL_SEAM
#undef WXL_SEAM_HEAD
} // namespace wxl::seam

#endif // WXL_SEAMS_H
