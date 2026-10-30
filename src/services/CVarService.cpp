// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.cvar: the variables mods and the interface register, kept here by name and made as the
// client's own CVars through the binding. A registration made before the client reads
// Config.wtf waits and is made just before it (ConsoleVar.Initialize Before), so the file's
// `SET` lines set it; one made later finds the line the client kept as an unregistered variable
// and adopts its value. Every variable carries the service's callback, which the client calls
// before each set: it holds a range and a read-only variable. The client deletes the account's
// and the character's variables at login and logout; they are made again at once. The same on
// both runtimes: only how the bindings reach the client differs.

#include "services/CVarService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Code.hpp"
#include "runtime/Core.hpp"
#include "services/ScriptService.hpp"

#include "wxl/host/mod_host.hpp"
#include "wxl/services/script.h"

#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>

#if defined(_M_IX86)
    #define WXL_CVAR_CALLBACK __cdecl
#else
    #define WXL_CVAR_CALLBACK
#endif

namespace
{
    using namespace wxl::core;

    constexpr char kTag[] = "cvar";

    // The client's bits of CVar::m_flags.
    constexpr uint32_t kArchive    = 0x1;        ///< written by the saves
    constexpr uint32_t kLatch      = 0x2;        ///< a set waits in m_latchedValue
    constexpr uint32_t kLocked     = 0x4;        ///< InternalSet writes nothing (SetReadOnly)
    constexpr uint32_t kProtected  = 0x8;        ///< SetCVar only with protected actions allowed
    constexpr uint32_t kAccount    = 0x10;       ///< category: the account's settings
    constexpr uint32_t kCharacter  = 0x20;       ///< category: the character's settings
    constexpr uint32_t kCategory   = 0x30;
    constexpr uint32_t kHidden     = 0x40;       ///< the interface does not see it
    constexpr uint32_t kScript     = 0x80;       ///< the interface's RegisterCVar: never saved
    constexpr uint32_t kReadOnly   = 0x100;      ///< the console and SetCVar refuse it
    constexpr uint32_t kRegistered = 0x80000000; ///< registered by code
    constexpr uint32_t kManaged    = kArchive | kLatch | kLocked | kProtected | kCategory | kHidden | kScript | kReadOnly;

    /// The console category the console lists a mod's variables under (CATEGORY_GAME).
    constexpr uint32_t kCategoryGame = 4;

    /// The owner the interface's registrations have; mods are the host's tokens, 1 and up.
    constexpr uint32_t kInterface = 0xFFFFFFFFu;

    /// A variable registered through the service.
    struct Record
    {
        std::string key;  ///< the name without case
        std::string name; ///< as first registered
        std::string fallback;
        const char* help  = "";
        uint32_t    flags = 0;     ///< WXL_CVAR_* that register_cvar takes
        uint32_t    owner = 0;     ///< who holds it; 0 once released
        bool        byInterface = false;
        bool        made  = false; ///< the client has it
        bool        ranged = false;
        double      min = 0.0, max = 0.0;
    };

    std::deque<Record>      g_records; // never shrinks: help strings live in it
    std::deque<std::string> g_helps;   // the console keeps a variable's help by pointer

    bool g_configReading = false; ///< between Initialize's Before and After
    bool g_configRead    = false;
    int  g_selfSetting   = 0;     ///< the service is setting a variable

    std::string Key(std::string_view name)
    {
        std::string key(name);
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return key;
    }

    std::string_view View(wxl_str text) { return text.data ? std::string_view(text.data, text.size) : std::string_view(); }

    Record* Find(std::string_view name)
    {
        const std::string key = Key(name);
        for (Record& record : g_records)
            if (record.key == key) return &record;
        return nullptr;
    }

    /// The client's own comparison of two values: without case.
    bool SameValue(const char* a, const char* b)
    {
        for (;; ++a, ++b)
        {
            const int ca = std::tolower(static_cast<unsigned char>(*a));
            const int cb = std::tolower(static_cast<unsigned char>(*b));
            if (ca != cb) return false;
            if (!ca) return true;
        }
    }

    /// A whole text read as a number, spaces around it allowed.
    bool ParseNumber(const char* text, double* out)
    {
        if (!text) return false;
        while (*text == ' ' || *text == '\t') ++text;
        if (!*text) return false;
        char*        end   = nullptr;
        const double value = std::strtod(text, &end);
        if (end == text) return false;
        while (*end == ' ' || *end == '\t') ++end;
        if (*end || !std::isfinite(value)) return false;
        *out = value;
        return true;
    }

