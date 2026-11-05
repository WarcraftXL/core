// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/extensions/Services.hpp"

#include "common/Log.hpp"
#include "runtime/Owners.hpp"

#include <string>
#include <vector>

namespace wxl::runtime::extensions
{
    namespace
    {
        /// Name, version and pointer are stored as given: two extensions may agree on a service the
        /// core knows nothing of.
        struct Service
        {
            std::string name;
            uint32_t    version;
            void*       iface;
            owners::Id  owner;
            bool        withdrawn;
        };

        std::vector<Service>& All()
        {
            static std::vector<Service> services;
            return services;
        }
    }

    void PublishInterface(const char* name, uint32_t version, void* iface)
    {
        if (!name || !iface) return;
        All().push_back(Service{ name, version, iface, owners::Current(), false });
        WLOG_DEBUG("extensions: interface '%s' v%u published", name, version);
    }

    void* GetInterface(const char* name, uint32_t version)
    {
        if (!name) return nullptr;
        const Service* best = nullptr;
        for (const Service& service : All())
        {
            if (service.withdrawn || service.version < version || service.name != name) continue;
            if (!best || service.version > best->version) best = &service;
        }
        return best ? best->iface : nullptr;
    }

    uint32_t WithdrawServices(uint32_t owner)
    {
        uint32_t withdrawn = 0;
        for (Service& service : All())
        {
            if (service.owner != owner || service.withdrawn) continue;
            service.withdrawn = true;
            ++withdrawn;
        }
        return withdrawn;
    }
}
