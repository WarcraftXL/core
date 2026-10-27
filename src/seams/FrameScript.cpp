// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The FrameScript seams: what the runtime does when the client's script context comes and goes, on both builds.

#include "wxl/seams/seams.h"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Core.hpp"
#include "runtime/Stages.hpp"
#include "seams/Chat.hpp"
#include "seams/Gameplay.hpp"
#include "services/CVarService.hpp"
#include "services/ScriptService.hpp"
#include "services/DrawService.hpp"
#include "services/TextureService.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    /// The context the mods were last told is live, so that ScriptVmClosing names the one
    /// ScriptVmReady did whatever CGGameUI's flags say by then. A context is made at Initialize
    /// and announced at its first CreateEvents; one the client destroys before that (the
    /// placeholders of InitializeClient and ShutdownGame) is never announced.
    bool     g_made    = false;
    bool     g_live    = false;
    uint32_t g_context = WXL_SCRIPT_CONTEXT_GLUE;

    /// Which interface the context being created serves: the game's while CGGameUI is logging
    /// in (InitializeFrameXML sets m_loggingIn before it creates the context) or initialised
    /// (ReloadUI recreates it with m_initialized still set), the glue's otherwise.
    uint32_t ContextNow()
    {
        return wxl::core::binding::ui_logging_in() || wxl::core::binding::ui_initialized()
                   ? WXL_SCRIPT_CONTEXT_GAME
                   : WXL_SCRIPT_CONTEXT_GLUE;
    }

    const char* ContextName(uint32_t context)
    {
        return context == WXL_SCRIPT_CONTEXT_GAME ? "game" : "glue";
    }

    /// Appends one entry of wxl_ui_event_args::values: the kind, the text, a NUL.
    void Entry(std::vector<char>& out, char kind, const char* text)
    {
        out.push_back(kind);
        if (text) out.insert(out.end(), text, text + std::strlen(text));
        out.push_back('\0');
    }

    /**
     * @brief Renders the arguments of a UI event the way the client reads them.
     *
     * Walks the format as vFrameScript_SignalEvent does: every `%` is looked at, the letter
     * after it decides what one va_arg is and how it is pushed, any other letter reads nothing.
     * The list is a copy, so the client's own walk starts where it did.
     * @return how many entries were written.
     */
    uint32_t Render(std::vector<char>& out, const char* format, void* list)
    {
        if (!format) return 0;
        va_list  args  = static_cast<va_list>(list);
        uint32_t count = 0;
        char     text[32];
        for (; *format; ++format)
        {
            if (*format != '%') continue;
            switch (format[1])
            {
            case 'b':
                Entry(out, 'b', va_arg(args, int32_t) ? "1" : "0");
                break;
            case 'd':
                std::snprintf(text, sizeof text, "%d", va_arg(args, int32_t));
                Entry(out, 'd', text);
                break;
            case 'f':
                std::snprintf(text, sizeof text, "%.17g", va_arg(args, double));
                Entry(out, 'f', text);
                break;
            case 's':
                if (const char* string = va_arg(args, const char*)) Entry(out, 's', string);
                else Entry(out, 'n', nullptr);
                break;
            case 'u':
                std::snprintf(text, sizeof text, "%u", va_arg(args, uint32_t));
                Entry(out, 'u', text);
                break;
            default: continue;
            }
            ++count;
        }
        return count;
    }
}

namespace wxl::seam::FrameScript::Initialize
{
    void WXL_CALL After(int32_t, int32_t)
    {
        // Which interface the context is for is CGGameUI's state now; whether it gets one is
        // known at its CreateEvents.
        g_context = ContextNow();
        g_made    = true;
    }
}

namespace wxl::seam::FrameScript::Destroy
{
    void WXL_CALL Before()
    {
        g_made = false;
        if (!g_live)
        {
            WLOG_DEBUG(wxl::core::kTag, "script context closing before any interface was loaded into it");
            return;
        }
        WLOG_DEBUG(wxl::core::kTag, "script context closing: %s", ContextName(g_context));

        if (wxl::host::ModHost* host = wxl::core::Host())
        {
            wxl_script_vm_args args{};
            args.context      = g_context;
            args.native_state = wxl::core::binding::script_state();
            // A handler may call into the state, through wxl.script or the state it is handed.
            auto raise = [&] { host->Raise(WXL_EV_ScriptVmClosing, &args); };
            wxl::core::services::script::Keep("the ScriptVmClosing handlers", raise, nullptr);
        }
        wxl::core::services::script::OnContextClosing();
        wxl::core::seams::gameplay::Forget();
        g_live = false;
    }
}

