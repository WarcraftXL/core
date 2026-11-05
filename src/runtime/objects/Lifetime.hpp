// SPDX-License-Identifier: GPL-3.0-or-later
// The lifetime events of the objects table: OnObjectAdded and OnObjectRemoved.

#pragma once

#include "runtime/objects/Objects.hpp"

namespace wxl::runtime::objects
{
    /// The object manager took `object`: OnObjectAdded about its handle when the handle is new.
    void Announce(Table& table, Source& source, void* object);

    /// The object manager lets `object` go: OnObjectRemoved while its handle resolves, then it expires.
    void Withdraw(Table& table, Source& source, void* object);
}
