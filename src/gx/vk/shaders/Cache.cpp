// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The translations kept in memory and on disk.
//
// The pack: a header (uint32 "WXSP", uint32 format), then records, each uint32 "REC ", uint32 the
// body's size, the body: the 16-byte key, a byte 1 for a module or 0 for a refusal, three zeros,
// then for a module its ProgramInfo (bytes pixel, major, minor, fixed fog (bit 0) and palette (bit 1); uint32 inputs, const
// rows, samplers, cube) and its SPIR-V words, for a refusal the reason's text.

#include "gx/vk/shaders/Cache.hpp"

#include "gx/vk/Log.hpp"

#define XXH_INLINE_ALL
#define XXH_NO_PREFETCH // clang-cl for 32-bit lacks the intrinsic without its target flag
#include <xxhash.h>

#include <chrono>
#include <cstring>

namespace wxl::vk::shaders
{
    namespace
    {
        constexpr uint32_t kPackMagic   = 0x50535857u; // "WXSP"
        constexpr uint32_t kPackFormat  = 1;
        constexpr uint32_t kRecordMagic = 0x20434552u; // "REC "
        constexpr uint32_t kInfoBytes   = 20;

        void PutInfo(uint8_t* to, const ProgramInfo& info)
        {
            to[0] = info.pixel ? 1 : 0;
            to[1] = info.major;
            to[2] = info.minor;
            to[3] = static_cast<uint8_t>((info.fixed_fog ? 1 : 0) | (info.palette ? 2 : 0));
            std::memcpy(to + 4, &info.inputs, 4);
            std::memcpy(to + 8, &info.const_rows, 4);
            std::memcpy(to + 12, &info.samplers, 4);
            std::memcpy(to + 16, &info.cube, 4);
        }

        void GetInfo(const uint8_t* from, ProgramInfo& info)
        {
            info.pixel     = from[0] != 0;
            info.major     = from[1];
            info.minor     = from[2];
            info.fixed_fog = (from[3] & 1) != 0;
            info.palette   = (from[3] & 2) != 0;
            std::memcpy(&info.inputs, from + 4, 4);
            std::memcpy(&info.const_rows, from + 8, 4);
            std::memcpy(&info.samplers, from + 12, 4);
            std::memcpy(&info.cube, from + 16, 4);
        }
    }

    CacheKey Cache::KeyOf(const void* code, size_t size, const Options& options)
    {
        const uint64_t seed = static_cast<uint64_t>(kTranslatorVersion) << 32 | (options.clip_distance ? 1u : 0u);
        const XXH128_hash_t h = XXH3_128bits_withSeed(code, size, seed);
        return CacheKey{h.low64, h.high64};
    }

    void Cache::Open(const std::string& pack, const Options& options)
    {
        Close();
        std::lock_guard<std::mutex> lock(mutex_);
        options_ = options;
        path_    = pack;
        stop_    = false;
        if (!path_.empty()) Index();
        worker_ = std::thread([this] { Work(); });
    }

    void Cache::Index()
    {
        // The records of the pack, or a new pack when there is none or it is not this format.
        reader_ = std::fopen(path_.c_str(), "rb");
        bool fresh = true;
        if (reader_)
        {
            uint32_t header[2] = {};
            if (std::fread(header, 4, 2, reader_) == 2 && header[0] == kPackMagic && header[1] == kPackFormat)
            {
                fresh = false;
                uint64_t at = 8;
                for (;;)
                {
                    uint32_t record[2] = {};
                    if (std::fread(record, 4, 2, reader_) != 2) break;
                    CacheKey key;
                    if (record[0] != kRecordMagic || record[1] < 20 || std::fread(&key, 16, 1, reader_) != 1)
                    {
                        fresh = true; // a damaged pack starts again
                        break;
                    }
                    index_[key] = Entry{at + 8 + 16, record[1] - 16};
                    at += 8 + record[1];
                    if (std::fseek(reader_, static_cast<long>(at), SEEK_SET) != 0) break;
                }
            }
            if (fresh)
            {
                std::fclose(reader_);
                reader_ = nullptr;
                index_.clear();
            }
        }
        if (fresh)
        {
            writer_ = std::fopen(path_.c_str(), "wb");
            if (writer_)
            {
                const uint32_t header[2] = {kPackMagic, kPackFormat};
                std::fwrite(header, 4, 2, writer_);
                std::fflush(writer_);
            }
            reader_ = std::fopen(path_.c_str(), "rb");
        }
        else
            writer_ = std::fopen(path_.c_str(), "ab");
        if (!writer_) Log(LogLevel::Warn, "the shader cache %s cannot be written: translations are kept for this launch only", path_.c_str());
        else Log(LogLevel::Debug, "shader cache %s: %zu records", path_.c_str(), index_.size());
    }

