// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The x86 realisation of the client binding: what the attachment readies before the rows read.

#pragma once

#include "gx/d3d9/Highlight.hpp"

namespace wxl::core::binding::x86
{
    /**
     * @brief Records the calling thread as the client's main thread.
     *
     * Call from the attachment's Prepare, which runs on the thread the entry trampoline runs on.
     * A row that reaches the object manager's thread-local state answers its absent value from
     * any other thread.
     */
    void Prepare();

    /// Where wxl.highlight's renderer reads the stock client's device, the vertex program's
    /// constants and whether the client draws with shaders.
    gx::d3d9::highlight::Source HighlightSource();
}