    /// The client's StringToBOOL(text, 0), as GetCVarBool reads a variable.
    bool ReadBool(const char* text)
    {
        if (!text) return false;
        switch (*text)
        {
        case '0': case 'F': case 'N': case 'f': case 'n': return false;
        case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
        case 'T': case 'Y': case 't': case 'y': return true;
        default: break;
        }
        const std::string key = Key(text);
        return key == "on" || key == "enabled";
    }

    uint32_t Caller()
    {
        const wxl::host::ModHost* host = Host();
        return host ? host->CurrentMod() : 0u;
    }

    std::string OwnerName(uint32_t owner)
    {
        if (owner == kInterface) return "the interface";
        const wxl::host::ModHost*   host   = Host();
        const wxl::host::LoadedMod* loaded = host && owner ? host->ModOf(owner) : nullptr;
        return loaded ? "mod " + loaded->id : std::string("a mod");
    }

    /// Writes a refusal to the log, naming who was refused, and answers it.
    wxl_result Refuse(wxl_result result, uint32_t caller, const char* call, std::string_view name, const std::string& why)
    {
        WLOG_WARN(kTag, "wxl.cvar.%s('%.*s') of %s refused: %s", call, static_cast<int>(name.size()), name.data(),
                  OwnerName(caller).c_str(), why.c_str());
        return result;
    }

    bool OnGameThread(const char* call)
    {
        static std::atomic<bool> said{false};
        const wxl::host::ModHost* host = Host();
        if (!host || host->OnMainThread()) return true;
        if (!said.exchange(true))
            WLOG_WARN(kTag, "wxl.cvar.%s was called off the main thread, where the client keeps its variables; it was refused",
                      call);
        return false;
    }

