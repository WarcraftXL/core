// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.script service: the SDK's script table over the client's own Lua, through the binding.

#include "services/ScriptService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Code.hpp"
#include "runtime/Core.hpp"

#include "wxl/events.h"
#include "wxl/host/guard.hpp"
#include "wxl/host/mod_host.hpp"

#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <set>
#include <string>
#include <vector>

/// One invocation of a registered function: the state it runs in, how many arguments it got,
/// how many results it pushed, and the failure it asked for. Lives on the thunk's stack for the
/// call; the accessors answer nothing once `open` is cleared.
struct wxl_script_call
{
    void*   state;
    int32_t arg_count;
    int32_t pushed;
    bool    open;
    bool    failed;
    char    message[512];
};

namespace
{
    constexpr char kTag[] = "script";

    namespace code    = wxl::core::code;
    namespace binding = wxl::core::binding;

    /// A function a mod registered on the live context. Its slot (index + 1) is the closure's
    /// upvalue, which is how a call finds it again; a withdrawn one keeps its slot, cleared.
    struct Registration
    {
        std::string   name;
        wxl_script_fn fn;
        void*         user;
        uint32_t      owner; ///< the host's token for the mod, 0 for nobody
        bool          live;
    };

    bool     g_live    = false;
    uint32_t g_context = WXL_SCRIPT_CONTEXT_GLUE;
    void*    g_state   = nullptr;

    std::vector<Registration> g_functions;

    /// The events mods declared on the live context. The client's event hash keeps the name by
    /// pointer, so each string stays where it is until the context closes.
    std::deque<std::string> g_events;

    /// The client's own event table, as it last installed it: what ours are appended to.
    const char** g_clientNames = nullptr;
    uint32_t     g_clientCount = 0;
    bool         g_reissuing   = false;

    wxl::host::ModHost::ModToken CurrentMod()
    {
        wxl::host::ModHost* host = wxl::core::Host();
        return host ? host->CurrentMod() : 0;
    }

    const char* ModName(uint32_t owner)
    {
        wxl::host::ModHost* host = wxl::core::Host();
        const wxl::host::LoadedMod* mod = host ? host->ModOf(owner) : nullptr;
        return mod ? mod->id.c_str() : "(no mod)";
    }

    /// The paths of the runtime that left the state other than they found it, each told once.
    std::set<std::string> g_told;

    bool FirstTime(const char* path, const char* what)
    {
        return g_told.insert(std::string(path) + '\n' + what).second;
    }

    /// A call into the client's Lua that a hardware fault stopped: the state is back where it
    /// was, the call is abandoned as a Lua error would abandon it.
    void ReportFault(const char* what, const wxl::host::Fault& fault)
    {
        char where[96];
        wxl::host::DescribeFault(fault, where, sizeof where);
        WLOG_ERROR(kTag, "%s: the client faulted inside it: %s; the call is abandoned and the "
                         "script state is put back as it was before it",
                   what, where);
    }

    /// The Lua index of a 0-based argument, or 0 when the call has no such argument.
    int32_t ArgIndex(const wxl_script_call* call, uint32_t index)
    {
        if (!call || !call->open) return 0;
        if (index >= static_cast<uint32_t>(call->arg_count)) return 0;
        return static_cast<int32_t>(index) + 1;
    }

    /// Installs the client's event table again, ours appended, so each of ours has an index.
    void Reissue()
    {
        std::vector<const char*> names;
        names.reserve(g_clientCount + g_events.size());
        for (uint32_t i = 0; i < g_clientCount; ++i) names.push_back(g_clientNames[i]);
        for (const std::string& event : g_events) names.push_back(event.c_str());

        g_reissuing = true;
        auto install = [&] { binding::script_create_events(names.data(), static_cast<uint32_t>(names.size())); };
        wxl::host::Fault fault;
        if (!wxl::core::services::script::Keep("wxl.script's event table", install, &fault))
            ReportFault("installing the event table", fault);
        g_reissuing = false;
    }

