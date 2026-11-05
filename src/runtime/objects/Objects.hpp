// SPDX-License-Identifier: GPL-3.0-or-later
// The objects service ("wxl.objects"): the client's resident objects as generational handles.

#pragma once

#include "wxl/abi/Objects.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wxl::runtime::objects
{
    /// Where objects come from: the client's object manager, or a test's.
    class Source
    {
    public:
        virtual ~Source() = default;
        /// Whether there is an object manager to ask.
        virtual bool InWorld() = 0;
        /// The resident object with that GUID, or null.
        virtual void* Find(uint64_t guid) = 0;
        /// The GUID and type mask in an object's header.
        virtual uint64_t GuidOf(void* object) = 0;
        virtual uint32_t TypeMaskOf(void* object) = 0;
    };

    /// One slot per object seen (GUID and pointer), live until Forget or until the GUID answers another
    /// pointer; then its generation moves on and every handle to it answers WXL_E_EXPIRED.
    class Table
    {
    public:
        explicit Table(Source& source) : m_source(source) {}

        /// Called with each handle that expires: its connections end (events::DropSubject).
        void OnExpire(void (*onExpire)(WXL_Handle handle)) { m_onExpire = onExpire; }

        int FromGuid(uint64_t guid, WXL_Handle* out);
        int FromPointer(void* object, WXL_Handle* out);
        int Resolve(WXL_Handle handle, uint32_t typeMask, void** out);
        int Guid(WXL_Handle handle, uint64_t* out);

        /// The handle of an object the object manager just took: its live one, or a new one (`fresh`).
        WXL_Handle Track(void* object, bool* fresh);
        /// The live handle of `object`, or an empty one; asks the client nothing.
        WXL_Handle Find(void* object) const;
        /// Expires a handle: its object left the object manager.
        void Forget(WXL_Handle handle);
        /// The live slots.
        uint32_t Live() const { return m_live; }

    private:
        struct Slot
        {
            uint64_t guid       = 0;
            void*    object     = nullptr;
            uint32_t generation = 0; ///< odd while live, even while free; 0 never handed out
        };

        bool IsLive(const Slot& slot) const { return (slot.generation & 1) != 0; }
        void Expire(uint32_t index);
        /// A new live slot for a resident object.
        WXL_Handle Insert(uint64_t guid, void* object);
        /// The live slot of `handle`, checked against the object manager; null with `status` set.
        Slot* Check(WXL_Handle handle, int& status);

        Source&                                  m_source;
        void (*m_onExpire)(WXL_Handle)            = nullptr;
        std::vector<Slot>                        m_slots;
        std::vector<uint32_t>                    m_free;
        std::unordered_map<uint64_t, uint32_t>   m_byGuid;
        uint32_t                                 m_live = 0;
    };

    /// The service over the client, published as "wxl.objects". Main thread.
    void Publish();

    /// The object manager took `object` (created, or back in view): OnObjectAdded if its handle is new.
    void Added(void* object);
    /// The object manager lets `object` go: OnObjectRemoved while it resolves, then its handle expires.
    void Removed(void* object);
    /// The handle of a resident object, empty when there is none.
    WXL_Handle HandleOf(uint64_t guid);
    /// The client object of a live handle in `typeMask`, or null.
    void* PointerOf(WXL_Handle handle, uint32_t typeMask);
}
