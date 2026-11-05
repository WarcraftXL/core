// SPDX-License-Identifier: GPL-3.0-or-later
// The wxl.storage service: extensions register their redirects, providers and transforms.

#include "engine/storage/StorageHook.hpp"

#include "common/Log.hpp"
#include "engine/storage/Providers.hpp"
#include "runtime/extensions/Services.hpp"
#include "runtime/Owners.hpp"

#include "wxl/abi/Storage.h"

namespace wxl::storage
{
    namespace
    {
        void Refused(const char* what)
        {
            WLOG_ERROR("storage: '%s' registered one %s too many (%u at most)",
                       runtime::owners::Name(runtime::owners::Current()), what, kMaxRegistrations);
        }

        void __cdecl ApiRegisterClientProvider(WXL_ProvideFn fn)
        {
            if (fn && !AddProvider(fn)) Refused("provider");
        }

        void __cdecl ApiRegisterClientTransform(const char* suffix, WXL_TransformFn fn)
        {
            if (fn && suffix && *suffix && !AddTransform(suffix, fn)) Refused("transform");
        }

        void __cdecl ApiRegisterClientRedirect(WXL_RedirectFn fn)
        {
            if (fn && !AddRedirect(fn)) Refused("redirect");
        }

        const WXL_StorageApi g_api = {
            sizeof(WXL_StorageApi),     WXL_STORAGE_API_VERSION,     &ApiRegisterClientProvider,
            &ApiRegisterClientTransform, &ApiRegisterClientRedirect,
        };
    }

    void Publish()
    {
        runtime::extensions::PublishInterface("wxl.storage", WXL_STORAGE_API_VERSION, const_cast<WXL_StorageApi*>(&g_api));
    }
}
