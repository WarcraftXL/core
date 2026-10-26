// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The WorldFrame seams: what the runtime does around the world pass, and before the game acts on
// a click in the world.

#include "wxl/seams/seams.h"

#include "binding/Binding.hpp"
#include "objects/Objects.hpp"
#include "runtime/Core.hpp"
#include "runtime/Frame.hpp"
#include "gx/Vulkan.hpp"
#include "services/HighlightService.hpp"
#include "services/RenderService.hpp"
#include "services/SelectionCircleService.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

namespace wxl::seam::WorldFrame::RenderWorld
{
    void WXL_CALL Before(void* param)
    {
        // The world's draws begin: the Vulkan device counts them as the world's.
        wxl::core::gx::WorldPass(true);
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !param) return;

        // The batches of highlighted objects are kept from here (wxl.highlight).
        wxl::core::services::highlight::WorldBegin();

        wxl_frame_args args{};
        if (!wxl::core::frame::Current(args)) return;
        host->Raise(WXL_EV_WorldRenderBegin, &args);
    }

    void WXL_CALL After(void* param)
    {
        wxl::core::gx::WorldPass(false);
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !param) return;

        // The world and the text in it are drawn, and nothing of the interface over it yet: the
        // highlights go onto it first, then the mods' own passes.
        wxl::core::services::highlight::WorldEnd();
        wxl_render_point_args point{};
        if (wxl::core::services::render::PointBegin(WXL_RENDER_POINT_AFTER_WORLD, point))
        {
            host->Raise(WXL_EV_RenderPoint, &point);
            wxl::core::services::render::PointEnd();
        }
        // The circles of the frame are drawn: their counts are the last frame's now.
        wxl::core::services::selectioncircle::WorldEnd();
        wxl_frame_args args{};
        if (!wxl::core::frame::Current(args)) return;
        host->Raise(WXL_EV_WorldRenderEnd, &args);
    }
}

namespace wxl::seam::WorldFrame::PerformDefaultAction
{
    bool WXL_CALL Gate(void** self, uint32_t* button)
    {
        namespace binding = wxl::core::binding;
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !self || !*self || !button || !host->Listening(WXL_EV_WorldClickRequest)) return false;

        wxl_world_click_request_args args{};
        // The client numbers its buttons 1 left, 2 middle, 4 right; the contract 0, 2, 1.
        args.button = *button == 1 ? WXL_MOUSE_LEFT : (*button == 4 ? WXL_MOUSE_RIGHT : WXL_MOUSE_MIDDLE);
        uint64_t guid = 0;
        args.hit         = binding::input_click_hit(*self, &guid, &args.point);
        args.object_guid = guid;
        args.object      = guid ? wxl::core::objects::FromGuid(guid) : WXL_HANDLE_NONE;
        args.targeting   = binding::input_spell_targeting();

        wxl_bool refused = WXL_FALSE;
        args.refused     = &refused;
        host->Raise(WXL_EV_WorldClickRequest, &args);
        return refused != WXL_FALSE;
    }
}
