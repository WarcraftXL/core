// SPDX-License-Identifier: GPL-3.0-or-later
// The wxl.net service over the client's message dispatch (NetClient::ProcessMessage) and its send.

#include "runtime/net/Messages.hpp"

#include "common/Log.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "runtime/extensions/Services.hpp"
#include "wxl/Common.hpp"
#include "wxl/abi/Plugin.h"
#include "wxl/game/Binding.hpp"
#include "wxl/offsets/CDataStore.hpp"
#include "wxl/offsets/ClientServices.hpp"
#include "wxl/offsets/NetClient.hpp"

#include <cstring>

namespace wxl::runtime::net
{
    namespace
    {
        namespace off = wxl::offsets;
        using wxl::game::Native;

        /// NETSTATE: the world connection is up.
        constexpr uint32_t kConnected = 5;

        /// `cmp esi, imm32` in ProcessMessage, the immediate being the handler count.
        constexpr uint8_t kBoundOpcode[2] = { 0x81, 0xFE };

        Handlers                         g_handlers;
        off::NetClient::ProcessMessageFn g_origProcessMessage = nullptr;

        /// A message the client has no slot for and an extension took: its body goes to the handler, the
        /// message counts as read. Every other one goes on to the client unchanged.
        int32_t __fastcall ProcessMessageDetour(void* self, void* edx, uint32_t timeReceived, void* message, int32_t unused)
        {
            auto*   store = static_cast<off::layout::CDataStore*>(message);
            Message received;
            Handler handler;
            if (store
                && Split(static_cast<const uint8_t*>(store->m_data), store->m_base, store->m_read, store->m_size, received)
                && received.opcode >= g_handlers.First() && g_handlers.Find(received.opcode, handler))
            {
                store->m_read = store->m_size;
                owners::Run(handler.owner, "a message handler", [&]
                {
                    handler.fn(handler.user, received.opcode, timeReceived, received.body, received.size);
                });
                WLOG_INFO_ONCE("net: first message past the client's table (0x%X) handled", received.opcode);
                return 1;
            }
            return g_origProcessMessage(self, edx, timeReceived, message, unused);
        }

        /// The handler count ProcessMessage compares with, read from its code; false when the instruction
        /// there is not the stock one.
        __declspec(noinline) bool ReadBound(uint32_t* bound)
        {
            WXL_SEH_TRY(seh::Catch::AccessViolation)
            {
                const auto* at = reinterpret_cast<const uint8_t*>(off::NetClient::ProcessMessage_handlerCountImm);
                if (std::memcmp(at - sizeof kBoundOpcode, kBoundOpcode, sizeof kBoundOpcode) != 0) return false;
                std::memcpy(bound, at, sizeof *bound);
                return true;
            }
            WXL_SEH_EXCEPT { return false; }
        }

        __declspec(noinline) bool ConnectedNow()
        {
            WXL_SEH_TRY(seh::Catch::AccessViolation)
            {
                const auto* client = *reinterpret_cast<const uint8_t* const*>(off::ClientServices::s_currentConnection);
                if (!client) return false;
                uint32_t state;
                std::memcpy(&state, client + off::NetClient::fields::m_netState, sizeof state);
                return state == kConnected;
            }
            WXL_SEH_EXCEPT { return false; }
        }

        uint32_t __cdecl ApiFirstOpcode() { return g_handlers.First(); }

        int __cdecl ApiSetHandler(uint32_t opcode, WXL_NetHandler handler, void* user)
        {
            const owners::Id owner  = owners::Current();
            const int        status = g_handlers.Set(owner, opcode, handler, user);
            if (status == WXL_OK) WLOG_INFO("net: opcode 0x%X handled by %s", opcode, owners::Name(owner));
            else if (status == WXL_E_DENIED)
            {
                Handler held;
                g_handlers.Find(opcode, held);
                WLOG_WARN("net: %s asked for opcode 0x%X, which %s holds", owners::Name(owner), opcode, owners::Name(held.owner));
            }
            else WLOG_WARN("net: %s asked for opcode 0x%X: not one past the client's table (0x%X..0x%X)", owners::Name(owner),
                           opcode, g_handlers.First(), WXL_NET_MAX_SERVER_OPCODE);
            return status;
        }

        int __cdecl ApiClearHandler(uint32_t opcode) { return g_handlers.Clear(owners::Current(), opcode); }

        int __cdecl ApiConnected() { return ConnectedNow() ? 1 : 0; }

        /// The client's own send (ClientServices::Send2, NetClient::Send) over a store reading our bytes.
        int __cdecl ApiSend(uint32_t opcode, const void* data, uint32_t size)
        {
            if (size && !data) return WXL_E_INVALID_ARGUMENT;
            if (!ConnectedNow()) return WXL_E_NOT_FOUND;
            std::vector<uint8_t> bytes = Frame(opcode, data, size);
            alignas(8) uint8_t   store[off::CDataStore::fields::kSize];
            Native<off::CDataStore::ctor_4669A0Fn>(off::CDataStore::ctor_4669A0)(store, nullptr, bytes.data(),
                                                                                 uint32_t(bytes.size()));
            Native<off::ClientServices::Send2Fn>(off::ClientServices::Send2)(store);
            return WXL_OK;
        }

        const WXL_NetApi g_api = { sizeof(WXL_NetApi), WXL_NET_API_VERSION, &ApiFirstOpcode, &ApiSetHandler,
                                   &ApiClearHandler, &ApiConnected, &ApiSend };

        bool InstallNet()
        {
            uint32_t bound = 0;
            if (!ReadBound(&bound))
            {
                WLOG_WARN("net: NetClient::ProcessMessage is not the stock code: no message goes to the extensions");
                return false;
            }
            g_handlers.SetFirst(bound);
            if (!hook::Install("NetClient::ProcessMessage", off::NetClient::ProcessMessage,
                               reinterpret_cast<void*>(&ProcessMessageDetour),
                               reinterpret_cast<void**>(&g_origProcessMessage)))
            {
                WLOG_WARN("net: NetClient::ProcessMessage could not be hooked");
                return false;
            }
            WLOG_INFO("net: server opcodes 0x%X..0x%X go to the extensions that take them", bound, WXL_NET_MAX_SERVER_OPCODE);
            return true;
        }
    }

    void Publish()
    {
        owners::OnDisable("message handlers", [](owners::Id owner) { return g_handlers.DropOwner(owner); });
        extensions::PublishInterface(WXL_NET_API_NAME, WXL_NET_API_VERSION, const_cast<WXL_NetApi*>(&g_api));
    }
}

WXL_REGISTER_FEATURE("net", true, ::wxl::runtime::net::InstallNet)
