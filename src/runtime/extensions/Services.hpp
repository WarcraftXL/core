// SPDX-License-Identifier: GPL-3.0-or-later
// The services the core and the extensions publish to each other by name and version.

#pragma once

#include <cstdint>

namespace wxl::runtime::extensions
{
    /// Publishes a service under the code running now (owners::Current()); the pointer must outlive
    /// the process. The core publishes its own before any extension loads.
    void PublishInterface(const char* name, uint32_t version, void* iface);

    /// The highest published version at or above `version` of a service still offered, or null.
    void* GetInterface(const char* name, uint32_t version);

    /// Stops handing out a disabled extension's services; holders keep their pointer. Answers how many.
    uint32_t WithdrawServices(uint32_t owner);
}
