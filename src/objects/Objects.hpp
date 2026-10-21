// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// wxl.object on this runtime: the service instance, and the client binding behind it.

#pragma once

#include "binding/Binding.hpp"

#include "wxl/runtime/object_service.hpp"

#include <cstdint>

namespace wxl::core::objects
{
    /**
     * @brief The leaf reads of wxl.object, over wxl::core::binding.
     *
     * A FIELD row of object_reads.def is the binding fact of the same name, defined here; every
     * other read is written in Reads.cpp. The same code on both builds: what differs is how the
     * binding under it reads the client.
     */
    class ClientObjects final : public wxl::runtime::ObjectBinding
    {
    public:
        wxl_guid        Guid(void* object) const override;
        wxl_object_type Type(void* object) const override;
        bool            Name(void* object, const char*& out, uint32_t& size) const override;
        bool            UnitTarget(void* object, wxl_guid& out) const override;
        bool            UnitReaction(void* unit, void* other, uint32_t& out) const override;
        void*           LocalPlayer() const override;

#define WXL_OBJECT_FIELD(kinds, member, Method, type)     \
    bool Method(void* object, type& out) const override \
    {                                                   \
        out = binding::member(object);                  \
        return true;                                    \
    }
#define WXL_OBJECT_READ(kinds, member, Method, type) \
    bool Method(void* object, type& out) const override;
#define WXL_OBJECT_READ_AT(kinds, member, Method, type) \
    bool Method(void* object, uint32_t index, type& out) const override;
#include "wxl/runtime/object_reads.def"
#undef WXL_OBJECT_FIELD
#undef WXL_OBJECT_READ
#undef WXL_OBJECT_READ_AT
    };

    /// The one object service of this process, built on first use over ClientObjects.
    wxl::runtime::ObjectService& Service();

    /// The table published under WXL_OBJECT_SERVICE.
    const wxl_object_api* Api();

    // What the seams drive: the ObjectMgr seams track an object when the client builds it and
    // untrack it when the client lets it go, so a handle kept past that point reads as absent.
    wxl_handle Track(void* object);
    void       Untrack(void* object);
    void       Untrack(wxl_handle handle);
    bool       IsTracked(void* object);
    wxl_handle HandleOf(void* object);
    wxl_handle FromGuid(uint64_t guid);
}
