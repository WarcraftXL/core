// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The x64 attachment: nothing to detour, the accessors the client handed over, and the services.

#include "runtime/Attachment.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "objects/Objects.hpp"
#include "runtime/Core.hpp"
#include "seams/Table.hpp"
#include "services/DataService.hpp"
#include "services/GxService.hpp"
#include "services/M2FileService.hpp"
#include "services/M2Service.hpp"
#include "services/ScriptService.hpp"
#include "services/StorageService.hpp"

#include "wxl/services/object.h"
#include "wxl/services/data.h"
#include "wxl/services/gx.h"
#include "wxl/services/m2.h"
#include "wxl/services/m2file.h"
#include "wxl/services/script.h"
#include "wxl/services/storage.h"

namespace
{
    /// What the fork's source makes of a seam of this shape, for the log.
    const char* Calls(wxl::core::seams::Shape shape)
    {
        switch (shape)
        {
            case wxl::core::seams::Shape::Before: return "Before is a call at the head of";
            case wxl::core::seams::Shape::After: return "After is a call at every return of";
            case wxl::core::seams::Shape::Gate: return "Gate is a test at the head of";
            case wxl::core::seams::Shape::Observe: break;
        }
        return "Before and After are calls";
    }
}

namespace wxl::core::attachment
{
    void Prepare() {}

    std::vector<wxl_event> Events()
    {
        // The seam bodies are the same code on both architectures, so the events they raise are
        // the same list: the one the seam table derives.
        return seams::RaisedEvents();
    }

    std::vector<wxl::host::PublishedService> Services()
    {
        return {
            { WXL_OBJECT_SERVICE, WXL_OBJECT_SERVICE_VERSION, objects::Api() },
            { WXL_SCRIPT_SERVICE, WXL_SCRIPT_SERVICE_VERSION, &services::ScriptApi() },
            { WXL_STORAGE_SERVICE, WXL_STORAGE_SERVICE_VERSION, &services::StorageApi() },
            { WXL_DATA_SERVICE, WXL_DATA_SERVICE_VERSION, &services::DataApi() },
            { WXL_GX_SERVICE, WXL_GX_SERVICE_VERSION, &services::GxApi() },
            { WXL_M2_SERVICE, WXL_M2_SERVICE_VERSION, &services::M2Api() },
            { WXL_M2FILE_SERVICE, WXL_M2FILE_SERVICE_VERSION, &services::M2FileApi() },
        };
    }

    // Nothing of this build is a detour, and wxl.hook is not published on it, so a mod holds
    // nothing of the attachment to give back.
    void Release(uint32_t) {}

    void ReleaseAll() {}

    void Install()
    {
        // Nothing to attach: the client's source calls the entry points itself. What is checked
        // is what it handed over, since a seam's body reads the client through it.
        for (const seams::Seam& seam : seams::Table())
            WLOG_INFO(kTag, "seam %s: %s %s makes", seam.name, Calls(seam.shape), seam.symbol);
        for (const char* name : binding::Unbound())
            WLOG_WARN(kTag, "the client handed no %s accessor; what reads it answers absent", name);
    }
}
