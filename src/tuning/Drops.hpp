// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The calls of Wow.exe whose answer is dropped and whose callee does nothing else, taken out
// (include/wxl/tuning/drops_x86.def): a five-byte NOP over each `call` whose setting is on.

#pragma once

#include <cstdint>

namespace wxl::core::tuning::drops
{
    /// What one row found: dropped, already a NOP, or other bytes (left as they are).
    enum class Outcome : uint8_t
    {
        dropped,
        already,
        foreign,
    };

    /**
     * @brief Writes the NOP over the five bytes at @p code when they are `call @p callee` as it
     *        reads at @p at (the address the bytes run at). The caller makes them writable.
     */
    Outcome Drop(uint8_t* code, uintptr_t at, uintptr_t callee);

    /// x86: every row whose setting is on, in Wow.exe's code, logged in one line. Before the
    /// client's own threads start (the entry trampoline): no thread runs the bytes as they change.
    void Install();
}