    /// Why a name cannot be one, or empty.
    std::string NameFault(std::string_view name)
    {
        if (name.empty()) return "a variable needs a name";
        if (name.size() > WXL_CVAR_MAX_NAME)
            return "the name is " + std::to_string(name.size()) + " characters long; the console reads at most " +
                   std::to_string(WXL_CVAR_MAX_NAME);
        for (const char c : name)
            if (c == ' ' || c == ',' || c == ';' || c == '"' || static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
                return "a name holds no space, comma, semicolon, quote or control character, which the console "
                       "and Config.wtf split lines on";
        return {};
    }

    /// Why a value cannot be held, or empty. `saved` with the name's length bounds it to a line
    /// of Config.wtf.
    std::string ValueFault(std::string_view value, bool saved, size_t nameLength)
    {
        for (const char c : value)
            if (c == '"' || c == '\r' || c == '\n' || c == '\0')
                return "a value holds no quote, line break or null character, which would break its line in Config.wtf";
        if (value.size() > WXL_CVAR_MAX_VALUE)
            return "the value is " + std::to_string(value.size()) + " characters long; the console reads at most " +
                   std::to_string(WXL_CVAR_MAX_VALUE);
        if (saved && value.size() > WXL_CVAR_MAX_SAVED_VALUE(nameLength))
            return "the value is " + std::to_string(value.size()) + " characters long; a saved variable of this name holds at most " +
                   std::to_string(WXL_CVAR_MAX_SAVED_VALUE(nameLength)) +
                   ", since the client writes each one as a line of at most 259 characters";
        return {};
    }

    bool Saved(uint32_t flags) { return (flags & (WXL_CVAR_SAVED | WXL_CVAR_ACCOUNT | WXL_CVAR_CHARACTER)) != 0; }

    bool InRange(const Record& record, const char* value)
    {
        if (!record.ranged) return true;
        double number = 0.0;
        return ParseNumber(value, &number) && number >= record.min && number <= record.max;
    }

    std::string RangeText(const Record& record)
    {
        char text[96];
        std::snprintf(text, sizeof text, "a number from %.17g to %.17g", record.min, record.max);
        return text;
    }

    /// The client's flags for a record: what its WXL_CVAR_* say, the latch only once Config.wtf
    /// is read (before, the file's line would go to the latched value, not the current one).
    uint32_t ClientFlags(const Record& record, uint32_t current)
    {
        uint32_t flags = (current & ~kManaged) | kRegistered;
        if (Saved(record.flags)) flags |= kArchive;
        if (record.flags & WXL_CVAR_ACCOUNT) flags |= kAccount;
        if (record.flags & WXL_CVAR_CHARACTER) flags |= kCharacter;
        if (record.flags & WXL_CVAR_READ_ONLY) flags |= kReadOnly;
        if ((record.flags & WXL_CVAR_LATCHED) && g_configRead) flags |= kLatch;
        if (record.flags & WXL_CVAR_PROTECTED) flags |= kProtected;
        if (record.flags & WXL_CVAR_HIDDEN) flags |= kHidden;
        return flags;
    }

    bool WXL_CVAR_CALLBACK Callback(void* cvar, const char* previous, const char* value, void*)
    {
        return wxl::core::services::cvar::Allows(cvar, previous, value);
    }

    /// Makes a record one of the client's variables, or makes it again: the client's Register,
    /// then its flags. A value the client kept for the name stays, unless the range refuses it.
    bool Make(Record& record)
    {
        if (!code::Handable(reinterpret_cast<const void*>(&Callback), "wxl.cvar's callback")) return false;
        // The client sets the default through the callback, which lets the service's own sets
        // past a read-only variable.
        ++g_selfSetting;
        void* var = binding::cvar_register(record.name.c_str(), record.help, 0, record.fallback.c_str(),
                                           reinterpret_cast<void*>(&Callback), kCategoryGame);
        --g_selfSetting;
        if (!var)
        {
            WLOG_WARN(kTag, "the client made no variable '%s'", record.name.c_str());
            return false;
        }
        binding::cvar_write_flags(var, ClientFlags(record, binding::cvar_flags(var)));
        record.made = true;
        return true;
    }

    /// The client's variable of a name, if it is registered: what GetCVar finds.
    void* Registered(std::string_view name)
    {
        const std::string text(name);
        void*             var = binding::cvar_lookup(text.c_str());
        if (!var) return nullptr;
        return (binding::cvar_flags(var) & (kRegistered | kScript)) ? var : nullptr;
    }

    /// Sets a variable as the console would, through the callback with the service's own sets
    /// let past a read-only one.
    void SetValue(void* var, const char* value)
    {
        ++g_selfSetting;
        binding::cvar_set(var, value);
        --g_selfSetting;
    }

    // --- the registration -------------------------------------------------------------------

    wxl_result Register(uint32_t owner, std::string_view name, std::string_view fallback, uint32_t flags, std::string_view help,
                        std::string* why)
    {
        if (std::string fault = NameFault(name); !fault.empty())
        {
            *why = fault;
            return WXL_E_INVALID;
        }
        if (flags & ~WXL_CVAR_ALL_FLAGS)
        {
            *why = "a flag bit outside WXL_CVAR_ALL_FLAGS names nothing";
            return WXL_E_INVALID;
        }
        if ((flags & WXL_CVAR_ACCOUNT) && (flags & WXL_CVAR_CHARACTER))
        {
            *why = "a variable is the account's or the character's, not both";
            return WXL_E_INVALID;
        }
        if ((flags & WXL_CVAR_LATCHED) && (!(flags & WXL_CVAR_SAVED) || (flags & (WXL_CVAR_ACCOUNT | WXL_CVAR_CHARACTER))))
        {
            *why = "a latched variable takes its value at the next start, from Config.wtf: it needs saved, and "
                   "neither account nor character, which are read at each login";
            return WXL_E_INVALID;
        }
        if ((flags & WXL_CVAR_READ_ONLY) && (flags & (WXL_CVAR_ACCOUNT | WXL_CVAR_CHARACTER)))
        {
            *why = "a read-only variable cannot take the account's or the character's saved value, which arrives "
                   "as a set from outside the mod; save it with saved instead";
            return WXL_E_INVALID;
        }
        if (std::string fault = ValueFault(fallback, Saved(flags), name.size()); !fault.empty())
        {
            *why = "the default: " + fault;
            return WXL_E_INVALID;
        }

        Record* record = Find(name);
        if (record && record->owner && record->owner != owner)
        {
            *why = OwnerName(record->owner) + " holds it; choose a name of your own (start it with a prefix of yours)";
            return WXL_E_INVALID;
        }
        if (!record)
        {
            if (g_configRead || g_configReading)
            {
                const std::string text(name);
                void*             var = binding::cvar_lookup(text.c_str());
                if (var && (binding::cvar_flags(var) & kRegistered))
                {
                    *why = "it is one of the client's own variables; choose a name of your own (start it with a prefix of yours)";
                    return WXL_E_INVALID;
                }
            }
            g_records.emplace_back();
            record       = &g_records.back();
            record->key  = Key(name);
            record->name = std::string(name);
        }
        if (record->ranged)
        {
            const std::string text(fallback);
            if (!InRange(*record, text.c_str()))
            {
                *why = "the default is outside its range, " + RangeText(*record);
                return WXL_E_INVALID;
            }
        }

        record->fallback    = std::string(fallback);
        record->flags       = flags;
        record->owner       = owner;
        record->byInterface = owner == kInterface;
        if (std::string_view(record->help) != help)
        {
            g_helps.emplace_back(help);
            record->help = g_helps.back().c_str();
        }

        // Before Config.wtf is read, the registration waits for ConfigReading.
        if (!g_configRead && !g_configReading) return WXL_OK;
        if (!Make(*record))
        {
            *why = "the client could not make it";
            return WXL_E_FAILED;
        }
        return WXL_OK;
    }

    // --- the table ----------------------------------------------------------------------------

    wxl_result WXL_CALL RegisterCVar(wxl_str name, wxl_str fallback, uint32_t flags, wxl_str help)
    {
        if (!OnGameThread("register_cvar")) return WXL_E_INVALID;
        const uint32_t caller = Caller();
        std::string    why;
        const wxl_result result = Register(caller, View(name), View(fallback), flags, View(help), &why);
        if (result != WXL_OK) return Refuse(result, caller, "register_cvar", View(name), why);
        WLOG_DEBUG(kTag, "%s registered %.*s", OwnerName(caller).c_str(), static_cast<int>(name.size), name.data);
        return WXL_OK;
    }

    wxl_result WXL_CALL Release(wxl_str name)
    {
        if (!OnGameThread("release")) return WXL_E_INVALID;
        Record* record = Find(View(name));
        if (!record || !record->owner || record->owner != Caller()) return WXL_E_NOT_FOUND;
        record->owner = 0;
        return WXL_OK;
    }

    wxl_result WXL_CALL SetRange(wxl_str name, double min, double max)
    {
        if (!OnGameThread("set_range")) return WXL_E_INVALID;
        const uint32_t caller = Caller();
        Record*        record = Find(View(name));
        if (!record || record->owner != caller) return WXL_E_NOT_FOUND;
        if (!std::isfinite(min) || !std::isfinite(max) || min > max)
            return Refuse(WXL_E_INVALID, caller, "set_range", View(name), "the bounds are two numbers, the first not above the second");
        if (min == 0.0 && max == 0.0)
        {
            record->ranged = false;
            return WXL_OK;
        }
        Record bounded = *record;
        bounded.ranged = true;
        bounded.min    = min;
        bounded.max    = max;
        if (!InRange(bounded, record->fallback.c_str()))
            return Refuse(WXL_E_INVALID, caller, "set_range", View(name),
                          "the default '" + record->fallback + "' is outside " + RangeText(bounded));
        record->ranged = true;
        record->min    = min;
        record->max    = max;
        if (record->made)
            if (void* var = Registered(record->name))
                if (!InRange(*record, binding::cvar_value(var))) SetValue(var, record->fallback.c_str());
        return WXL_OK;
    }

    wxl_bool WXL_CALL Exists(wxl_str name)
    {
        if (!OnGameThread("exists")) return WXL_FALSE;
        if (NameFault(View(name)).size()) return WXL_FALSE;
        if (Find(View(name))) return WXL_TRUE;
        return Registered(View(name)) ? WXL_TRUE : WXL_FALSE;
    }

    /// The text of a variable now: the client's, or the default of a registration still waiting.
    wxl_result Text(wxl_str name, const char** out)
    {
        if (NameFault(View(name)).size()) return WXL_E_NOT_FOUND;
        const Record* record = Find(View(name));
        if (record && !record->made)
        {
            *out = record->fallback.c_str();
            return WXL_OK;
        }
        void* var = Registered(View(name));
        if (!var) return WXL_E_NOT_FOUND;
        const char* value = binding::cvar_value(var);
        *out              = value ? value : "";
        return WXL_OK;
    }

    wxl_result WXL_CALL Get(wxl_str name, wxl_str* out)
    {
        if (!OnGameThread("get")) return WXL_E_INVALID;
        if (!out) return WXL_E_INVALID;
        const char*      text   = nullptr;
        const wxl_result result = Text(name, &text);
        if (result != WXL_OK) return result;
        *out = wxl_str{text, static_cast<uint32_t>(std::strlen(text))};
        return WXL_OK;
    }

    wxl_result WXL_CALL GetNumber(wxl_str name, double* out)
    {
        if (!OnGameThread("get_number")) return WXL_E_INVALID;
        if (!out) return WXL_E_INVALID;
        const char*      text   = nullptr;
        const wxl_result result = Text(name, &text);
        if (result != WXL_OK) return result;
        return ParseNumber(text, out) ? WXL_OK : WXL_E_INVALID;
    }

    wxl_result WXL_CALL GetBool(wxl_str name, wxl_bool* out)
    {
        if (!OnGameThread("get_bool")) return WXL_E_INVALID;
        if (!out) return WXL_E_INVALID;
        const char*      text   = nullptr;
        const wxl_result result = Text(name, &text);
        if (result != WXL_OK) return result;
        *out = ReadBool(text) ? WXL_TRUE : WXL_FALSE;
        return WXL_OK;
    }

    wxl_result WXL_CALL GetDefault(wxl_str name, wxl_str* out)
    {
        if (!OnGameThread("get_default")) return WXL_E_INVALID;
        if (!out) return WXL_E_INVALID;
        if (NameFault(View(name)).size()) return WXL_E_NOT_FOUND;
        if (const Record* record = Find(View(name)))
        {
            *out = wxl_str{record->fallback.c_str(), static_cast<uint32_t>(record->fallback.size())};
            return WXL_OK;
        }
        void* var = Registered(View(name));
        if (!var) return WXL_E_NOT_FOUND;
        const char* text = binding::cvar_default(var);
        if (!text) text = "";
        *out = wxl_str{text, static_cast<uint32_t>(std::strlen(text))};
        return WXL_OK;
    }

    wxl_result SetText(const char* call, wxl_str name, std::string_view value)
    {
        if (!OnGameThread(call)) return WXL_E_INVALID;
        const uint32_t caller = Caller();
        if (NameFault(View(name)).size()) return WXL_E_NOT_FOUND;
        const Record* record = Find(View(name));
        if (record && !record->made)
            return Refuse(WXL_E_PENDING, caller, call, View(name),
                          "the client has not read Config.wtf yet; set it from WXL_STAGE_STORAGE on");
        void* var = Registered(View(name));
        if (!var) return WXL_E_NOT_FOUND;
        const uint32_t flags = binding::cvar_flags(var);
        if (flags & kLocked) return Refuse(WXL_E_INVALID, caller, call, View(name), "the client keeps it read-only");
        if ((flags & kReadOnly) && !(record && record->owner == caller))
            return Refuse(WXL_E_INVALID, caller, call, View(name),
                          record ? "it is read-only to everyone but " + OwnerName(record->owner)
                                 : std::string("the client keeps it read-only"));
        const bool saved = (flags & kArchive) && !(flags & kScript);
        if (std::string fault = ValueFault(value, saved, name.size); !fault.empty())
            return Refuse(WXL_E_INVALID, caller, call, View(name), fault);
        const std::string text(value);
        if (record && !InRange(*record, text.c_str()))
            return Refuse(WXL_E_INVALID, caller, call, View(name), "'" + text + "' is not " + RangeText(*record));
        SetValue(var, text.c_str());
        return WXL_OK;
    }

    wxl_result WXL_CALL Set(wxl_str name, wxl_str value) { return SetText("set", name, View(value)); }

    wxl_result WXL_CALL SetNumber(wxl_str name, double value)
    {
        if (!std::isfinite(value)) return Refuse(WXL_E_INVALID, Caller(), "set_number", View(name), "the value is no number");
        char text[64];
        const auto written = std::to_chars(text, text + sizeof text, value);
        return SetText("set_number", name, std::string_view(text, static_cast<size_t>(written.ptr - text)));
    }

    wxl_result WXL_CALL SetBool(wxl_str name, wxl_bool value) { return SetText("set_bool", name, value ? "1" : "0"); }

    wxl_result WXL_CALL Reset(wxl_str name)
    {
        if (!OnGameThread("reset")) return WXL_E_INVALID;
        if (NameFault(View(name)).size()) return WXL_E_NOT_FOUND;
        std::string fallback;
        if (const Record* record = Find(View(name)))
            fallback = record->fallback;
        else if (void* var = Registered(View(name)))
        {
            const char* text = binding::cvar_default(var);
            if (!text) return Refuse(WXL_E_INVALID, Caller(), "reset", View(name), "the variable has no default");
            fallback = text;
        }
        else
            return WXL_E_NOT_FOUND;
        return SetText("reset", name, fallback);
    }

    wxl_result WXL_CALL Flags(wxl_str name, uint32_t* out)
    {
        if (!OnGameThread("flags")) return WXL_E_INVALID;
        if (!out) return WXL_E_INVALID;
        if (NameFault(View(name)).size()) return WXL_E_NOT_FOUND;
        const Record* record = Find(View(name));
        if (record && !record->made)
        {
            *out = record->flags | (record->byInterface ? WXL_CVAR_INTERFACE : WXL_CVAR_MOD);
            return WXL_OK;
        }
        void* var = Registered(View(name));
        if (!var) return WXL_E_NOT_FOUND;
        const uint32_t flags  = binding::cvar_flags(var);
        uint32_t       answer = 0;
        if ((flags & kArchive) && !(flags & kScript))
        {
            if ((flags & kCategory) == kAccount)
                answer |= WXL_CVAR_ACCOUNT;
            else if ((flags & kCategory) == kCharacter)
                answer |= WXL_CVAR_CHARACTER;
            else
                answer |= WXL_CVAR_SAVED;
        }
        if (flags & (kReadOnly | kLocked)) answer |= WXL_CVAR_READ_ONLY;
        if (flags & kLatch) answer |= WXL_CVAR_LATCHED;
        if (flags & kProtected) answer |= WXL_CVAR_PROTECTED;
        if (flags & kHidden) answer |= WXL_CVAR_HIDDEN;
        if (binding::cvar_latched(var)) answer |= WXL_CVAR_PENDING;
        if (record)
            answer |= record->byInterface ? WXL_CVAR_INTERFACE : WXL_CVAR_MOD;
        else if (flags & kRegistered)
            answer |= WXL_CVAR_CLIENT;
        else
            answer |= WXL_CVAR_INTERFACE;
        *out = answer;
        return WXL_OK;
    }

    const wxl_cvar_api g_api = {
        sizeof(wxl_cvar_api),
        &RegisterCVar,
        &Release,
        &SetRange,
        &Exists,
        &Get,
        &GetNumber,
        &GetBool,
        &GetDefault,
        &Set,
        &SetNumber,
        &SetBool,
        &Reset,
        &Flags,
    };

    // --- WxlRegisterCVar: the interface's own registration --------------------------------------

    constexpr const char kUsage[] = "Usage: WxlRegisterCVar(\"name\", default [, flags [, \"help\"]])";

    /// Flags as the interface writes them: a number, or names apart by spaces, commas or bars.
    bool InterfaceFlags(std::string_view text, uint32_t* out, std::string* why)
    {
        static constexpr const char* kNames[] = {"saved", "account", "character", "read_only", "latched", "protected", "hidden"};
        uint32_t flags = 0;
        size_t   at    = 0;
        while (at < text.size())
        {
            while (at < text.size() && (text[at] == ' ' || text[at] == ',' || text[at] == '|')) ++at;
            size_t end = at;
            while (end < text.size() && text[end] != ' ' && text[end] != ',' && text[end] != '|') ++end;
            if (end == at) break;
            const std::string word = Key(text.substr(at, end - at));
            bool              known = false;
            for (uint32_t i = 0; i < sizeof kNames / sizeof kNames[0]; ++i)
                if (word == kNames[i])
                {
                    flags |= 1u << i;
                    known = true;
                }
            if (!known)
            {
                *why = "WxlRegisterCVar: no flag is named \"" + word +
                       "\"; the flags are saved, account, character, read_only, latched, protected and hidden";
                return false;
            }
            at = end;
        }
        *out = flags;
        return true;
    }

    void WXL_CALL InterfaceRegister(wxl_script_call* call, void*)
    {
        const wxl_script_api& script = services::ScriptApi();
        wxl_str               name{};
        if (!script.arg_string(call, 0, &name) || script.arg_type(call, 0) != WXL_VALUE_STRING)
        {
            script.fail(call, wxl_str{kUsage, static_cast<uint32_t>(sizeof kUsage - 1)});
            return;
        }
        // The default: a string, a number as the interface writes it, a boolean as 1 or 0.
        std::string fallback;
        wxl_str     text{};
        wxl_bool    on     = WXL_FALSE;
        double      number = 0.0;
        if (script.arg_type(call, 1) == WXL_VALUE_BOOL && script.arg_bool(call, 1, &on))
            fallback = on ? "1" : "0";
        else if (script.arg_number(call, 1, &number))
        {
            char written[32];
            std::snprintf(written, sizeof written, "%.14g", number); // as the interface's tostring writes it
            fallback = written;
        }
        else if (script.arg_string(call, 1, &text))
            fallback.assign(text.data ? text.data : "", text.size);
        else
        {
            script.fail(call, wxl_str{kUsage, static_cast<uint32_t>(sizeof kUsage - 1)});
            return;
        }
        std::string why;
        uint32_t    flags = 0;
        double      mask  = 0.0;
        if (script.arg_type(call, 2) == WXL_VALUE_NUMBER && script.arg_number(call, 2, &mask))
            flags = mask >= 0.0 && mask <= 4294967295.0 ? static_cast<uint32_t>(mask) : ~0u;
        else if (script.arg_string(call, 2, &text) && !InterfaceFlags(View(text), &flags, &why))
        {
            script.fail(call, wxl_str{why.c_str(), static_cast<uint32_t>(why.size())});
            return;
        }
        std::string help;
        if (script.arg_string(call, 3, &text)) help.assign(text.data ? text.data : "", text.size);

        const std::string_view key(name.data ? name.data : "", name.size);
        if (Register(kInterface, key, fallback, flags, help, &why) != WXL_OK)
        {
            Refuse(WXL_E_INVALID, kInterface, "register_cvar", key, why);
            const std::string message = "WxlRegisterCVar(\"" + std::string(key) + "\"): " + why;
            script.fail(call, wxl_str{message.c_str(), static_cast<uint32_t>(message.size())});
            return;
        }
        script.push_bool(call, WXL_TRUE);
    }
}

namespace wxl::core::services
{
    const wxl_cvar_api& CVarApi() { return g_api; }