    /// The index of a declared event in the installed table, or -1.
    int32_t EventIndex(const char* name)
    {
        for (size_t i = 0; i < g_events.size(); ++i)
            if (g_events[i] == name) return static_cast<int32_t>(g_clientCount + i);
        return -1;
    }

    void Push(void* state, const wxl_value& value)
    {
        switch (value.type)
        {
        case WXL_VALUE_NUMBER: binding::script_push_number(state, value.as.number); break;
        case WXL_VALUE_STRING:
            binding::script_push_string(state, value.as.string.data ? value.as.string.data : "",
                                        value.as.string.data ? value.as.string.size : 0u);
            break;
        case WXL_VALUE_BOOL: binding::script_push_bool(state, value.as.boolean); break;
        default: binding::script_push_nil(state); break;
        }
    }

    /// Forgets a registration and takes its global away from the live context.
    void Withdraw(size_t slot)
    {
        Registration& registration = g_functions[slot - 1];
        if (!registration.live) return;
        registration.live = false;
        if (!g_live) return;
        auto clear = [&] { binding::script_clear_global(g_state, registration.name.c_str()); };
        wxl::host::Fault fault;
        if (!wxl::core::services::script::Keep("wxl.script's withdrawal of a function", clear, &fault))
            ReportFault(("withdrawing " + registration.name).c_str(), fault);
    }

    // --- The native side of a registered function -------------------------------------------

    /// One call into a mod's function as the guard runs it: the C call, with C++ exceptions
    /// caught here and logged, since nothing may leave towards Lua. The slot rather than the
    /// name, so that the call itself copies no string; only the paths below read the name.
    struct ModCall
    {
        wxl_script_call* call;
        wxl_script_fn    fn;
        void*            user;
        uint32_t         slot;
    };

    /// The name a slot is registered under, for a message. Checks the slot again: the call that
    /// is being reported on may have registered or withdrawn functions in between.
    const char* SlotName(uint32_t slot)
    {
        if (slot == 0 || slot > g_functions.size()) return "(gone)";
        return g_functions[slot - 1].name.c_str();
    }

    void InvokeMod(void* context)
    {
        ModCall* mod = static_cast<ModCall*>(context);
        try
        {
            mod->fn(mod->call, mod->user);
        }
        catch (const std::exception& e)
        {
            WLOG_ERROR(kTag, "script function %s threw: %s", SlotName(mod->slot), e.what());
        }
        catch (...)
        {
            WLOG_ERROR(kTag, "script function %s threw something that is not a std::exception",
                       SlotName(mod->slot));
        }
    }

    /// What every registered global is: reads the slot out of the closure, runs the mod's
    /// function under the guard, and answers Lua with what it pushed or with an error. Nothing
    /// with a destructor is live when script_error is called, since that call does not return.
    int32_t WXL_CALL NativeThunk(void* state)
    {
        const uint32_t slot = binding::script_closure_slot(state);
        if (!g_live || slot == 0 || slot > g_functions.size() || !g_functions[slot - 1].live)
        {
            binding::script_error(state, "WarcraftXL: this function belongs to a mod that no "
                                         "longer provides it");
            return 0;
        }

        // Copied out: the function may register another and move the vector. The name is not,
        // since only the fault paths below need it and SlotName reads it back there.
        const wxl_script_fn fn    = g_functions[slot - 1].fn;
        void* const         user  = g_functions[slot - 1].user;
        const uint32_t      owner = g_functions[slot - 1].owner;

        wxl_script_call call{};
        call.state     = state;
        call.arg_count = binding::script_top(state);
        call.open      = true;

        ModCall             mod{&call, fn, user, slot};
        wxl::host::ModHost* host     = wxl::core::Host();
        const uint32_t      previous = host ? host->SwapCurrentMod(owner) : 0;
        wxl::host::Fault    fault;
        const bool          returned = wxl::host::CallGuarded(&InvokeMod, &mod, &fault);
        if (host) host->SwapCurrentMod(previous);
        call.open = false;

        if (!returned)
        {
            char what[96];
            wxl::host::DescribeFault(fault, what, sizeof what);
            WLOG_ERROR(kTag, "script function %s of mod %s faulted: %s", SlotName(slot),
                       ModName(owner), what);
            if (host && owner != 0)
                host->Disable(owner, std::string("in its script function ") + SlotName(slot));
            else
                Withdraw(slot);
            std::snprintf(call.message, sizeof call.message,
                          "WarcraftXL: %s faulted (%s); the mod providing it is disabled",
                          SlotName(slot), what);
            binding::script_error(state, call.message);
            return 0;
        }

        if (call.failed)
        {
            binding::script_error(state, call.message);
            return 0;
        }
        return call.pushed;
    }

