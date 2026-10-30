// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.net service. Claims are kept under a lock (a mod may claim from any thread); messages
// are handed out on the main thread, from the NetClient.ProcessMessage gate, the handler called
// as its mod under the crash guard with nothing held. A mod is told nothing of another's numbers
// beyond who holds them, which the log says.

#include "services/NetService.hpp"

#include "services/ModCall.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "net/Zlib.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/mod_host.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using wxl::core::services::CallMod;

    constexpr char kNetTag[] = "net";

    /// The client's own SMSG_AUTH_RESPONSE, and the result that says the session is accepted.
    constexpr uint32_t kAuthResponse = 0x1EE;
    constexpr uint8_t  kAuthOk       = 0x0C;

    struct Claim
    {
        uint32_t           mod     = 0;
        wxl_net_handler_fn handler = nullptr;
        void*              user    = nullptr;
        uint32_t           plain   = 0;     ///< for a compressed number: the one it inflates to
        bool               warned  = false; ///< a bad stream was logged once
        uint64_t           window  = 0;     ///< the second the sends below were counted in
        uint32_t           sent    = 0;
    };

    std::mutex                              g_lock;
    std::unordered_map<uint32_t, Claim>     g_claims;
    std::unordered_map<uint32_t, uint32_t>  g_heard; ///< mod -> the connection it heard on
    std::atomic<uint32_t>                   g_state{WXL_NET_OFFLINE};
    std::atomic<uint32_t>                   g_connection{0};
    uint32_t                                g_connections = 0;

    std::string Hex(uint32_t value)
    {
        char text[16];
        std::snprintf(text, sizeof text, "0x%X", static_cast<unsigned>(value));
        return text;
    }

    uint32_t Asking()
    {
        const wxl::host::ModHost* host = wxl::core::Host();
        return host ? host->CurrentMod() : 0u;
    }

    const wxl::host::Manifest* ManifestOf(uint32_t mod)
    {
        const wxl::host::ModHost* host = wxl::core::Host();
        return host ? host->ManifestOf(mod) : nullptr;
    }

    std::string ModId(uint32_t mod)
    {
        const wxl::host::Manifest* manifest = ManifestOf(mod);
        return manifest ? manifest->id : std::string("a mod");
    }

    bool OnMainThread()
    {
        const wxl::host::ModHost* host = wxl::core::Host();
        return !host || host->OnMainThread();
    }

    wxl_result Refuse(uint32_t mod, const char* call, uint32_t opcode, const std::string& why)
    {
        WLOG_WARN(kNetTag, "mod %s: %s refused %s: %s", ModId(mod).c_str(), call, Hex(opcode).c_str(), why.c_str());
        return WXL_E_INVALID;
    }

    // --- the table ---------------------------------------------------------------------------

    uint32_t WXL_CALL State() { return g_state.load(std::memory_order_relaxed); }

    wxl_result WXL_CALL ClaimOpcode(uint32_t opcode)
    {
        const uint32_t mod = Asking();
        if (!mod) return Refuse(mod, "claim_opcode", opcode, "no mod's code is running; claim from the mod's own code");
        if (opcode > WXL_NET_OPCODE_MAX)
            return Refuse(mod, "claim_opcode", opcode, "an opcode is at most 0xFFFF, the server writes it in 16 bits");
        const wxl::host::Manifest* manifest = ManifestOf(mod);
        const std::string          id       = manifest ? manifest->id : std::string();
        {
            std::lock_guard<std::mutex> lock(g_lock);
            const auto held = g_claims.find(opcode);
            if (held != g_claims.end())
            {
                if (held->second.mod == mod) return WXL_OK;
                const uint32_t holder = held->second.mod;
                // Logged outside nothing that needs the lock: the id is looked up from the host.
                WLOG_WARN(kNetTag, "mod %s: claim_opcode refused %s: %s is claimed by %s", id.c_str(), Hex(opcode).c_str(),
                          Hex(opcode).c_str(), ModId(holder).c_str());
                return WXL_E_INVALID;
            }
        }
        if (opcode <= WXL_NET_STOCK_LIMIT && !(manifest && manifest->net.stock))
            return Refuse(mod, "claim_opcode", opcode,
                          "it is one of the client's own numbers (up to 0x520); a mod's own start at 0x521, and a tool "
                          "that must use the client's says \"net\": { \"stock\": true } in its mod.json");
        if (const wxl::host::ModHost* host = wxl::core::Host())
            if (const wxl::host::DeclaredOpcodes* declared = host->Opcodes().Find(opcode))
                if (declared->served_by != id)
                    return Refuse(mod, "claim_opcode", opcode,
                                  Hex(opcode) + " is declared by " + declared->mod + " in its mod.json");
        if (manifest && !manifest->net.opcodes.empty() && !manifest->net.Declares(opcode))
            return Refuse(mod, "claim_opcode", opcode,
                          "this mod's mod.json lists its numbers in net.opcodes, and " + Hex(opcode) +
                              " is not among them");
        std::lock_guard<std::mutex> lock(g_lock);
        Claim& claim = g_claims[opcode];
        if (claim.mod && claim.mod != mod) return WXL_E_INVALID; // claimed meanwhile, by another thread
        claim.mod = mod;
        return WXL_OK;
    }

    /// Takes a number out, and the compressed numbers pointing at it lose their pairing. Locked.
    void DropLocked(uint32_t opcode)
    {
        g_claims.erase(opcode);
        for (auto& [number, claim] : g_claims)
            if (claim.plain == opcode) claim.plain = 0;
    }

    wxl_result WXL_CALL ReleaseOpcode(uint32_t opcode)
    {
        const uint32_t              mod = Asking();
        std::lock_guard<std::mutex> lock(g_lock);
        const auto held = g_claims.find(opcode);
        if (held == g_claims.end() || held->second.mod != mod) return WXL_E_NOT_FOUND;
        DropLocked(opcode);
        return WXL_OK;
    }

    uint32_t WXL_CALL ReservedOpcodeLimit() { return WXL_NET_STOCK_LIMIT; }

    wxl_result WXL_CALL SetHandler(uint32_t opcode, wxl_net_handler_fn handler, void* user)
    {
        const uint32_t              mod = Asking();
        std::lock_guard<std::mutex> lock(g_lock);
        const auto held = g_claims.find(opcode);
        if (held == g_claims.end() || held->second.mod != mod) return WXL_E_INVALID;
        held->second.handler = handler;
        held->second.user    = handler ? user : nullptr;
        return WXL_OK;
    }

    wxl_result WXL_CALL SetCompressed(uint32_t opcode, uint32_t compressed)
    {
        const uint32_t              mod = Asking();
        std::lock_guard<std::mutex> lock(g_lock);
        const auto plain = g_claims.find(opcode);
        if (plain == g_claims.end() || plain->second.mod != mod) return WXL_E_INVALID;
        if (!compressed)
        {
            for (auto& [number, claim] : g_claims)
                if (claim.plain == opcode) claim.plain = 0;
            return WXL_OK;
        }
        const auto packed = g_claims.find(compressed);
        if (compressed == opcode || packed == g_claims.end() || packed->second.mod != mod) return WXL_E_INVALID;
        packed->second.plain = opcode;
        return WXL_OK;
    }

    uint32_t WXL_CALL MaxSend() { return WXL_NET_MAX_SEND; }

    uint32_t WXL_CALL Connection() { return g_connection.load(std::memory_order_relaxed); }

    bool HeardLocked(uint32_t mod)
    {
        const uint32_t connection = g_connection.load(std::memory_order_relaxed);
        const auto     heard      = g_heard.find(mod);
        return connection && heard != g_heard.end() && heard->second == connection;
    }

    wxl_bool WXL_CALL Heard()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        return HeardLocked(Asking()) ? WXL_TRUE : WXL_FALSE;
    }

    wxl_result WXL_CALL Send(uint32_t opcode, const void* data, uint32_t size)
    {
        if (!OnMainThread()) return WXL_E_UNSUPPORTED;
        const uint32_t mod = Asking();
        if (opcode > WXL_NET_OPCODE_MAX || size > WXL_NET_MAX_SEND || (size && !data)) return WXL_E_INVALID;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            const auto held = g_claims.find(opcode);
            if (held == g_claims.end() || held->second.mod != mod) return WXL_E_INVALID;
            if (g_state.load(std::memory_order_relaxed) != WXL_NET_AUTHENTICATED) return WXL_E_FAILED;
            // The client's own numbers are known to every server; a mod's are not until its
            // module has spoken.
            if (opcode > WXL_NET_STOCK_LIMIT && !HeardLocked(mod)) return WXL_E_FAILED;
            const uint64_t second = GetTickCount64() / 1000;
            Claim&         claim  = held->second;
            if (claim.window != second)
            {
                claim.window = second;
                claim.sent   = 0;
            }
            if (claim.sent >= WXL_NET_SEND_RATE) return WXL_E_BUSY;
            ++claim.sent;
        }
        // The client's framing: the opcode as a u32, then the body.
        std::vector<uint8_t> message(4 + size);
        std::memcpy(message.data(), &opcode, 4);
        if (size) std::memcpy(message.data() + 4, data, size);
        return wxl::core::binding::net_send(message.data(), static_cast<uint32_t>(message.size())) ? WXL_OK
                                                                                                   : WXL_E_FAILED;
    }

    wxl_result WXL_CALL Deflate(const void* data, uint32_t size, void* buffer, uint64_t capacity, uint64_t* out_size)
    {
        if ((size && !data) || !out_size || (capacity && !buffer)) return WXL_E_INVALID;
        std::vector<uint8_t> stream;
        if (!wxl::core::zlib::Deflate(data, size, stream)) return WXL_E_FAILED;
        *out_size = stream.size();
        if (stream.size() > capacity) return WXL_E_INVALID;
        if (!stream.empty()) std::memcpy(buffer, stream.data(), stream.size());
        return WXL_OK;
    }

    wxl_result WXL_CALL Inflate(const void* data, uint32_t size, void* buffer, uint64_t capacity, uint64_t* out_size)
    {
        if ((size && !data) || !out_size || (capacity && !buffer)) return WXL_E_INVALID;
        std::vector<uint8_t> bytes;
        if (!wxl::core::zlib::Inflate(data, size, WXL_NET_MAX_INFLATED, bytes))
        {
            *out_size = 0;
            return WXL_E_INVALID;
        }
        *out_size = bytes.size();
        if (bytes.size() > capacity) return WXL_E_INVALID;
        if (!bytes.empty()) std::memcpy(buffer, bytes.data(), bytes.size());
        return WXL_OK;
    }

    const wxl_net_api g_net = {
        sizeof(wxl_net_api), &State,         &Send,    &ClaimOpcode, &ReleaseOpcode, &ReservedOpcodeLimit,
        &SetHandler,         &SetCompressed, &MaxSend, &Connection,  &Heard,         &Deflate,
        &Inflate,
    };

    /// Logs a bad compressed message once per number.
    void WarnBadStream(uint32_t opcode, const std::string& why)
    {
        bool first = false;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            const auto held = g_claims.find(opcode);
            if (held != g_claims.end() && !held->second.warned) first = held->second.warned = true;
        }
        if (first) WLOG_WARN(kNetTag, "malformed %s from the server: %s; dropped", Hex(opcode).c_str(), why.c_str());
    }
}

