// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A frame capture read back (wxl/core/gx_capture.h): its header checked, its chunks walked in
// order, the device's memory regions kept up to date as its MEM chunks come, so that at each event
// the reader holds the state the device had. What replays a capture through a renderer builds on
// this; it reads bytes only and knows no device. Portable: built natively for the tests.

#pragma once

#include "wxl/core/gx_capture.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wxl::gx::capture
{
    struct Chunk
    {
        uint32_t       id = 0;
        const uint8_t* body = nullptr;
        uint32_t       size = 0;
    };

    class Reader
    {
    public:
        /**
         * @brief Takes a capture's bytes (kept by the caller while the reader is used).
         * @return false with @p why: not a capture, another version, a header that does not fit.
         */
        bool Open(const uint8_t* data, size_t size, std::string& why);

        const wxl_gx_capture_header& Header() const { return header_; }

        /**
         * @brief The next chunk that is not a MEM (those are applied to the regions on the way).
         * @return false at the end, or with @p why set when the file is cut or a chunk malformed.
         */
        bool Next(Chunk& out, std::string& why);

        /// A region as the last MEM chunks left it: empty before its first.
        const std::vector<uint8_t>& Region(wxl_gx_capture_region region) const;

        /// MEM chunks applied so far.
        uint32_t Deltas() const { return deltas_; }

    private:
        bool Apply(const Chunk& chunk, std::string& why);

        const uint8_t*         data_ = nullptr;
        size_t                 size_ = 0;
        size_t                 at_ = 0;
        wxl_gx_capture_header  header_{};
        std::vector<uint8_t>   regions_[5];
        uint32_t               deltas_ = 0;
    };

    /// Reads a little-endian value at @p at of @p chunk; false when it does not fit.
    bool Read32(const Chunk& chunk, uint32_t at, uint32_t& out);
    bool Read64(const Chunk& chunk, uint32_t at, uint64_t& out);
}
