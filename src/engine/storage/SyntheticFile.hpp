// SPDX-License-Identifier: GPL-3.0-or-later
// A file handle over bytes in memory, laid out like the client's own so its code can read one.

#pragma once

#include <cstdint>

namespace wxl::storage
{
    /// The client's 0x30-byte file handle: size, buffer and position sit where its code reads them.
    /// A native handle holds a small kind at +0x00; ours holds kSyntheticMagic.
#pragma pack(push, 1)
    struct SyntheticFile
    {
        uint32_t magic;
        uint32_t reserved04;
        uint32_t reserved08;
        char*    shortName;
        char*    fullName;
        uint32_t size;
        uint8_t* buffer;
        uint32_t position;
        uint32_t reserved20;
        uint32_t reserved24;
        void*    reserved28;
        void*    reserved2c;
    };
#pragma pack(pop)

    constexpr uint32_t kSyntheticMagic = 0x464C5857; // 'WXLF'
    constexpr uint32_t kNativeFileSize = 0x30;
    static_assert(sizeof(SyntheticFile) == kNativeFileSize, "SyntheticFile must match the client's file handle");

    /// Where a seek counts from, as the client passes it.
    enum class SeekFrom : uint32_t { Begin = 0, Current = 1, End = 2 };

    /// A handle owning a copy of `size` bytes; null when memory runs out.
    SyntheticFile* MakeSyntheticFile(const char* name, const uint8_t* data, uint32_t size);

    bool IsSynthetic(const void* handle);

    /// Copies up to `length` bytes from the position; true only when all of them were there.
    bool ReadSynthetic(SyntheticFile& file, void* destination, uint32_t length, uint32_t* read);

    /// Moves the position, clamped to the bytes; answers it.
    uint32_t SeekSynthetic(SyntheticFile& file, int32_t distance, SeekFrom from);

    void FreeSynthetic(SyntheticFile* file);
}
