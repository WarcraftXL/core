// SPDX-License-Identifier: GPL-3.0-or-later
// The extensions as owners: whose code runs, each call into one under a fault guard, what a fault takes back.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace wxl::runtime::owners
{
    /// An extension, numbered from 1 in load order. 0 is the core.
    using Id = uint32_t;
    constexpr Id kCore = 0;

    /// Registers a loaded extension; `folder` (its folder under Extensions\) defaults to its name.
    /// Answers kCore when the table is full.
    Id Add(const char* name, const char* folder = nullptr);

    /// permissions.low_level (raw detours and patches); the core has it.
    void GrantLowLevel(Id owner);
    bool LowLevel(Id owner);

    /// "" for the core.
    const char* Folder(Id owner);

    /// "core" for 0, "?" for an id never added.
    const char* Name(Id owner);

    /// Whether the owner's code is still called. The core always is.
    bool Running(Id owner);

    /// Whose code is innermost on this thread's stack; kCore outside any extension.
    Id Current();

    /// Runs fn(context) as `owner`'s code, under a guard that catches any fault or exception leaving
    /// it: the owner is then disabled and false returned, and the caller goes on as if the call had
    /// returned. The core's own code runs unguarded. `what` names the call for the log ("WXL_Load").
    bool Call(Id owner, const char* what, void (*fn)(void* context), void* context);

    template <class F>
    bool Run(Id owner, const char* what, F&& f)
    {
        using Fn = std::remove_reference_t<F>;
        return Call(owner, what, [](void* p) { (*static_cast<Fn*>(p))(); },
                    const_cast<void*>(static_cast<const void*>(&f)));
    }

    /// Takes back what an owner holds of one kind; answers how many.
    using TakeBack = uint32_t (*)(Id owner);

    /// Answers how many of one kind an owner keeps through Disable.
    using Kept = uint32_t (*)(Id owner);

    /// Registers a kind of thing Disable takes back, named for the log ("subscriptions"); `kept`, if any,
    /// counts the ones of that kind it leaves in place (resident hooks).
    void OnDisable(const char* what, TakeBack takeBack, Kept kept = nullptr);

    /// Stops calling the owner's code and takes back everything registered with OnDisable. Once.
    void Disable(Id owner, const char* reason);
}