namespace wxl::core::services
{
    const wxl_net_api& NetApi() { return g_net; }

    namespace net
    {
        void OnConnected()
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_connection.store(++g_connections ? g_connections : ++g_connections, std::memory_order_relaxed);
            g_state.store(WXL_NET_CONNECTED, std::memory_order_relaxed);
            g_heard.clear();
        }

        void OnDisconnected()
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_connection.store(0, std::memory_order_relaxed);
            g_state.store(WXL_NET_OFFLINE, std::memory_order_relaxed);
            g_heard.clear();
        }

        bool Receive(uint32_t opcode, const void* body, uint32_t size)
        {
            if (opcode == kAuthResponse && size >= 1 && static_cast<const uint8_t*>(body)[0] == kAuthOk)
                g_state.store(WXL_NET_AUTHENTICATED, std::memory_order_relaxed);

            uint32_t           mod     = 0;
            uint32_t           plain   = 0;
            wxl_net_handler_fn handler = nullptr;
            void*              user    = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                const auto held = g_claims.find(opcode);
                if (held == g_claims.end()) return false;
                mod   = held->second.mod;
                plain = held->second.plain;
                // The mod's module answered on this connection: its gate opens.
                g_heard[mod] = g_connection.load(std::memory_order_relaxed);
                const auto target = g_claims.find(plain ? plain : opcode);
                if (target != g_claims.end() && target->second.mod == mod)
                {
                    handler = target->second.handler;
                    user    = target->second.user;
                }
            }

            std::vector<uint8_t> inflated;
            const void*          data   = size ? body : nullptr;
            uint32_t             length = size;
            if (plain)
            {
                // The client's own compressed form: a u32 size, then a zlib stream of that many.
                uint32_t want = 0;
                if (size < 4)
                {
                    WarnBadStream(opcode, "shorter than its u32 size");
                    return true;
                }
                std::memcpy(&want, body, 4);
                if (want > WXL_NET_MAX_INFLATED)
                {
                    WarnBadStream(opcode, "it says " + std::to_string(want) + " bytes, over the 8 MB a message may inflate to");
                    return true;
                }
                inflated.resize(want);
                if (!wxl::core::zlib::InflateExact(static_cast<const uint8_t*>(body) + 4, size - 4, inflated.data(), want))
                {
                    WarnBadStream(opcode, "its zlib stream does not inflate to the " + std::to_string(want) + " bytes it says");
                    return true;
                }
                data   = inflated.empty() ? nullptr : inflated.data();
                length = want;
            }
            if (!handler) return true;
            const uint32_t number = plain ? plain : opcode;
            const std::string what = "its wxl.net handler of " + Hex(number);
            CallMod(mod, kNetTag, what.c_str(), [&] { handler(user, number, data, length); });
            return true;
        }

        void ForgetMod(uint32_t mod)
        {
            std::lock_guard<std::mutex> lock(g_lock);
            std::vector<uint32_t> gone;
            for (const auto& [opcode, claim] : g_claims)
                if (claim.mod == mod) gone.push_back(opcode);
            for (const uint32_t opcode : gone) DropLocked(opcode);
            g_heard.erase(mod);
        }

        void Clear()
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_claims.clear();
            g_heard.clear();
            g_connection.store(0, std::memory_order_relaxed);
            g_state.store(WXL_NET_OFFLINE, std::memory_order_relaxed);
        }
    }
}
