// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The MapRenderChunk seams: a terrain chunk's layers made (wxl.terrain's layer providers asked)
// and drawn (which of the eight paths draws the chunk), on both builds.

#include "wxl/seams/seams.h"

#include "services/TerrainLayers.hpp"

namespace layers = wxl::core::services::terrain_layers;

namespace wxl::seam::MapRenderChunk::CreateLayer
{
    void WXL_CALL After(void* self, void* area, const void* layer, int32_t) { layers::LayerCreated(self, area, layer); }
}

#define WXL_LAYER_PATH(name, path, param)                                                \
    namespace wxl::seam::MapRenderChunk::name                                            \
    {                                                                                    \
        void WXL_CALL Before(void* param) { layers::Enter(layers::Path::path, param); } \
        void WXL_CALL After(void* param) { layers::Leave(param); }                       \
    }

WXL_LAYER_PATH(RenderSinglePassVertexPixelShader, vertex_single, self)
WXL_LAYER_PATH(RenderMultiPassVertexPixelShader, vertex_multi, render_chunk)
WXL_LAYER_PATH(RenderMultiPassVertexPixelShaderBigAlpha, vertex_multi, render_chunk)
WXL_LAYER_PATH(RenderSinglePassPixelShader, fixed_single, self)
WXL_LAYER_PATH(RenderMultiPassPixelShader, fixed_multi, render_chunk)
WXL_LAYER_PATH(RenderMultiPassPixelShaderBigAlpha, fixed_multi, render_chunk)
WXL_LAYER_PATH(RenderMultiPassAdditive, fixed_multi, render_chunk)
WXL_LAYER_PATH(RenderMultiPassAlpha, fixed_multi, render_chunk)

#undef WXL_LAYER_PATH
