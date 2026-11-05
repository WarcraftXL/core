// SPDX-License-Identifier: GPL-3.0-or-later
// The event bus: connections in priority order, maybe about one object, extension handlers under their guard.

#pragma once

#include "wxl/abi/Common.h"
#include "wxl/Events.hpp"
#include "wxl/abi/Plugin.h"

#include <cstdint>

namespace wxl::events
{
    /// Connects a handler, owned by the code running now (runtime::owners::Current()). A subject limits
    /// it to the events raised about that object. `release` runs once it has ended and no walk is on.
    /// One made during a walk is called from the next one.
    int Connect(Event e, WXL_Handle subject, int32_t priority, Handler handler, void* user, WXL_ReleaseFn release,
                WXL_Connection* out);

    /// Ends a connection; WXL_E_EXPIRED when it already had.
    int Disconnect(WXL_Connection connection);

    /// WXL_OK while live, WXL_E_EXPIRED once ended.
    int Connected(WXL_Connection connection);

    /// Connects a core handler for the process lifetime.
    inline void Subscribe(Event e, Handler handler, void* user)
    {
        WXL_Connection id{};
        Connect(e, WXL_Handle{}, WXL_PRIORITY_DEFAULT, handler, user, nullptr, &id);
    }

    /// Raises an event to the connections without a subject.
    void EmitRaw(Event e, const void* args);

    /// Raises an event about `subject`: its connections and those without a subject.
    void EmitFor(Event e, WXL_Handle subject, const void* args);

    template <Event E>
    inline void Emit(const Args<E>& args) { EmitRaw(E, &args); }

    /// Whether anything is connected to the event. A hot path may read it unsynchronized.
    bool Any(Event e);

    /// Ends every connection of an extension (runtime::owners::Id); answers how many. Its releases
    /// are not called: its code is no longer run.
    uint32_t DropOwner(uint32_t owner);

    /// Ends every connection about an object that expired; answers how many.
    uint32_t DropSubject(WXL_Handle subject);
}
