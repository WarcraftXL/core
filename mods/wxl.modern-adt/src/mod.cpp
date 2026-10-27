// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.modern-adt: the client reads ADTs of later expansions' data. A reading layer
// (adt/Reader: the top-level chunks, MHDR, MTXF, MTXP), a store of what each area and each texture
// layer carries (store/Store), and features that use it, each with its own section of
// modern-adt.ini:
//
//   texture_scale  retail's per-texture scale (MTXP, MTXF bits 4 to 7): a texture drawn 2^scale
//                  times larger than the client's eight repeats a chunk, through wxl.terrain's
//                  layer provider. Stock maps carry no scale and are drawn as the client draws them.
//   height_blend   retail's height blend (MTXP heightScale and heightOffset, each texture's _h.blp):
//                  the client's single-pass terrain pixel programs rewritten through wxl.gx's
//                  shader provider, each layer's _h texture and numbers through wxl.terrain's.
//
// Main thread only, as wxl.terrain and wxl.gx call their providers.

#include "Options.hpp"
#include "features/HeightBlend.hpp"
#include "features/TextureScale.hpp"
#include "store/Store.hpp"

#include "wxl/sdk.hpp"
#include "wxl/services/gx.h"
#include "wxl/services/storage.h"
#include "wxl/services/terrain.h"

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace
{
    constexpr const char* kTag = "modern-adt";

    wxl::Service<wxl_terrain_api> g_terrain(WXL_TERRAIN_SERVICE, WXL_TERRAIN_SERVICE_VERSION);
    wxl::Service<wxl_gx_api>      g_gx(WXL_GX_SERVICE, WXL_GX_SERVICE_VERSION);
    wxl::Service<wxl_storage_api> g_storage(WXL_STORAGE_SERVICE, WXL_STORAGE_SERVICE_VERSION);

    bool FileExists(const std::string& path)
    {
        return g_storage && g_storage->exists && g_storage->exists(wxl::Str(path)) == WXL_TRUE;
    }

    class ModernAdtMod final : public wxl::Mod
    {
    public:
        wxl_result OnLoad() override
        {
            options_ = ReadOptions();
            for (const std::string& line : options_.problems) wxl::Log(WXL_LOG_WARN, kTag, line.c_str());
            store_ = std::make_unique<wxl::modern_adt::Store>(options_.source);
            scale_ = std::make_unique<wxl::modern_adt::TextureScale>(*store_);
            blend_ = std::make_unique<wxl::modern_adt::HeightBlend>(*store_, &FileExists);
            if (!options_.texture_scale && !options_.height_blend)
            {
                wxl::Log(WXL_LOG_INFO, kTag, "texture_scale and height_blend off (enabled = 0): no feature runs");
                return WXL_OK;
            }
            if (!g_terrain || !wxl::detail::Carries(g_terrain.Get(), &wxl_terrain_api::set_layer_provider))
            {
                wxl::Log(WXL_LOG_WARN, kTag, "wxl.terrain has no layer provider (a runtime before abi 0.1.42): no feature runs");
                return WXL_OK;
            }
            // The height blend needs the look's extra texture (abi 0.1.43) and wxl.gx's shader provider.
            blending_ = options_.height_blend && g_gx && wxl::detail::Carries(g_gx.Get(), &wxl_gx_api::set_shader_provider);
            if (options_.height_blend && !blending_)
                wxl::Log(WXL_LOG_WARN, kTag, "wxl.gx has no shader provider (a runtime before abi 0.1.43): height_blend does nothing");
            wxl_terrain_layer_provider provider{};
            provider.struct_size = sizeof provider;
            provider.fn          = &ModernAdtMod::OnLayer;
            provider.user        = this;
            provider.flags       = blending_ ? WXL_TERRAIN_LAYER_PARAMS_EVERY_DRAW : 0u;
            if (g_terrain->set_layer_provider(&provider) != WXL_OK)
            {
                wxl::Log(WXL_LOG_ERROR, kTag, "wxl.terrain refused the layer provider");
                return WXL_E_FAILED;
            }
            if (blending_)
            {
                wxl_gx_shader_provider shaders{};
                shaders.struct_size = sizeof shaders;
                shaders.fn          = &ModernAdtMod::OnProgram;
                shaders.user        = this;
                if (g_gx->set_shader_provider(&shaders) != WXL_OK)
                {
                    wxl::Log(WXL_LOG_ERROR, kTag, "wxl.gx refused the shader provider: height_blend does nothing");
                    blending_ = false;
                }
            }
            char line[200];
            if (options_.texture_scale)
            {
                std::snprintf(line, sizeof line, "texture_scale on: retail's per-texture scale from %s, on the maps whose ADTs carry it",
                              options_.source == wxl::modern_adt::adt::Prefer::mtxp   ? "MTXP"
                              : options_.source == wxl::modern_adt::adt::Prefer::mtxf ? "MTXF"
                                                                                       : "MTXP, else MTXF");
                wxl::Log(WXL_LOG_INFO, kTag, line);
            }
            if (blending_)
                wxl::Log(WXL_LOG_INFO, kTag,
                         "height_blend on: MTXP's heights and the _h textures, in the single-pass ps_3_0 terrain programs (Direct3D 9 and Vulkan)");
            return WXL_OK;
        }

        void OnUnload() override
        {
            if (g_terrain && wxl::detail::Carries(g_terrain.Get(), &wxl_terrain_api::set_layer_provider)) g_terrain->set_layer_provider(nullptr);
            if (blending_) g_gx->set_shader_provider(nullptr);
            if (blend_ && blending_)
            {
                char line[320];
                std::snprintf(line, sizeof line, "height_blend: %llu program(s) rewritten, %llu refused%s%s, %llu layer(s) with their height",
                              static_cast<unsigned long long>(blend_->Programs()), static_cast<unsigned long long>(blend_->Refused()),
                              blend_->Refused() ? " (last: " : "", blend_->Refused() ? (blend_->LastRefusal() + ")").c_str() : "",
                              static_cast<unsigned long long>(blend_->Blended()));
                wxl::Log(WXL_LOG_INFO, kTag, line);
            }
            if (scale_)
            {
                char line[160];
                std::snprintf(line, sizeof line, "texture_scale: %llu layer(s) seen, %llu scaled, %zu area(s) read",
                              static_cast<unsigned long long>(scale_->Asked()), static_cast<unsigned long long>(scale_->Scaled()),
                              store_->Areas());
                wxl::Log(WXL_LOG_INFO, kTag, line);
            }
        }

    private:
        static void WXL_CALL OnLayer(void* user, const wxl_terrain_layer* layer, wxl_terrain_layer_look* look)
        {
            auto* self = static_cast<ModernAdtMod*>(user);
            if (!layer || !look || !self->scale_) return;
            // The store keeps the layer whatever the options; the scale is answered only when on.
            const float before = look->texture_scale;
            self->scale_->Answer(*layer, *look);
            if (!self->options_.texture_scale) look->texture_scale = before;
            if (self->blending_) self->blend_->Answer(*layer, *look);
        }

        static wxl_bool WXL_CALL OnProgram(void* user, const wxl_gx_shader_code* code, wxl_gx_shader_answer* answer)
        {
            auto* self = static_cast<ModernAdtMod*>(user);
            if (!code || !answer || !self->blend_ || !self->blend_->Program(*code, self->program_)) return WXL_FALSE;
            answer->code = self->program_.data();
            answer->size = static_cast<uint32_t>(self->program_.size() * sizeof(uint32_t));
            return WXL_TRUE;
        }

        static wxl::modern_adt::Options ReadOptions()
        {
            const wxl_host*         host    = wxl::Host();
            const wxl_runtime_info* runtime = host && host->runtime ? host->runtime() : nullptr;
            if (!runtime || runtime->struct_size < offsetof(wxl_runtime_info, mods_directory) + sizeof(wxl_str) ||
                !runtime->mods_directory.size)
                return {};
            const std::string path =
                std::string(runtime->mods_directory.data, runtime->mods_directory.size) + "\\wxl.modern-adt\\modern-adt.ini";
            std::ifstream     in(path, std::ios::binary);
            if (!in) return {};
            std::ostringstream text;
            text << in.rdbuf();
            return wxl::modern_adt::ParseOptions(text.str());
        }

        wxl::modern_adt::Options                       options_;
        std::unique_ptr<wxl::modern_adt::Store>        store_;
        std::unique_ptr<wxl::modern_adt::TextureScale> scale_;
        std::unique_ptr<wxl::modern_adt::HeightBlend>  blend_;
        bool                                           blending_ = false;
        std::vector<uint32_t>                          program_; ///< the last program answered, read by the runtime before the call returns
    };
}

WXL_MOD(ModernAdtMod, "wxl.modern-adt", "Modern ADT")