    // --- The table ----------------------------------------------------------------------------

    uint32_t WXL_CALL Context(void) { return g_context; }

    wxl_bool WXL_CALL LiveNow(void) { return g_live ? WXL_TRUE : WXL_FALSE; }

    wxl_result WXL_CALL RegisterFunction(const char* name, wxl_script_fn fn, void* user)
    {
        if (!name || !*name || !fn) return WXL_E_INVALID;
        if (!g_live)
        {
            WLOG_WARN(kTag, "register_function '%s': no script context is live; register from a "
                            "ScriptVmReady handler",
                      name);
            return WXL_E_FAILED;
        }
        bool             taken = false;
        auto             look  = [&] { taken = binding::script_global_is_set(g_state, name) != WXL_FALSE; };
        wxl::host::Fault fault;
        if (!wxl::core::services::script::Keep("register_function", look, &fault))
        {
            ReportFault((std::string("register_function '") + name + "'").c_str(), fault);
            return WXL_E_FAILED;
        }
        if (taken)
        {
            WLOG_WARN(kTag, "register_function '%s': the name is already taken in this context",
                      name);
            return WXL_E_INVALID;
        }

        if (!code::Handable(reinterpret_cast<const void*>(&NativeThunk), "register_function")) return WXL_E_FAILED;

        const uint32_t owner = CurrentMod();
        g_functions.push_back(Registration{name, fn, user, owner, true});
        const uint32_t slot = static_cast<uint32_t>(g_functions.size());
        auto set = [&] { binding::script_set_global_closure(g_state, name, &NativeThunk, slot); };
        if (!wxl::core::services::script::Keep("register_function", set, &fault))
        {
            ReportFault((std::string("register_function '") + name + "'").c_str(), fault);
            g_functions.back().live = false;
            return WXL_E_FAILED;
        }
        WLOG_DEBUG(kTag, "function %s registered by %s", name, ModName(owner));
        return WXL_OK;
    }

    wxl_result WXL_CALL RegisterEvent(const char* name)
    {
        if (!name || !*name) return WXL_E_INVALID;
        if (!g_live)
        {
            WLOG_WARN(kTag, "register_event '%s': no script context is live; declare it from a "
                            "ScriptVmReady handler",
                      name);
            return WXL_E_FAILED;
        }
        if (EventIndex(name) >= 0) return WXL_OK;

        g_events.push_back(name);
        Reissue();
        WLOG_DEBUG(kTag, "event %s declared by %s at index %d", name, ModName(CurrentMod()),
                   EventIndex(name));
        return WXL_OK;
    }

