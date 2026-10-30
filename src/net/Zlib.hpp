// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// zlib streams for wxl.net: the compressed opcodes the runtime inflates, and the deflate and
// inflate a mod is offered. stb_image's inflater and stb_image_write's deflater (deps/stb, MIT or
// public domain), each in this one translation unit.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wxl::core::zlib
{
    /// A zlib stream (header, deflate, adler32) of the bytes. False when it cannot be made.
    bool Deflate(const void* data, size_t size, std::vector<uint8_t>& out);

    /// Inflates a stream that must hold exactly @p size bytes into @p out. False for a stream
    /// that is not zlib, is cut short, or holds more or fewer bytes.
    bool InflateExact(const void* data, size_t size_in, void* out, size_t size);

    /// Inflates a stream of unknown size, at most @p max bytes. False for a stream that is not
    /// zlib, is cut short, or holds more than @p max.
    bool Inflate(const void* data, size_t size, size_t max, std::vector<uint8_t>& out);
}