    namespace cvar
    {
        void ConfigReading()
        {
            g_configReading = true;
            for (Record& record : g_records)
                if (!record.made) Make(record);
        }

        void ConfigRead()
        {
            g_configReading = false;
            g_configRead    = true;
            for (Record& record : g_records)
            {
                if (!record.made && !Make(record)) continue;
                if (!(record.flags & WXL_CVAR_LATCHED)) continue;
                if (void* var = binding::cvar_lookup(record.name.c_str()))
                    binding::cvar_write_flags(var, ClientFlags(record, binding::cvar_flags(var)));
            }
        }

        void CategoryDeleted(uint32_t category)
        {
            const uint32_t flag = category == kAccount ? WXL_CVAR_ACCOUNT : WXL_CVAR_CHARACTER;
            for (Record& record : g_records)
                if ((record.flags & flag) && record.made)
                {
                    record.made = false;
                    Make(record);
                }
        }

        void OnContextReady()
        {
            const wxl_script_api& script = ScriptApi();
            if (script.register_function("WxlRegisterCVar", &InterfaceRegister, nullptr) != WXL_OK)
                WLOG_WARN(kTag, "the interface was not given WxlRegisterCVar");
        }

        void ForgetMod(uint32_t mod)
        {
            for (Record& record : g_records)
                if (record.owner == mod) record.owner = 0;
        }

        void Clear()
        {
            for (Record& record : g_records) record.owner = 0;
        }

        bool Allows(void* cvar, const char* previous, const char* value)
        {
            if (!cvar || !value) return true;
            const char*   name   = binding::cvar_name(cvar);
            const Record* record = name ? Find(name) : nullptr;
            if (!record) return true;
            const bool same = previous && SameValue(previous, value);
            if ((record->flags & WXL_CVAR_READ_ONLY) && !same && !g_selfSetting && !g_configReading) return false;
            return InRange(*record, value);
        }
    }
}