    wxl_result WXL_CALL FireEvent(const char* name, const wxl_value* args, uint32_t count)
    {
        if (!name || (count && !args)) return WXL_E_INVALID;
        if (!g_live) return WXL_E_FAILED;
        const int32_t index = EventIndex(name);
        if (index < 0) return WXL_E_NOT_FOUND;

        // As the client's own signal does, the event's name goes first: a frame's OnEvent receives
        // (self, event, ...) and the legacy `event` global is set from it.
        auto signal = [&]
        {
            binding::script_push_string(g_state, name, static_cast<uint32_t>(std::strlen(name)));
            for (uint32_t i = 0; i < count; ++i) Push(g_state, args[i]);
            binding::script_signal_event(static_cast<uint32_t>(index), g_state,
                                         static_cast<int32_t>(count + 1));
            binding::script_pop(g_state, static_cast<int32_t>(count + 1));
        };
        wxl::host::Fault fault;
        if (!wxl::core::services::script::Keep("fire_event", signal, &fault))
        {
            ReportFault((std::string("fire_event ") + name).c_str(), fault);
            return WXL_E_FAILED;
        }
        return WXL_OK;
    }

    wxl_result WXL_CALL Run(wxl_str chunk, const char* name)
    {
        if (!chunk.data && chunk.size) return WXL_E_INVALID;
        if (!g_live) return WXL_E_FAILED;
        const std::string source(chunk.data ? chunk.data : "", chunk.size);
        const char* const label = name ? name : "WarcraftXL";
        auto              run   = [&] { binding::script_run(source.c_str(), label); };
        wxl::host::Fault  fault;
        if (!wxl::core::services::script::Keep("run", run, &fault))
        {
            ReportFault((std::string("run ") + label).c_str(), fault);
            return WXL_E_FAILED;
        }
        return WXL_OK;
    }

    uint32_t WXL_CALL ArgCount(wxl_script_call* call)
    {
        return call && call->open ? static_cast<uint32_t>(call->arg_count) : 0u;
    }

