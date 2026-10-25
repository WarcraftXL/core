// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The content hash of a file: MD5 by the system's own implementation (Windows CNG, BCrypt), over
// the bytes the layer that holds the file answers, kept by name and layer.

#include "services/Hashes.hpp"

#include "services/Assets.hpp"
#include "services/DataService.hpp"
#include "services/ModCall.hpp"
#include "services/StorageService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/mod_host.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr char kHashTag[] = "storage";

    namespace assets = wxl::core::services::assets;

    /// The most hashes kept: the cache starts again from empty past it.
    constexpr size_t kCacheMax = 16384;

    char Lower(char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); }

    std::string Key(const std::string& name)
    {
        std::string key(name);
        for (char& c : key) c = c == '/' ? '\\' : Lower(c);
        return key;
    }

    // --- MD5 -------------------------------------------------------------------------------------

    /// The system's MD5 provider, opened once for the process.
    BCRYPT_ALG_HANDLE Md5()
    {
        static BCRYPT_ALG_HANDLE algorithm = [] {
            BCRYPT_ALG_HANDLE handle = nullptr;
            if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&handle, BCRYPT_MD5_ALGORITHM, nullptr, 0))) handle = nullptr;
            if (!handle) WLOG_ERROR(kHashTag, "the system's MD5 (BCrypt) cannot be opened: no file can be hashed");
            return handle;
        }();
        return algorithm;
    }

    /// One MD5 computation, fed piece by piece.
    class Digest
    {
    public:
        Digest()
        {
            BCRYPT_ALG_HANDLE algorithm = Md5();
            if (!algorithm) return;
            DWORD size = 0, written = 0;
            if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size),
                                                  sizeof size, &written, 0)))
                return;
            object_.resize(size);
            if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash_, object_.data(), size, nullptr, 0, 0))) hash_ = nullptr;
        }

        ~Digest()
        {
            if (hash_) BCryptDestroyHash(hash_);
        }

        Digest(const Digest&)            = delete;
        Digest& operator=(const Digest&) = delete;

        bool Ready() const { return hash_ != nullptr; }

        bool Add(const void* data, uint32_t size)
        {
            return hash_ && BCRYPT_SUCCESS(BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), size, 0));
        }

        bool Finish(uint8_t (&out)[16])
        {
            return hash_ && BCRYPT_SUCCESS(BCryptFinishHash(hash_, out, sizeof out, 0));
        }

    private:
        BCRYPT_HASH_HANDLE   hash_ = nullptr;
        std::vector<uint8_t> object_;
    };

    // --- what is kept ----------------------------------------------------------------------------

    /// One file hashed: which file of which layer it was, and what it hashed to.
    struct Kept
    {
        uint32_t    layer = WXL_FILE_LAYER_NONE;
        std::string identity; ///< the archive, the disk path and its write time, or the source
        uint64_t    size = 0;
        uint8_t     md5[16] = {};
        std::string source;
    };

    std::mutex                            g_cacheLock;
    std::unordered_map<std::string, Kept> g_cache; ///< by Key(name) and the layer's identity

    /// What tells one file of a layer from another: the archive's address and path, the file's
    /// path on the disk and when it was written, the source's id. Empty when it cannot be told,
    /// and such a file is not kept.
    std::string IdentityOf(const assets::Resolution& found, std::string& source)
    {
        switch (found.layer)
        {
        case WXL_FILE_LAYER_ARCHIVE:
        {
            wxl_core_archive described{};
            if (found.archive && wxl::core::binding::archive_record_of(found.archive, &described) && described.path)
                source = described.path;
            if (!found.archive) return std::string();
            char address[24];
            std::snprintf(address, sizeof address, "%p|", found.archive);
            return address + source;
        }
        case WXL_FILE_LAYER_DISK:
        case WXL_FILE_LAYER_FOLDER:
        {
            std::string path = found.path;
            const char* base = wxl::core::binding::storage_base_path();
            if (path.size() > 2 && path[0] == '\\' && path[1] == '\\' && base && *base)
                path = std::string(base) + path.substr(1);
            source = path;
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!::GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return std::string();
            char written[48];
            std::snprintf(written, sizeof written, "|%08lX%08lX", data.ftLastWriteTime.dwHighDateTime,
                          data.ftLastWriteTime.dwLowDateTime);
            return path + written;
        }
        case WXL_FILE_LAYER_PROVIDER:
        case WXL_FILE_LAYER_SERVED:
        {
            assets::SourceLayer(found.source, &source);
            return "source " + std::to_string(found.source);
        }
        default: return std::string();
        }
    }

    /// The name an open of @p asked opens, as the client's open resolves it: a database a mod
    /// serves, an override, a redirect, a scope.
    std::string Resolve(const char* asked)
    {
        char        buffer[512];
        std::string name = asked;
        if (wxl::core::services::data::ServedPath(name.c_str(), name.size(), buffer, sizeof buffer)) return buffer;
        if (wxl::core::services::storage::Overridden(name.c_str(), buffer, sizeof buffer)) name = buffer;
        if (wxl::core::services::storage::Redirected(name.c_str(), buffer, sizeof buffer)) name = buffer;
        if (assets::Unscoped(name.c_str(), buffer, sizeof buffer)) name = buffer;
        return name;
    }

    // --- the requests of mods --------------------------------------------------------------------

    struct Job
    {
        uint64_t         id = 0;
        uint32_t         mod = 0;
        std::string      name;
        uint32_t         flags = 0;
        wxl_file_hash_fn fn    = nullptr;
        void*            user  = nullptr;

        wxl_result    result = WXL_E_FAILED;
        wxl_file_hash hash{};
        std::string   source; ///< what hash.source points into
    };

    /// Shared with the thread, which may outlive everything else at the way out: never freed.
    struct Queue
    {
        std::mutex              lock;
        std::condition_variable wake;
        std::condition_variable idle;
        std::deque<Job>         waiting;
        std::deque<Job>         done;
        uint64_t                running = 0; ///< the job the thread is hashing, 0 for none
        uint32_t                runningMod = 0; ///< whose it is
        std::vector<uint64_t>   dropped;     ///< running jobs whose mod went
        bool                    started = false;
        bool                    stopping = false;
        bool                    stopped  = false;
    };

    Queue&                g_queue = *new Queue();
    std::atomic<uint64_t> g_nextJob{1};

    void Work()
    {
        Queue& queue = g_queue;
        std::unique_lock<std::mutex> lock(queue.lock);
        while (true)
        {
            queue.wake.wait(lock, [&] { return queue.stopping || !queue.waiting.empty(); });
            if (queue.stopping) break;
            Job job = std::move(queue.waiting.front());
            queue.waiting.pop_front();
            queue.running    = job.id;
            queue.runningMod = job.mod;
            lock.unlock();

            job.hash.struct_size = sizeof job.hash;
            job.result           = wxl::core::services::hashes::Hash(job.name.c_str(), job.flags, &job.hash);
            if (job.result == WXL_OK && job.hash.source.data)
                job.source.assign(job.hash.source.data, job.hash.source.size);

            lock.lock();
            queue.running    = 0;
            queue.runningMod = 0;
            const auto dropped = std::find(queue.dropped.begin(), queue.dropped.end(), job.id);
            if (dropped != queue.dropped.end()) queue.dropped.erase(dropped);
            else if (!queue.stopping) queue.done.push_back(std::move(job));
            queue.idle.notify_all();
        }
        queue.stopped = true;
        queue.idle.notify_all();
    }
}

