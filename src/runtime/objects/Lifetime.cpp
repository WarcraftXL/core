// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/objects/Lifetime.hpp"

#include "engine/events/Event.hpp"

namespace wxl::runtime::objects
{
    namespace
    {
        events::ObjectLifeArgs LifeArgs(Source& source, WXL_Handle handle, void* object)
        {
            return events::ObjectLifeArgs{ handle, source.GuidOf(object), source.TypeMaskOf(object), object };
        }
    }

    void Announce(Table& table, Source& source, void* object)
    {
        bool fresh = false;
        const WXL_Handle handle = table.Track(object, &fresh);
        if (!fresh) return;
        const auto args = LifeArgs(source, handle, object);
        events::EmitFor(events::Event::OnObjectAdded, handle, &args);
    }

    void Withdraw(Table& table, Source& source, void* object)
    {
        const WXL_Handle handle = table.Find(object);
        if (handle.generation == 0) return;
        const auto args = LifeArgs(source, handle, object);
        events::EmitFor(events::Event::OnObjectRemoved, handle, &args);
        table.Forget(handle);
    }
}
