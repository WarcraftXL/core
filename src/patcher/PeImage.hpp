// SPDX-License-Identifier: GPL-3.0-or-later
// A 32-bit PE file in memory, edited in place: the large-address flag and one more import.

#pragma once

#include <cstdint>
#include <vector>

namespace wxl::patcher
{
    /// The headers are found again on each call: AddImport grows the buffer.
    class PeImage
    {
    public:
        explicit PeImage(std::vector<uint8_t>& bytes) : m_bytes(bytes) {}

        /// A 32-bit PE with sane headers.
        bool Valid() const;

        /// A section of that name exists (compared on its 8 bytes).
        bool HasSection(const char* name) const;

        void SetLargeAddressAware();

        /// Imports dll!func through a new section named `tagSection`; true at once when it exists.
        bool AddImport(const char* dll, const char* func, const char* tagSection);

    private:
        std::vector<uint8_t>& m_bytes;
    };
}