namespace wxl::core::services::hashes
{
    wxl_result Hash(const char* asked, uint32_t flags, wxl_file_hash* out)
    {
        if (!asked || !*asked || !out || out->struct_size < offsetof(wxl_file_hash, source)) return WXL_E_INVALID;
        thread_local std::string source;

        const std::string name = Resolve(asked);
        void*             file = nullptr;
        assets::Resolution found;
        {
            // The layers answer, the transforms and the events do not; nothing raised for it.
            const assets::AskingScope asking;
            const assets::ModeScope   inner(assets::Mode::Inner);
            assets::ClearPending();
            assets::ClearResolution();
            file  = wxl::core::binding::file_open(name.c_str());
            found = assets::LastResolution();
        }
        if (!file) return assets::PendingSeen() ? WXL_E_PENDING : WXL_E_NOT_FOUND;

        const uint64_t size = wxl::core::binding::file_size(file);
        std::string    from;
        std::string    identity = IdentityOf(found, from);
        const std::string key   = Key(name) + "\n" + std::to_string(found.layer) + "\n" + identity;

        Kept kept;
        bool hit = false;
        if (!identity.empty() && !(flags & WXL_HASH_FRESH))
        {
            std::lock_guard<std::mutex> lock(g_cacheLock);
            const auto it = g_cache.find(key);
            if (it != g_cache.end() && it->second.size == size)
            {
                kept = it->second;
                hit  = true;
            }
        }

        if (!hit)
        {
            Digest digest;
            if (!digest.Ready())
            {
                wxl::core::binding::file_close(file);
                return WXL_E_UNSUPPORTED;
            }
            std::vector<uint8_t> buffer(static_cast<size_t>(std::min<uint64_t>(size ? size : 1, 1u << 20)));
            uint64_t             read = 0;
            {
                const assets::ModeScope inner(assets::Mode::Inner);
                while (read < size)
                {
                    const uint64_t left = size - read;
                    const uint32_t want = static_cast<uint32_t>(std::min<uint64_t>(left, buffer.size()));
                    const uint32_t got  = wxl::core::binding::file_read(file, buffer.data(), want);
                    if (!got || !digest.Add(buffer.data(), got)) break;
                    read += got;
                }
            }
            if (read != size || !digest.Finish(kept.md5))
            {
                wxl::core::binding::file_close(file);
                return WXL_E_FAILED;
            }
            kept.layer    = found.layer;
            kept.identity = identity;
            kept.size     = size;
            kept.source   = from;
            if (!identity.empty())
            {
                std::lock_guard<std::mutex> lock(g_cacheLock);
                if (g_cache.size() >= kCacheMax) g_cache.clear();
                g_cache[key] = kept;
            }
        }
        wxl::core::binding::file_close(file);

        source = kept.source;
        // Written no further than the caller's struct goes.
        wxl_file_hash answer{};
        answer.struct_size = out->struct_size;
        answer.layer       = kept.layer;
        answer.size        = kept.size;
        std::memcpy(answer.md5, kept.md5, sizeof answer.md5);
        answer.source = wxl_str{source.c_str(), static_cast<uint32_t>(source.size())};
        std::memcpy(out, &answer, std::min<size_t>(out->struct_size, sizeof answer));
        return WXL_OK;
    }

