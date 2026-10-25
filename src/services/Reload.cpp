// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Making the client read again what it already loaded: textures in place, models through their
// cache and the units that draw them.

#include "services/Reload.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "objects/Objects.hpp"
#include "runtime/Core.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr char kReloadTag[] = "storage";

    /// The most model files of one name dropped at once.
    constexpr uint32_t kEvictMax = 64;

    char Lower(char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); }

    /// What a name is for the reload: a texture, a model (or a model's skin), or neither.
    enum class Kind
    {
        Texture,
        Model,
        Other,
    };

    /// A model's skin profile, "Foo00.skin", names its model "Foo.m2": two digits before the
    /// extension, which the client appends to the model's name without its extension.
    bool SkinModel(std::string& leaf)
    {
        const size_t dot = leaf.rfind('.');
        if (dot == std::string::npos || dot < 2) return false;
        if (!std::isdigit(static_cast<unsigned char>(leaf[dot - 1])) ||
            !std::isdigit(static_cast<unsigned char>(leaf[dot - 2])))
            return false;
        leaf = leaf.substr(0, dot - 2) + ".m2";
        return true;
    }

    /// Whether @p model or something hanging on it, a few levels down, was made from one of
    /// the files in @p files.
    bool DrawsWith(void* model, const std::vector<void*>& files, int depth)
    {
        if (!model || depth > 3) return false;
        void* file = wxl::core::binding::m2_model_file(model);
        if (file && std::find(files.begin(), files.end(), file) != files.end()) return true;
        void*          children[16];
        const uint32_t count = wxl::core::binding::m2_children(model, children, 16);
        for (uint32_t i = 0; i < count && i < 16; ++i)
            if (DrawsWith(children[i], files, depth + 1)) return true;
        return false;
    }
}

namespace wxl::core::services::reload
{
    wxl_result Reload(const char* asked, wxl_asset_reload* out)
    {
        wxl::host::ModHost* host = wxl::core::Host();
        if (!asked || !*asked || (host && !host->OnMainThread())) return WXL_E_INVALID;

        std::string name(asked);
        for (char& c : name)
            if (c == '/') c = '\\';
        const size_t slash = name.rfind('\\');
        const size_t dot   = name.rfind('.');
        std::string  extension;
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            for (size_t i = dot + 1; i < name.size(); ++i) extension += Lower(name[i]);

        Kind kind = Kind::Other;
        if (extension == "blp") kind = Kind::Texture;
        if (extension == "m2" || extension == "mdx" || extension == "mdl" || extension == "skin") kind = Kind::Model;
        if (kind == Kind::Other) return WXL_E_UNSUPPORTED;

        wxl_asset_reload done{};
        done.struct_size = sizeof done;

        if (kind == Kind::Texture)
        {
            const std::string base = name.substr(0, dot);
            done.textures          = wxl::core::binding::texture_reload(base.c_str(), &done.busy);
        }
        else
        {
            // The cache keeps a model by its file name alone, lower case, with .m2.
            std::string leaf = slash == std::string::npos ? name : name.substr(slash + 1);
            for (char& c : leaf) c = Lower(c);
            if (extension == "skin")
            {
                if (!SkinModel(leaf)) return WXL_E_UNSUPPORTED;
            }
            else if (extension != "m2")
            {
                leaf = leaf.substr(0, leaf.rfind('.')) + ".m2";
            }

            std::vector<void*> files(kEvictMax);
            done.models = wxl::core::binding::m2_cache_evict(leaf.c_str(), files.data(), kEvictMax);
            files.resize(std::min(done.models, kEvictMax));

            if (!files.empty())
            {
                // Units and players in view drawn with a dropped file: made again, by name.
                std::vector<void*> units;
                wxl::core::objects::Service().Handles().ForEach([&](wxl_handle, void* object, uint32_t) {
                    const uint32_t type = wxl::core::binding::object_type_id(object);
                    if (type != WXL_OBJECT_UNIT && type != WXL_OBJECT_PLAYER) return;
                    if (DrawsWith(wxl::core::binding::m2_object_model(object), files, 0)) units.push_back(object);
                });
                for (void* unit : units)
                    if (wxl::core::binding::unit_update_display(unit)) ++done.objects;
            }
        }

        WLOG_INFO(kReloadTag, "%s read again: %u texture(s), %u model file(s), %u unit(s) made again, %u still loading",
                  asked, done.textures, done.models, done.objects, done.busy);
        if (out && out->struct_size >= sizeof(uint32_t))
        {
            const uint32_t size = out->struct_size;
            std::memcpy(out, &done, std::min<size_t>(size, sizeof done));
            out->struct_size = size;
        }
        if (done.textures || done.models) return WXL_OK;
        return done.busy ? WXL_E_PENDING : WXL_E_NOT_FOUND;
    }
}