    void Cache::Close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            queue_.clear();
        }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        if (writer_) std::fclose(writer_);
        if (reader_) std::fclose(reader_);
        writer_ = reader_ = nullptr;
        index_.clear();
        memory_.clear();
        refusals_.clear();
    }

    bool Cache::ReadPack(const CacheKey& key, Translation& out, std::string& why, bool& refused)
    {
        refused         = false;
        const auto found = index_.find(key);
        if (found == index_.end() || !reader_) return false;
        std::vector<uint8_t> body(found->second.size);
        if (std::fseek(reader_, static_cast<long>(found->second.offset), SEEK_SET) != 0 ||
            std::fread(body.data(), 1, body.size(), reader_) != body.size() || body.size() < 4)
            return false;
        if (body[0] == 0)
        {
            refused = true;
            why.assign(reinterpret_cast<const char*>(body.data() + 4), body.size() - 4);
            return true;
        }
        if (body.size() < 4 + kInfoBytes || (body.size() - 4 - kInfoBytes) % 4) return false;
        GetInfo(body.data() + 4, out.info);
        out.words.resize((body.size() - 4 - kInfoBytes) / 4);
        std::memcpy(out.words.data(), body.data() + 4 + kInfoBytes, out.words.size() * 4);
        return true;
    }

    void Cache::WritePack(const CacheKey& key, const Translation* translation, const std::string& why)
    {
        if (!writer_ || index_.count(key)) return;
        std::vector<uint8_t> body(16 + 4);
        std::memcpy(body.data(), &key, 16);
        body[16] = translation ? 1 : 0;
        if (translation)
        {
            body.resize(20 + kInfoBytes + translation->words.size() * 4);
            PutInfo(body.data() + 20, translation->info);
            std::memcpy(body.data() + 20 + kInfoBytes, translation->words.data(), translation->words.size() * 4);
        }
        else
            body.insert(body.end(), why.begin(), why.end());
        if (std::fseek(writer_, 0, SEEK_END) != 0) return;
        const long at = std::ftell(writer_);
        const uint32_t record[2] = {kRecordMagic, static_cast<uint32_t>(body.size())};
        if (std::fwrite(record, 4, 2, writer_) != 2 || std::fwrite(body.data(), 1, body.size(), writer_) != body.size()) return;
        std::fflush(writer_);
        index_[key] = Entry{static_cast<uint64_t>(at) + 8 + 16, static_cast<uint32_t>(body.size() - 16)};
    }

    std::shared_ptr<const Translation> Cache::Get(const void* code, size_t size, std::string& why)
    {
        const CacheKey key = KeyOf(code, size, options_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto kept = memory_.find(key);
            if (kept != memory_.end()) return kept->second;
            const auto no = refusals_.find(key);
            if (no != refusals_.end())
            {
                why = no->second;
                return nullptr;
            }
            auto made    = std::make_shared<Translation>();
            bool refused = false;
            if (ReadPack(key, *made, why, refused))
            {
                ++stats_.from_disk;
                if (refused)
                {
                    ++stats_.refused;
                    refusals_[key] = why;
                    return nullptr;
                }
                memory_[key] = made;
                return made;
            }
        }
        // Not kept: translated now, outside the lock.
        auto           made  = std::make_shared<Translation>();
        const auto     begin = std::chrono::steady_clock::now();
        const bool     ok    = Translate(code, size, options_, *made, why);
        const double   ms    = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.milliseconds += ms;
        if (!ok)
        {
            ++stats_.refused;
            refusals_[key] = why;
            WritePack(key, nullptr, why);
            return nullptr;
        }
        ++stats_.translated;
        WritePack(key, made.get(), why);
        memory_[key] = made;
        return made;
    }

    std::vector<CacheKey> Cache::Keys() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<CacheKey>       keys;
        keys.reserve(index_.size());
        for (const auto& entry : index_) keys.push_back(entry.first);
        return keys;
    }

    std::shared_ptr<const Translation> Cache::Find(const CacheKey& key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto                  kept = memory_.find(key);
        if (kept != memory_.end()) return kept->second;
        if (refusals_.count(key)) return nullptr;
        auto        made    = std::make_shared<Translation>();
        bool        refused = false;
        std::string why;
        if (!ReadPack(key, *made, why, refused) || refused) return nullptr;
        ++stats_.from_disk;
        memory_[key] = made;
        return made;
    }

    void Cache::Prewarm(std::vector<std::vector<uint8_t>> records)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            for (auto& r : records) queue_.push_back(std::move(r));
        }
        wake_.notify_one();
    }

    void Cache::Work()
    {
        for (;;)
        {
            std::vector<uint8_t> record;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_) return;
                record = std::move(queue_.front());
                queue_.pop_front();
                const CacheKey key = KeyOf(record.data(), record.size(), options_);
                // Known already: nothing to do.
                if (memory_.count(key) || refusals_.count(key) || index_.count(key)) continue;
            }
            Translation  made;
            std::string  why;
            const auto   begin = std::chrono::steady_clock::now();
            const bool   ok    = Translate(record.data(), record.size(), options_, made, why);
            const double ms    = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            const CacheKey key = KeyOf(record.data(), record.size(), options_);
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.milliseconds += ms;
            if (!ok)
            {
                ++stats_.refused;
                refusals_[key] = why;
                WritePack(key, nullptr, why);
                continue;
            }
            ++stats_.translated;
            ++stats_.prewarmed;
            if (writer_) WritePack(key, &made, why);
            else memory_[key] = std::make_shared<Translation>(std::move(made));
        }
    }

    Cache::Stats Cache::Counters() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    size_t Cache::Pending() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
}
