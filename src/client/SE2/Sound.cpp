// SPDX-License-Identifier: GPL-3.0-or-later
// The sound seam: OnSoundPlay before the client's PlaySound script function runs.

#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "runtime/HookPoints.hpp"
#include "wxl/offsets/free/Script.hpp"

namespace
{
    namespace ev  = wxl::events;
    namespace off = wxl::offsets;

    off::Script_PlaySoundFn g_origPlaySound = nullptr;

    /// The sound's id or name is on the script state's stack.
    int __cdecl PlaySoundDetour(void* scriptState)
    {
        const ev::SoundPlayArgs args{ scriptState };
        ev::Emit<ev::Event::OnSoundPlay>(args);

        WLOG_INFO_ONCE("sound: play hook active");
        return g_origPlaySound(scriptState);
    }

    bool InstallSound()
    {
        wxl::runtime::hookpoints::Attach("Script_PlaySound", &PlaySoundDetour, &g_origPlaySound);
        return true;
    }
}

WXL_REGISTER_FEATURE("sound", true, InstallSound)
