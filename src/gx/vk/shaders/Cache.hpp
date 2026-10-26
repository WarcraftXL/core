// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The translations of the client's records, kept: in memory for the device's life, and on disk in
// one append-only pack (<client>\WarcraftXL\cache\vk\shaders.pack, outside the client's Cache\,
// which the client wipes), so a record is translated once per machine. A record is known by the
// 128-bit xxHash3 of its bytes, seeded with the translator's version and its options: a new
// translator or another device's options never read an older module. A refusal is kept too, so a
// record the translator cannot write is not tried again.
//
// Prewarm: the engine loads every permutation of a shader at once, and binds each later; the
// permutations are handed here as they load, and a worker thread translates the ones the pack
// lacks, so the bind finds them on disk. Get() never waits for the worker: a record it has not
// reached yet is translated on the spot.
//
// Thread-safe. The pack is read where it is (an index of offsets), not held in memory.

#pragma once

#include "gx/vk/shaders/Translate.hpp"

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace wxl::vk::shaders
{
    struct CacheKey
    {
        uint64_t low = 0;
        uint64_t high = 0;
        bool operator==(const CacheKey& other) const { return low == other.low && high == other.high; }
    };

    class Cache
    {
    public:
        struct Stats
        {
            uint64_t translated = 0; ///< records translated by this process
            uint64_t from_disk = 0;  ///< records read back from the pack
            uint64_t refused = 0;    ///< records the translator refused (or the pack says it did)
            uint64_t prewarmed = 0;  ///< of the translated, those the worker did
            double   milliseconds = 0.0; ///< spent translating, on any thread
        };

        Cache() = default;
        Cache(const Cache&)            = delete;
        Cache& operator=(const Cache&) = delete;
        ~Cache() { Close(); }

        /// Translates with @p options; @p pack (may be empty: memory only) is opened or made, and
        /// a pack another format wrote is started again.
        void Open(const std::string& pack, const Options& options);
        /// Stops the worker (what it has not reached is dropped) and closes the pack.
        void Close();

        static CacheKey KeyOf(const void* code, size_t size, const Options& options);

        /**
         * @brief The translation of @p code: kept, read from the pack, or made now.
         * @return null with @p why when the translator refuses the record.
         */
        std::shared_ptr<const Translation> Get(const void* code, size_t size, std::string& why);

        /// Records the worker translates into the pack ahead of their first Get.
        void Prewarm(std::vector<std::vector<uint8_t>> records);

        Stats Counters() const;
        /// Records queued for the worker.
        size_t Pending() const;

    private:
        struct KeyHash
        {
            size_t operator()(const CacheKey& k) const { return static_cast<size_t>(k.low ^ (k.high * 0x9E3779B97F4A7C15ull)); }
        };
        struct Entry
        {
            uint64_t offset = 0; ///< in the pack, of the record's body
            uint32_t size = 0;
        };

        bool ReadPack(const CacheKey& key, Translation& out, std::string& why, bool& refused);
        void WritePack(const CacheKey& key, const Translation* translation, const std::string& why);
        void Index();
        void Work();

        Options                                             options_;
        std::string                                         path_;
        FILE*                                               writer_ = nullptr;
        FILE*                                               reader_ = nullptr;
        mutable std::mutex                                  mutex_;
        std::unordered_map<CacheKey, Entry, KeyHash>        index_;
        std::unordered_map<CacheKey, std::shared_ptr<const Translation>, KeyHash> memory_;
        std::unordered_map<CacheKey, std::string, KeyHash>  refusals_;
        Stats                                               stats_;

        std::thread                                         worker_;
        std::condition_variable                             wake_;
        std::deque<std::vector<uint8_t>>                    queue_;
        bool                                                stop_ = false;
    };
}
