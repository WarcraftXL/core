// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// zlib streams through stb (deps/stb): stbi_zlib_decode_buffer inflates into a buffer of a known
// size and fails past it, stbi_zlib_compress deflates at its best level.

#include "net/Zlib.hpp"

#include <climits>
#include <cstdlib>
#include <cstring>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace wxl::core::zlib
{
    bool Deflate(const void* data, size_t size, std::vector<uint8_t>& out)
    {
        out.clear();
        if (size > static_cast<size_t>(INT_MAX) || (!data && size)) return false;
        unsigned char none   = 0;
        int           length = 0;
        unsigned char* stream = stbi_zlib_compress(
            const_cast<unsigned char*>(size ? static_cast<const unsigned char*>(data) : &none),
            static_cast<int>(size), &length, 8);
        if (!stream || length <= 0)
        {
            std::free(stream);
            return false;
        }
        out.assign(stream, stream + length);
        std::free(stream);
        return true;
    }

    bool InflateExact(const void* data, size_t size_in, void* out, size_t size)
    {
        if (!data || size_in > static_cast<size_t>(INT_MAX) || size > static_cast<size_t>(INT_MAX)) return false;
        // One byte more than wanted: a stream that fills it holds too much.
        std::vector<char> buffer(size + 1);
        const int got = stbi_zlib_decode_buffer(buffer.data(), static_cast<int>(buffer.size()),
                                                static_cast<const char*>(data), static_cast<int>(size_in));
        if (got < 0 || static_cast<size_t>(got) != size) return false;
        if (size) std::memcpy(out, buffer.data(), size);
        return true;
    }

    bool Inflate(const void* data, size_t size, size_t max, std::vector<uint8_t>& out)
    {
        out.clear();
        if (!data || size > static_cast<size_t>(INT_MAX) || max >= static_cast<size_t>(INT_MAX)) return false;
        // stb inflates into a buffer it may not grow: try larger ones up to the bound, one byte
        // past it so that a stream holding more than max fails.
        size_t capacity = size * 4 + 256;
        for (;;)
        {
            if (capacity > max + 1) capacity = max + 1;
            out.resize(capacity);
            const int got = stbi_zlib_decode_buffer(reinterpret_cast<char*>(out.data()), static_cast<int>(capacity),
                                                    static_cast<const char*>(data), static_cast<int>(size));
            if (got >= 0 && static_cast<size_t>(got) < capacity)
            {
                out.resize(static_cast<size_t>(got));
                return true;
            }
            if (capacity == max + 1)
            {
                out.clear();
                return false;
            }
            capacity *= 4;
        }
    }
}