    wxl_bool WXL_CALL ArgNumber(wxl_script_call* call, uint32_t index, double* out)
    {
        const int32_t at = ArgIndex(call, index);
        if (!at || !out) return WXL_FALSE;
        if (binding::script_value_type(call->state, at) != WXL_VALUE_NUMBER) return WXL_FALSE;
        *out = binding::script_to_number(call->state, at);
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL ArgString(wxl_script_call* call, uint32_t index, wxl_str* out)
    {
        const int32_t at = ArgIndex(call, index);
        if (!at || !out) return WXL_FALSE;
        if (binding::script_value_type(call->state, at) != WXL_VALUE_STRING) return WXL_FALSE;
        uint32_t    length = 0;
        const char* text   = binding::script_to_string(call->state, at, &length);
        if (!text) return WXL_FALSE;
        out->data = text;
        out->size = length;
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL ArgBool(wxl_script_call* call, uint32_t index, wxl_bool* out)
    {
        const int32_t at = ArgIndex(call, index);
        if (!at || !out) return WXL_FALSE;
        if (binding::script_value_type(call->state, at) != WXL_VALUE_BOOL) return WXL_FALSE;
        *out = binding::script_to_bool(call->state, at);
        return WXL_TRUE;
    }

    uint32_t WXL_CALL ArgType(wxl_script_call* call, uint32_t index)
    {
        const int32_t at = ArgIndex(call, index);
        return at ? binding::script_value_type(call->state, at) : WXL_VALUE_NIL;
    }

    void WXL_CALL PushNumber(wxl_script_call* call, double value)
    {
        if (!call || !call->open) return;
        binding::script_push_number(call->state, value);
        ++call->pushed;
    }

    void WXL_CALL PushString(wxl_script_call* call, wxl_str value)
    {
        if (!call || !call->open) return;
        binding::script_push_string(call->state, value.data ? value.data : "",
                                    value.data ? value.size : 0u);
        ++call->pushed;
    }

    void WXL_CALL PushBool(wxl_script_call* call, wxl_bool value)
    {
        if (!call || !call->open) return;
        binding::script_push_bool(call->state, value);
        ++call->pushed;
    }

    void WXL_CALL PushNil(wxl_script_call* call)
    {
        if (!call || !call->open) return;
        binding::script_push_nil(call->state);
        ++call->pushed;
    }

    void WXL_CALL Fail(wxl_script_call* call, wxl_str message)
    {
        if (!call || !call->open) return;
        call->failed = true;
        std::snprintf(call->message, sizeof call->message, "%.*s",
                      message.data ? static_cast<int>(message.size) : 0,
                      message.data ? message.data : "");
    }

    const wxl_script_api g_api = {
        sizeof(wxl_script_api),
        &Context,
        &RegisterFunction,
        &RegisterEvent,
        &FireEvent,
        &Run,
        &ArgCount,
        &ArgNumber,
        &ArgString,
        &ArgBool,
        &ArgType,
        &PushNumber,
        &PushString,
        &PushBool,
        &PushNil,
        &Fail,
        &LiveNow,
    };
}

namespace wxl::core::services
{
    const wxl_script_api& ScriptApi() { return g_api; }

    namespace script
    {
        void OnContextReady(uint32_t context, void* state)
        {
            g_live    = true;
            g_context = context;
            g_state   = state;
            g_functions.clear();
            g_events.clear();
            g_clientNames = nullptr;
            g_clientCount = 0;
        }

        void OnContextClosing()
        {
            g_live  = false;
            g_state = nullptr;
            g_functions.clear();
            g_events.clear();
            g_clientNames = nullptr;
            g_clientCount = 0;
        }

        void OnClientEventsInstalled(const char** names, uint32_t count)
        {
            g_clientNames = names;
            g_clientCount = names ? count : 0;
            if (!g_events.empty()) Reissue();
        }

        bool Reissuing() { return g_reissuing; }

        const char* EventName(uint32_t index)
        {
            if (index < g_clientCount) return g_clientNames ? g_clientNames[index] : nullptr;
            index -= g_clientCount;
            return index < g_events.size() ? g_events[index].c_str() : nullptr;
        }

        void OnModDisabled(uint32_t token)
        {
            if (token == 0) return;
            for (size_t slot = 1; slot <= g_functions.size(); ++slot)
                if (g_functions[slot - 1].owner == token) Withdraw(slot);
        }

        bool     Live() { return g_live; }
        uint32_t Context() { return g_context; }
        void*    State() { return g_state; }

        bool Keep(const char* path, wxl::host::GuardedFn fn, void* context, wxl::host::Fault* fault)
        {
            wxl::host::Fault ignored;
            if (!fault) fault = &ignored;
            void* const state = g_live ? g_state : nullptr;
            if (!state) return wxl::host::CallGuarded(fn, context, fault);

            wxl_script_mark mark{};
            binding::script_mark(state, &mark);
            const int32_t top      = binding::script_top(state);
            const bool    returned = wxl::host::CallGuarded(fn, context, fault);
            // The context may have closed inside: its state is gone, and nothing is left to keep.
            if (!g_live || g_state != state) return returned;

            if (binding::script_unwind(state, &mark))
            {
                // A fault caught further in (a mod's handler under the host's guard) left them.
                if (returned && FirstTime(path, "calls"))
                    WLOG_WARN(kTag, "%s left calls unfinished on the client's script state, a fault inside "
                                    "them having been caught further in; they are dropped",
                              path);
                return returned;
            }
            if (!returned) return false;

            const int32_t now = binding::script_top(state);
            if (now == top) return true;
            if (FirstTime(path, "top"))
            {
                if (now > top)
                    WLOG_WARN(kTag, "%s left %d value(s) on the client's script stack; they are dropped, "
                                    "since the client ends the process when the stack is not empty between frames",
                              path, now - top);
                else
                    WLOG_WARN(kTag, "%s took %d value(s) it had not pushed off the client's script stack; "
                                    "nils stand in for them",
                              path, top - now);
            }
            if (now > top) binding::script_pop(state, now - top);
            for (int32_t i = now; i < top; ++i) binding::script_push_nil(state);
            return true;
        }
    }
}
