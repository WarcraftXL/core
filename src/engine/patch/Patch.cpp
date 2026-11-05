// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/patch/Patch.hpp"

#include "common/Log.hpp"
#include "common/Mem.hpp"
#include "runtime/Owners.hpp"

#include "wxl/abi/Plugin.h"

#include <windows.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace wxl::patch
{
    namespace
    {
        struct Record
        {
            std::string          name;
            uintptr_t            address;
            std::vector<uint8_t> original;
            uint32_t             owner;
        };

        std::mutex& Mutex()
        {
            static std::mutex m;
            return m;
        }

        /// Never freed: extensions may be disabled while static destructors run.
        std::vector<Record>& Records()
        {
            static auto* records = new std::vector<Record>;
            return *records;
        }

        /// True when [address, address + length) is committed memory this process may read.
        bool Readable(uintptr_t address, uint32_t length)
        {
            uintptr_t at = address;
            const uintptr_t end = address + length;
            while (at < end)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT
                    || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                    return false;
                at = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            }
            return true;
        }

        bool Overlaps(const Record& r, uintptr_t address, uint32_t length)
        {
            return address < r.address + r.original.size() && r.address < address + length;
        }
    }

    int Write(const char* name, uintptr_t address, const void* expected, const void* bytes, uint32_t length,
              uint32_t owner)
    {
        const char* label = name ? name : "patch";
        if (!address || !bytes || !length || address + length < address) return WXL_E_INVALID_ARGUMENT;
        if (!Readable(address, length))
        {
            WLOG_ERROR("patch: '%s' of %s at 0x%08X: not mapped", label, runtime::owners::Name(owner),
                       unsigned(address));
            return WXL_E_INVALID_ARGUMENT;
        }

        std::lock_guard<std::mutex> lock(Mutex());
        for (const Record& r : Records())
            if (r.owner != owner && Overlaps(r, address, length))
            {
                WLOG_ERROR("patch: '%s' of %s at 0x%08X refused: '%s' of %s already changed those bytes", label,
                           runtime::owners::Name(owner), unsigned(address), r.name.c_str(),
                           runtime::owners::Name(r.owner));
                return WXL_E_MISMATCH;
            }

        const auto* at = reinterpret_cast<const uint8_t*>(address);
        if (expected && std::memcmp(at, expected, length) != 0)
        {
            WLOG_WARN("patch: '%s' of %s at 0x%08X not written: the bytes there are not the ones expected", label,
                      runtime::owners::Name(owner), unsigned(address));
            return WXL_E_MISMATCH;
        }

        Record record{ label, address, std::vector<uint8_t>(at, at + length), owner };
        if (!mem::Patch(reinterpret_cast<void*>(address), bytes, length))
        {
            WLOG_ERROR("patch: '%s' at 0x%08X: the page could not be made writable", label, unsigned(address));
            return WXL_E_INVALID_ARGUMENT;
        }
        Records().push_back(std::move(record));
        WLOG_DEBUG("patch: '%s' of %s at 0x%08X (%u bytes)", label, runtime::owners::Name(owner), unsigned(address),
                   length);
        return WXL_OK;
    }

    uint32_t DropOwner(uint32_t owner)
    {
        std::lock_guard<std::mutex> lock(Mutex());
        std::vector<Record>& records = Records();
        uint32_t restored = 0;
        for (size_t i = records.size(); i-- > 0;)
        {
            if (records[i].owner != owner) continue;
            const Record& r = records[i];
            mem::Patch(reinterpret_cast<void*>(r.address), r.original.data(), r.original.size());
            records.erase(records.begin() + std::ptrdiff_t(i));
            ++restored;
        }
        return restored;
    }
}