namespace wxl::seam::FrameScript::CreateEvents
{
    void WXL_CALL After(const char** names, uint32_t count)
    {
        // The service installs the table again with its own events behind the client's; that
        // call reaches this seam too, and is not a new list.
        if (wxl::core::services::script::Reissuing()) return;

        // The first table of a context: an interface is coming up in it, which is when the mods
        // are told. The client has registered its own functions, and loads its frames next.
        const bool announce = g_made && !g_live;
        void*      state    = announce ? wxl::core::binding::script_state() : nullptr;
        if (announce)
        {
            g_live = true;
            wxl::core::services::script::OnContextReady(g_context, state);
            // WxlTexture, for the interface the context is coming up for.
            wxl::core::services::texture::OnScriptReady();
            // WxlDraw: shapes in the world for the interface's add-ons, a frame at a time.
            wxl::core::services::draw::OnScriptReady();
            wxl::core::services::cvar::OnContextReady();
        }
        wxl::core::services::script::OnClientEventsInstalled(names, count);
        wxl::core::seams::gameplay::Install(names, count);
        if (!announce) return;
        WLOG_DEBUG(wxl::core::kTag, "script context up: %s", ContextName(g_context));

        // There is a script VM with an interface coming and an API can be registered on it,
        // which is the stage exactly. The client builds more than one in a session; only the
        // first is news.
        wxl::core::stages::Reach(WXL_STAGE_SCRIPT);

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;
        wxl_script_vm_args args{};
        args.context      = g_context;
        args.native_state = state;
        auto raise = [&] { host->Raise(WXL_EV_ScriptVmReady, &args); };
        wxl::core::services::script::Keep("the ScriptVmReady handlers", raise, nullptr);
    }
}

namespace wxl::seam::FrameScript::vSignalEvent
{
    void WXL_CALL Before(uint32_t index, const char* format, void* args)
    {
        // The windows an NPC opens are told to the interface by name; their events say which.
        bool            opened = false;
        wxl_window_args window{};
        if (wxl::core::seams::gameplay::Signal(index, opened, window))
            if (wxl::host::ModHost* host = wxl::core::Host())
            {
                if (opened) host->Raise(WXL_EV_WindowOpened, &window);
                else host->Raise(WXL_EV_WindowClosed, &window);
            }

        // CHAT_MSG_ADDON is where the client delivers an addon message of the server.
        wxl_chat_addon_args addon{};
        if (wxl::core::seams::chat::AddonDelivered(index, format, args, addon))
            if (wxl::host::ModHost* host = wxl::core::Host()) host->Raise(WXL_EV_ChatAddonMessage, &addon);

        // The client fires events all the time; the arguments are rendered only for a mod
        // that listens, and the name comes from the table the service was handed, so an
        // index outside it is an event nothing can be told about.
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || host->Events().LiveCount(WXL_EV_UiEvent) == 0) return;
        const char* name = wxl::core::services::script::EventName(index);
        if (!name) return;

        // One buffer per call: a handler may fire an event of its own, which reaches here
        // again before this raise is over.
        std::vector<char> values;
        const uint32_t    count = Render(values, format, args);

        wxl_ui_event_args event{};
        event.index       = index;
        event.name        = wxl_str{name, static_cast<uint32_t>(std::strlen(name))};
        event.format      = wxl_str{format, format ? static_cast<uint32_t>(std::strlen(format)) : 0u};
        event.values      = values.empty() ? nullptr : values.data();
        event.values_size = static_cast<uint32_t>(values.size());
        event.value_count = count;
        host->Raise(WXL_EV_UiEvent, &event);
    }
}

namespace wxl::seam::FrameScript::HandleError
{
    void WXL_CALL Before(void* state)
    {
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !state) return;

        // The error value is on top of the stack. A string or a number reads as text, which
        // is the conversion the client itself makes next; anything else is what the client
        // will report it as.
        const char*   message = "UNKNOWN ERROR";
        uint32_t      length  = static_cast<uint32_t>(std::strlen(message));
        const int32_t top     = wxl::core::binding::script_top(state);
        if (top > 0)
        {
            const uint32_t type = wxl::core::binding::script_value_type(state, top);
            if (type == WXL_VALUE_STRING || type == WXL_VALUE_NUMBER)
            {
                uint32_t    size = 0;
                const char* text = wxl::core::binding::script_to_string(state, top, &size);
                if (text)
                {
                    message = text;
                    length  = size;
                }
            }
        }

        wxl_script_error_args args{};
        args.context = g_live ? g_context : ContextNow();
        args.message = wxl_str{message, length};
        host->Raise(WXL_EV_ScriptError, &args);
    }
}
