// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// FileDataIDs named from listfiles (see FileNames.hpp).

#include "features/FileNames.hpp"

#include <cinttypes>
#include <cstdio>

namespace wxl::modern_adt
{
    void FileNames::MapOf(std::string_view tilePath)
    {
        const size_t slash = tilePath.find_last_of("\\/");
        if (slash != std::string_view::npos) map_.assign(tilePath.substr(0, slash));
    }

    const Listfile* FileNames::Load(const std::string& csv, bool inArchives)
    {
        const auto known = loaded_.find(csv);
        if (known != loaded_.end()) return known->second.get();
        std::unique_ptr<Listfile>& slot = loaded_[csv];
        std::vector<uint8_t>       bytes;
        if (!(inArchives ? archives_ : disk_) || !(inArchives ? archives_ : disk_)(csv, bytes) || bytes.empty())
        {
            said_ = csv + (inArchives ? " (in the client's archives)" : "") + ": not found";
            if (log_) log_(said_);
            return nullptr;
        }
        const uint64_t hash = Listfile::Hash(bytes.data(), bytes.size());
        char           cache[64];
        std::snprintf(cache, sizeof cache, "listfile-%016" PRIx64 ".idx", hash);
        const std::string    cachePath = folder_ + "\\cache\\" + cache;
        auto                 list      = std::make_unique<Listfile>();
        std::vector<uint8_t> index;
        if (!(disk_ && disk_(cachePath, index) && list->Use(std::move(index), hash)))
        {
            index = Listfile::BuildIndex(bytes.data(), bytes.size(), hash);
            if (!list->Use(index, hash))
            {
                said_ = csv + ": not a listfile";
                if (log_) log_(said_);
                return nullptr;
            }
            ++indexed_;
            const bool kept = write_ && write_(cachePath, index);
            said_ = csv + ": " + std::to_string(list->Count()) + " name(s) indexed" + (kept ? ", kept in " + cachePath : ", not kept (the cache folder is not writable)");
        }
        else
            said_ = csv + ": " + std::to_string(list->Count()) + " name(s), from " + cachePath;
        if (!list->Count()) said_ += " (no line of the form id;path)";
        if (log_) log_(said_);
        slot = std::move(list);
        return slot.get();
    }

    bool FileNames::Name(uint32_t id, std::string& out)
    {
        if (!map_.empty())
            if (const Listfile* map = Load(map_ + "\\listfile.csv", true); map && map->Name(id, out))
            {
                ++named_;
                return true;
            }
        if (const Listfile* mine = Load(folder_ + "\\listfile.csv", false); mine && mine->Name(id, out))
        {
            ++named_;
            return true;
        }
        ++unknown_;
        return false;
    }
}
