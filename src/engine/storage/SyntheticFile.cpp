// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/storage/SyntheticFile.hpp"

#include <cstdlib>
#include <cstring>

namespace wxl::storage
{
    namespace
    {
        /// The client frees nothing of ours: every block is malloc'd and released by FreeSynthetic.
        char* CopyName(const char* name)
        {
            const size_t length = std::strlen(name) + 1;
            auto*        copy   = static_cast<char*>(std::malloc(length));
            if (copy) std::memcpy(copy, name, length);
            return copy;
        }
    }

    SyntheticFile* MakeSyntheticFile(const char* name, const uint8_t* data, uint32_t size)
    {
        auto* file = static_cast<SyntheticFile*>(std::calloc(1, sizeof(SyntheticFile)));
        if (!file) return nullptr;
        file->buffer = static_cast<uint8_t*>(std::malloc(size ? size : 1));
        if (!file->buffer)
        {
            std::free(file);
            return nullptr;
        }
        if (size && data) std::memcpy(file->buffer, data, size);
        file->magic     = kSyntheticMagic;
        file->size      = size;
        file->fullName  = CopyName(name);
        file->shortName = file->fullName;
        return file;
    }

    bool IsSynthetic(const void* handle)
    {
        return handle && *static_cast<const uint32_t*>(handle) == kSyntheticMagic;
    }

    bool ReadSynthetic(SyntheticFile& file, void* destination, uint32_t length, uint32_t* read)
    {
        const uint32_t available = file.position < file.size ? file.size - file.position : 0;
        const uint32_t count     = length < available ? length : available;
        if (count) std::memcpy(destination, file.buffer + file.position, count);
        file.position += count;
        if (read) *read = count;
        return count == length;
    }

    uint32_t SeekSynthetic(SyntheticFile& file, int32_t distance, SeekFrom from)
    {
        int64_t base = 0;
        if (from == SeekFrom::Current) base = file.position;
        else if (from == SeekFrom::End) base = file.size;
        int64_t position = base + distance;
        if (position < 0) position = 0;
        if (position > file.size) position = file.size;
        file.position = static_cast<uint32_t>(position);
        return file.position;
    }

    void FreeSynthetic(SyntheticFile* file)
    {
        std::free(file->buffer);
        std::free(file->fullName);
        std::free(file);
    }
}
