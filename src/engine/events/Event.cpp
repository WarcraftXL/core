// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/events/Event.hpp"

#include "runtime/Owners.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace wxl::events
{
    namespace
    {
        namespace owners = runtime::owners;

        constexpr size_t kEventCount     = static_cast<size_t>(Event::Count);
        constexpr size_t kMaxAnswerBytes = 16;

        struct Entry
        {
            Event         event;
            Handler       fn;
            void*         user;
            WXL_ReleaseFn release;
            owners::Id    owner;
            WXL_Handle    subject;
            int32_t       priority;
            uint32_t      slot;     ///< in the connection table
            uint64_t      sequence; ///< connection order, for equal priorities
            bool          ended;
        };

        /// Before in a walk: lower priority, then earlier connection.
        bool Before(const Entry* a, const Entry* b)
        {
            return a->priority < b->priority || (a->priority == b->priority && a->sequence < b->sequence);
        }

        /// An event's connections, each list in walk order. Those about one object are apart, so that
        /// an emit about no object never walks them.
        struct Bucket
        {
            std::vector<Entry*> open;
            std::vector<Entry*> about;
            bool                hasEnded = false;
        };

        struct Slot
        {
            uint32_t generation = 0; ///< odd while live
            Entry*   entry      = nullptr;
        };

        /// Never destroyed: an extension's static connection may end during process exit.
        struct Bus
        {
            Bucket                buckets[kEventCount];
            std::vector<Slot>     slots;
            std::vector<uint32_t> free;
            std::vector<Entry*>   pending; ///< connected during a walk: inserted after it
            std::vector<Entry*>   ended;   ///< ended: released and freed after the walks
            std::atomic<int>      walking{ 0 };
            uint64_t              sequence    = 0;
            uint32_t              withSubject = 0; ///< live connections about an object
        };

        Bus& TheBus()
        {
            static Bus* bus = new Bus();
            return *bus;
        }

        bool IsEmpty(WXL_Handle h) { return h.generation == 0; }
        bool Same(WXL_Handle a, WXL_Handle b) { return a.index == b.index && a.generation == b.generation; }

        void Insert(Bus& bus, Entry* entry)
        {
            Bucket&              bucket = bus.buckets[size_t(entry->event)];
            std::vector<Entry*>& list   = IsEmpty(entry->subject) ? bucket.open : bucket.about;
            list.insert(std::upper_bound(list.begin(), list.end(), entry, &Before), entry);
        }

        void Compact(std::vector<Entry*>& list)
        {
            list.erase(std::remove_if(list.begin(), list.end(), [](const Entry* x) { return x->ended; }), list.end());
        }

        /// What waited for the walks to end: ended connections leave their buckets, connections made
        /// during a walk join theirs, then the ended ones are released and freed.
        void Settle(Bus& bus)
        {
            if (bus.walking.load() != 0 || (bus.pending.empty() && bus.ended.empty())) return;

            std::vector<Entry*> ended, pending;
            ended.swap(bus.ended);
            pending.swap(bus.pending);
            for (Entry* entry : ended)
            {
                Bucket& bucket = bus.buckets[size_t(entry->event)];
                if (!bucket.hasEnded) continue;
                bucket.hasEnded = false;
                Compact(bucket.open);
                Compact(bucket.about);
            }
            for (Entry* entry : pending)
                if (!entry->ended) Insert(bus, entry);
            for (Entry* entry : ended)
            {
                Slot& slot = bus.slots[entry->slot];
                slot.entry = nullptr;
                ++slot.generation;
                bus.free.push_back(entry->slot);
                if (entry->release && owners::Running(entry->owner))
                {
                    void*         user    = entry->user;
                    WXL_ReleaseFn release = entry->release;
                    owners::Run(entry->owner, "a release", [&] { release(user); });
                }
                delete entry;
            }
        }

        void End(Bus& bus, Entry* entry, bool release)
        {
            if (entry->ended) return;
            entry->ended = true;
            if (!release) entry->release = nullptr;
            if (!IsEmpty(entry->subject)) --bus.withSubject;
            bus.buckets[size_t(entry->event)].hasEnded = true;
            bus.ended.push_back(entry);
        }

        /// One emit, as the walk hands it to each handler.
        struct Raised
        {
            const char* name;
            AnswerField answer;
            const void* args;
        };

        /// An extension's handler, under its owner's guard. An answer it half wrote before faulting is
        /// put back as it found it.
        void CallExtension(const Raised& raised, const Entry& entry)
        {
            if (!owners::Running(entry.owner)) return;
            uint8_t  saved[kMaxAnswerBytes];
            uint8_t* target = nullptr;
            if (raised.answer.size && raised.answer.size <= sizeof saved)
            {
                std::memcpy(&target, static_cast<const uint8_t*>(raised.args) + raised.answer.offset, sizeof target);
                if (target) std::memcpy(saved, target, raised.answer.size);
            }
            const Handler fn   = entry.fn;
            void*         user = entry.user;
            const void*   args = raised.args;
            if (!owners::Run(entry.owner, raised.name, [&] { fn(user, args); }) && target)
                std::memcpy(target, saved, raised.answer.size);
        }

        void Call(const Raised& raised, const Entry& entry)
        {
            if (entry.ended) return;
            if (entry.owner == owners::kCore) return entry.fn(entry.user, raised.args);
            CallExtension(raised, entry);
        }

        /// Walks the connections made before it, by index: the lists do not change during a walk. With
        /// a subject, its own connections are merged into the open ones in walk order.
        void Walk(Event e, WXL_Handle subject, const void* args)
        {
            if (e >= Event::Count) return;
            Bus&          bus    = TheBus();
            const Bucket& bucket = bus.buckets[size_t(e)];
            const Raised  raised{ Name(e), AnswerOf(e), args };
            ++bus.walking;
            const size_t openCount  = bucket.open.size();
            const size_t aboutCount = IsEmpty(subject) ? 0 : bucket.about.size();
            size_t       i = 0, j = 0;
            for (;;)
            {
                while (j < aboutCount && !Same(bucket.about[j]->subject, subject)) ++j;
                const bool openNext = i < openCount && (j == aboutCount || Before(bucket.open[i], bucket.about[j]));
                if (openNext) Call(raised, *bucket.open[i++]);
                else if (j < aboutCount) Call(raised, *bucket.about[j++]);
                else break;
            }
            --bus.walking;
            Settle(bus);
        }

        uint32_t NewSlot(Bus& bus)
        {
            if (bus.free.empty())
            {
                bus.slots.push_back(Slot{});
                return uint32_t(bus.slots.size() - 1);
            }
            const uint32_t index = bus.free.back();
            bus.free.pop_back();
            return index;
        }

        /// The live entry of a connection, or null with `status` set.
        Entry* Live(Bus& bus, WXL_Connection connection, int& status)
        {
            status = connection.generation == 0 ? WXL_E_NULL_HANDLE : WXL_E_EXPIRED;
            if (connection.generation == 0 || connection.index >= bus.slots.size()) return nullptr;
            const Slot& slot = bus.slots[connection.index];
            if (slot.generation != connection.generation || !slot.entry || slot.entry->ended) return nullptr;
            status = WXL_OK;
            return slot.entry;
        }
    }

    int Connect(Event e, WXL_Handle subject, int32_t priority, Handler handler, void* user, WXL_ReleaseFn release,
                WXL_Connection* out)
    {
        if (!out) return WXL_E_INVALID_ARGUMENT;
        *out = WXL_Connection{};
        if (e >= Event::Count || !handler) return WXL_E_INVALID_ARGUMENT;
        Bus& bus = TheBus();

        const uint32_t index = NewSlot(bus);
        auto*          entry = new Entry{ e,        handler, user, release, owners::Current(), subject,
                                          priority, index,   ++bus.sequence, false };
        Slot& slot = bus.slots[index];
        ++slot.generation; // even -> odd: live
        slot.entry = entry;
        if (!IsEmpty(subject)) ++bus.withSubject;

        if (bus.walking.load() != 0) bus.pending.push_back(entry);
        else Insert(bus, entry);
        *out = WXL_Connection{ index, slot.generation };
        return WXL_OK;
    }

    int Disconnect(WXL_Connection connection)
    {
        Bus&   bus = TheBus();
        int    status;
        Entry* entry = Live(bus, connection, status);
        if (!entry) return status;
        End(bus, entry, true);
        Settle(bus);
        return WXL_OK;
    }

    int Connected(WXL_Connection connection)
    {
        int status;
        Live(TheBus(), connection, status);
        return status;
    }

    void EmitRaw(Event e, const void* args) { Walk(e, WXL_Handle{}, args); }

    void EmitFor(Event e, WXL_Handle subject, const void* args) { Walk(e, subject, args); }

    bool Any(Event e)
    {
        if (e >= Event::Count) return false;
        const Bucket& bucket = TheBus().buckets[size_t(e)];
        const auto    live   = [](const Entry* entry) { return !entry->ended; };
        return std::any_of(bucket.open.begin(), bucket.open.end(), live) ||
               std::any_of(bucket.about.begin(), bucket.about.end(), live);
    }

    uint32_t DropOwner(uint32_t owner)
    {
        Bus&     bus     = TheBus();
        uint32_t dropped = 0;
        for (const Slot& slot : bus.slots)
        {
            if (!slot.entry || slot.entry->ended || slot.entry->owner != owner) continue;
            End(bus, slot.entry, false);
            ++dropped;
        }
        Settle(bus);
        return dropped;
    }

    uint32_t DropSubject(WXL_Handle subject)
    {
        Bus& bus = TheBus();
        if (IsEmpty(subject) || bus.withSubject == 0) return 0;
        uint32_t dropped = 0;
        for (const Slot& slot : bus.slots)
        {
            if (!slot.entry || slot.entry->ended || !Same(slot.entry->subject, subject)) continue;
            End(bus, slot.entry, true);
            ++dropped;
        }
        Settle(bus);
        return dropped;
    }
}