    wxl_result HashAsync(const char* name, uint32_t flags, wxl_file_hash_fn fn, void* user, uint64_t* out, uint32_t mod)
    {
        if (!name || !*name || !fn || !out) return WXL_E_INVALID;
        Queue& queue = g_queue;
        Job    job;
        const uint64_t id = g_nextJob.fetch_add(1, std::memory_order_relaxed);
        job.id    = id;
        job.mod   = mod;
        job.name  = name;
        job.flags = flags;
        job.fn    = fn;
        job.user  = user;
        {
            std::lock_guard<std::mutex> lock(queue.lock);
            if (queue.stopping) return WXL_E_FAILED;
            if (!queue.started)
            {
                queue.started = true;
                std::thread(&Work).detach();
            }
            queue.waiting.push_back(std::move(job));
        }
        queue.wake.notify_one();
        *out = id;
        return WXL_OK;
    }

    void Pump()
    {
        Queue&          queue = g_queue;
        std::deque<Job> ready;
        {
            std::lock_guard<std::mutex> lock(queue.lock);
            if (queue.done.empty()) return;
            ready.swap(queue.done);
        }
        for (Job& job : ready)
        {
            // A job's source is its own string, so the answer's points there.
            if (job.result == WXL_OK)
                job.hash.source = wxl_str{job.source.c_str(), static_cast<uint32_t>(job.source.size())};
            const wxl_file_hash* hash = job.result == WXL_OK ? &job.hash : nullptr;
            CallMod(job.mod, kHashTag, "its hash_file_async answer",
                    [&] { job.fn(job.user, job.id, job.result, hash); });
        }
    }

    void ForgetMod(uint32_t mod)
    {
        Queue&                      queue = g_queue;
        std::lock_guard<std::mutex> lock(queue.lock);
        const auto mine = [mod](const Job& job) { return job.mod == mod; };
        queue.waiting.erase(std::remove_if(queue.waiting.begin(), queue.waiting.end(), mine), queue.waiting.end());
        queue.done.erase(std::remove_if(queue.done.begin(), queue.done.end(), mine), queue.done.end());
        // The job the thread is on: its answer is dropped when it is done.
        if (queue.running && queue.runningMod == mod) queue.dropped.push_back(queue.running);
    }

    void ForgetCache()
    {
        std::lock_guard<std::mutex> lock(g_cacheLock);
        g_cache.clear();
    }

    void Stop()
    {
        Queue&                       queue = g_queue;
        std::unique_lock<std::mutex> lock(queue.lock);
        queue.waiting.clear();
        queue.done.clear();
        if (!queue.started || queue.stopping) return;
        queue.stopping = true;
        queue.wake.notify_all();
        // The hash in progress finishes; a file read takes what it takes, bounded here.
        if (!queue.idle.wait_for(lock, std::chrono::seconds(5), [&] { return queue.stopped; })) return;
        // Stopped: a later request starts a thread again (the harness asks after a shutdown).
        queue.started  = false;
        queue.stopping = false;
        queue.stopped  = false;
    }
}
