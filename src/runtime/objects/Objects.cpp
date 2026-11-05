// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/objects/Objects.hpp"

namespace wxl::runtime::objects
{
    namespace
    {
        /// A slot whose generation reaches this is never reused: its handles could not stay unique.
        constexpr uint32_t kLastGeneration = 0xFFFFFFFEu;
    }

    void Table::Expire(uint32_t index)
    {
        Slot& slot = m_slots[index];
        if (!IsLive(slot)) return;
        const WXL_Handle expired{ index, slot.generation };
        m_byGuid.erase(slot.guid);
        slot.object = nullptr;
        ++slot.generation;
        --m_live;
        if (slot.generation != kLastGeneration) m_free.push_back(index);
        if (m_onExpire) m_onExpire(expired);
    }

    Table::Slot* Table::Check(WXL_Handle handle, int& status)
    {
        if (handle.generation == 0) { status = WXL_E_NULL_HANDLE; return nullptr; }
        if (handle.index >= m_slots.size() || m_slots[handle.index].generation != handle.generation)
        {
            status = WXL_E_EXPIRED;
            return nullptr;
        }
        if (!m_source.InWorld())
        {
            status = WXL_E_NOT_IN_WORLD;
            return nullptr;
        }
        Slot& slot = m_slots[handle.index];
        if (m_source.Find(slot.guid) != slot.object)
        {
            Expire(handle.index);
            status = WXL_E_EXPIRED;
            return nullptr;
        }
        status = WXL_OK;
        return &slot;
    }

    int Table::FromGuid(uint64_t guid, WXL_Handle* out)
    {
        if (!out || !guid) return WXL_E_INVALID_ARGUMENT;
        *out = WXL_Handle{};
        if (!m_source.InWorld()) return WXL_E_NOT_IN_WORLD;
        void* object = m_source.Find(guid);

        if (auto it = m_byGuid.find(guid); it != m_byGuid.end())
        {
            if (object && m_slots[it->second].object == object)
            {
                *out = WXL_Handle{ it->second, m_slots[it->second].generation };
                return WXL_OK;
            }
            Expire(it->second); // another object under that GUID, or none
        }
        if (!object) return WXL_E_NOT_FOUND;
        *out = Insert(guid, object);
        return WXL_OK;
    }

    WXL_Handle Table::Insert(uint64_t guid, void* object)
    {
        uint32_t index;
        if (!m_free.empty())
        {
            index = m_free.back();
            m_free.pop_back();
        }
        else
        {
            index = uint32_t(m_slots.size());
            m_slots.push_back(Slot{});
        }
        Slot& slot = m_slots[index];
        slot.guid   = guid;
        slot.object = object;
        ++slot.generation; // even -> odd: live
        m_byGuid[guid] = index;
        ++m_live;
        return WXL_Handle{ index, slot.generation };
    }

    int Table::FromPointer(void* object, WXL_Handle* out)
    {
        if (!out || !object) return WXL_E_INVALID_ARGUMENT;
        *out = WXL_Handle{};
        if (!m_source.InWorld()) return WXL_E_NOT_IN_WORLD;
        const uint64_t guid = m_source.GuidOf(object);
        // Only a resident object: a stale or foreign pointer finds another object, or none.
        if (!guid || m_source.Find(guid) != object) return WXL_E_NOT_FOUND;
        return FromGuid(guid, out);
    }

    int Table::Resolve(WXL_Handle handle, uint32_t typeMask, void** out)
    {
        if (!out) return WXL_E_INVALID_ARGUMENT;
        *out = nullptr;
        int status;
        Slot* slot = Check(handle, status);
        if (!slot) return status;
        if (!(m_source.TypeMaskOf(slot->object) & typeMask)) return WXL_E_WRONG_KIND;
        *out = slot->object;
        return WXL_OK;
    }

    int Table::Guid(WXL_Handle handle, uint64_t* out)
    {
        if (!out) return WXL_E_INVALID_ARGUMENT;
        int status;
        Slot* slot = Check(handle, status);
        if (!slot) return status;
        *out = slot->guid;
        return WXL_OK;
    }

    WXL_Handle Table::Track(void* object, bool* fresh)
    {
        *fresh = false;
        const uint64_t guid = object ? m_source.GuidOf(object) : 0;
        if (!guid) return WXL_Handle{};
        if (auto it = m_byGuid.find(guid); it != m_byGuid.end())
        {
            if (m_slots[it->second].object == object) return WXL_Handle{ it->second, m_slots[it->second].generation };
            Expire(it->second); // another object had that GUID
        }
        *fresh = true;
        return Insert(guid, object);
    }

    WXL_Handle Table::Find(void* object) const
    {
        const uint64_t guid = object ? m_source.GuidOf(object) : 0;
        const auto it = guid ? m_byGuid.find(guid) : m_byGuid.end();
        if (it == m_byGuid.end() || m_slots[it->second].object != object) return WXL_Handle{};
        return WXL_Handle{ it->second, m_slots[it->second].generation };
    }

    void Table::Forget(WXL_Handle handle)
    {
        if (handle.index < m_slots.size() && m_slots[handle.index].generation == handle.generation) Expire(handle.index);
    }
}
