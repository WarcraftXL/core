// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/storage/StorageHook.hpp"

#include "common/Config.hpp"
#include "common/Log.hpp"
#include "engine/storage/ArchiveName.hpp"
#include "engine/storage/Providers.hpp"
#include "engine/storage/SyntheticFile.hpp"
#include "runtime/HookPoints.hpp"
#include "wxl/offsets/SFile.hpp"
#include "wxl/offsets/free/S.hpp"

#include <atomic>
#include <vector>

namespace wxl::storage
{
    namespace
    {
        namespace off = wxl::offsets;

        constexpr uint32_t kStatsEvery     = 2000; ///< opens between two stats lines
        constexpr uint32_t kMaxServedLines = 60;   ///< WXL_STORAGE_LOGS lines at most

        off::SFile::OpenExFn         g_origOpen  = nullptr;
        off::SFileGetFileSizeFn      g_origSize  = nullptr;
        off::SFileReadFileFn         g_origRead  = nullptr;
        off::SFile::SetFilePointerFn g_origSeek  = nullptr;
        off::SFileCloseFileFn        g_origClose = nullptr;

        std::atomic<uint32_t> g_opens{ 0 };  ///< opens offered to the providers
        std::atomic<uint32_t> g_served{ 0 }; ///< opens a provider or a transform answered

        /// WXL_STORAGE_LOGS: a line per served or reshaped file.
        bool LogServed()
        {
            static const bool enabled = config::Env("WXL_STORAGE_LOGS", false);
            const uint32_t    served  = g_served.fetch_add(1) + 1;
            return enabled && served < kMaxServedLines;
        }

        /// A provider's bytes for a search-path open (no archive given); specific-archive opens stay native.
        bool TryProvide(void* archive, const char* name, void** out)
        {
            NameBuffer copy;
            if (!CopyArchiveName(name, copy) || !ShouldIntercept(copy) || archive) return false;
            const uint32_t opens = g_opens.fetch_add(1) + 1;
            if (opens % kStatsEvery == 0) WLOG_INFO("Storage stats: opens=%u served=%u", opens, g_served.load());

            std::vector<uint8_t> bytes;
            if (!Provide(copy, bytes)) return false;
            SyntheticFile* file = MakeSyntheticFile(copy, bytes.data(), uint32_t(bytes.size()));
            if (!file) return false;
            if (out) *out = file;
            if (LogServed()) WLOG_INFO("Storage: '%s' supplied by client provider (%u B)", copy, file->size);
            return true;
        }

        /// Offers a native open's bytes to the matching transforms. On a claim the native handle is
        /// closed and `*out` answers the reshaped bytes; otherwise it is rewound for its caller.
        void TryTransform(const char* name, void* handle, void** out)
        {
            NameBuffer copy;
            if (!CopyArchiveName(name, copy) || !ShouldIntercept(copy) || !HasTransform(copy)) return;

            uint32_t       sizeHigh = 0;
            const uint32_t size     = g_origSize(handle, &sizeHigh);
            if (sizeHigh != 0 || size == 0) return;

            std::vector<uint8_t> raw(size), reshaped;
            uint32_t             got = 0;
            if (g_origRead(handle, raw.data(), size, &got, nullptr, 0) && got == size &&
                Transform(copy, raw.data(), size, reshaped))
            {
                if (SyntheticFile* file = MakeSyntheticFile(copy, reshaped.data(), uint32_t(reshaped.size())))
                {
                    g_origClose(handle);
                    if (out) *out = file;
                    if (LogServed())
                        WLOG_INFO("Storage: '%s' reshaped by client transform (%u -> %u B)", copy, size, file->size);
                    return;
                }
            }
            uint32_t distanceHigh = 0;
            g_origSeek(handle, 0, &distanceHigh, uint32_t(SeekFrom::Begin));
        }

        int __stdcall OpenDetour(void* archive, const char* name, uint32_t flags, void** out)
        {
            char redirected[kMaxRedirectName];
            if (name && Redirect(name, redirected)) name = redirected;

            if (TryProvide(archive, name, out)) return 1;
            const int result = g_origOpen(archive, name, flags, out);
            if (result != 0 && out && *out) TryTransform(name, *out, out);
            return result;
        }

        uint32_t __stdcall SizeDetour(void* handle, uint32_t* sizeHigh)
        {
            if (!IsSynthetic(handle)) return g_origSize(handle, sizeHigh);
            if (sizeHigh) *sizeHigh = 0;
            return static_cast<SyntheticFile*>(handle)->size;
        }

        int __stdcall ReadDetour(void* handle, void* destination, uint32_t length, uint32_t* read, void* overlapped,
                                 uint32_t flags)
        {
            if (!IsSynthetic(handle)) return g_origRead(handle, destination, length, read, overlapped, flags);
            return ReadSynthetic(*static_cast<SyntheticFile*>(handle), destination, length, read) ? 1 : 0;
        }

        uint32_t __stdcall SeekDetour(void* handle, int32_t distance, uint32_t* distanceHigh, uint32_t from)
        {
            if (!IsSynthetic(handle)) return g_origSeek(handle, distance, distanceHigh, from);
            if (distanceHigh) *distanceHigh = 0;
            return SeekSynthetic(*static_cast<SyntheticFile*>(handle), distance, SeekFrom(from));
        }

        int __stdcall CloseDetour(void* handle)
        {
            if (!IsSynthetic(handle)) return g_origClose(handle);
            FreeSynthetic(static_cast<SyntheticFile*>(handle));
            return 1;
        }
    }

    void Install()
    {
        runtime::hookpoints::Attach("SFile::OpenEx", &OpenDetour, &g_origOpen);
        runtime::hookpoints::Attach("SFileGetFileSize", &SizeDetour, &g_origSize);
        runtime::hookpoints::Attach("SFileReadFile", &ReadDetour, &g_origRead);
        runtime::hookpoints::Attach("SFile::SetFilePointer", &SeekDetour, &g_origSeek);
        runtime::hookpoints::Attach("SFileCloseFile", &CloseDetour, &g_origClose);
        WLOG_INFO("Storage: hooks installed");
    }
}
