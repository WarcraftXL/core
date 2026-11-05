// SPDX-License-Identifier: GPL-3.0-or-later
// The texture seams: OnTextureUpload before a texture reaches the device, OnBlpLoad after a by-name create.

#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "runtime/HookPoints.hpp"
#include "wxl/offsets/free/G.hpp"
#include "wxl/offsets/free/T.hpp"

#include <cstdint>

namespace
{
    namespace ev  = wxl::events;
    namespace off = wxl::offsets;

    off::TextureCreate_4B9760Fn g_origTextureCreate = nullptr;
    off::GxTexUpdate_681F20Fn   g_origTextureUpdate = nullptr;

    /// The rectangle is the whole surface or a part of it.
    void __cdecl TextureUpdateDetour(void* texture, int left, int top, int right, int bottom, int flag)
    {
        const ev::TextureUploadArgs args{ texture, static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top) };
        ev::Emit<ev::Event::OnTextureUpload>(args);
        g_origTextureUpdate(texture, left, top, right, bottom, flag);
    }

    /// Raised on every request, a cached texture included, with the name asked for.
    void* __cdecl TextureCreateDetour(const char* name, uint32_t flags, void* status, uint32_t flags2)
    {
        void*                 texture = g_origTextureCreate(name, flags, status, flags2);
        const ev::BlpLoadArgs args{ name, texture };
        ev::Emit<ev::Event::OnBlpLoad>(args);
        return texture;
    }

    bool InstallTextures()
    {
        wxl::runtime::hookpoints::Attach("GxTexUpdate_681F20", &TextureUpdateDetour, &g_origTextureUpdate);
        wxl::runtime::hookpoints::Attach("TextureCreate_4B9760", &TextureCreateDetour, &g_origTextureCreate);
        return true;
    }
}

WXL_REGISTER_FEATURE("textures", true, InstallTextures)
