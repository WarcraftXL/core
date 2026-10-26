// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The WorldScene seams: the points of the world renderer where mods draw (RenderPoint).

#include "wxl/seams/seams.h"

#include "runtime/Core.hpp"
#include "services/RenderService.hpp"

#include "wxl/host/mod_host.hpp"

namespace
{
    void Raise(wxl_render_point point)
    {
        namespace render = wxl::core::services::render;
        wxl_render_point_args args{};
        if (!render::PointBegin(point, args)) return;
        wxl::core::Host()->Raise(WXL_EV_RenderPoint, &args);
        render::PointEnd();
    }
}

namespace wxl::seam::WorldScene::RenderDetailDoodads
{
    void WXL_CALL After() { Raise(WXL_RENDER_POINT_AFTER_OPAQUE_WORLD); }
}

namespace wxl::seam::WorldScene::RenderLiquidPass
{
    void WXL_CALL After() { Raise(WXL_RENDER_POINT_AFTER_LIQUIDS); }
}
