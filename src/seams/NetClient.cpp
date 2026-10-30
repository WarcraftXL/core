// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The NetClient seams: what the runtime does at the two functions every message goes through.

#include "wxl/seams/seams.h"

#include "binding/Binding.hpp"
#include "runtime/Core.hpp"
#include "services/NetService.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <cstdint>
#include <cstring>

namespace
{
    /// How many bytes of opcode a direction writes before the body of a message.
    constexpr uint32_t kIncomingOpcode = 2; ///< the server writes a uint16
    constexpr uint32_t kOutgoingOpcode = 4; ///< the client writes a uint32

    /// A message as the contract describes one, with the flag a handler writes to stop it.
    struct Message
    {
        wxl_bool         suppressed = WXL_FALSE;
        wxl_message_args args{};
    };

    /**
     * @brief Reads the message a store is positioned at into the contract's view of one.
     *
     * The store is read, never moved: its read cursor is where the client's own parsing will
     * start, so disturbing it would change what the client sees. Answers false when the store
     * holds nothing or too little to name an opcode, which is also what a store that has been
     * reset looks like -- its cursor is 0xFFFFFFFF, past the end.
     */
    bool ReadMessage(void** store, uint32_t opcode_bytes, Message& message)
    {
        if (!store || !*store) return false;

        const void*    data = wxl::core::binding::message_data(*store);
        const uint32_t size = wxl::core::binding::message_size(*store);
        const uint32_t read = wxl::core::binding::message_read(*store);
        if (!data || read > size || size - read < opcode_bytes) return false;

        const uint8_t* at     = static_cast<const uint8_t*>(data) + read;
        uint32_t       opcode = 0;
        std::memcpy(&opcode, at, opcode_bytes);

        message.args.opcode     = opcode;
        message.args.size       = size - read - opcode_bytes;
        message.args.data       = message.args.size ? at + opcode_bytes : nullptr;
        message.args.suppressed = &message.suppressed;
        return true;
    }
}

namespace wxl::seam::NetClient::ProcessMessage
{
    bool WXL_CALL Gate(int32_t* result, void** self, uint32_t* time_received, void** message,
                       int32_t* unused)
    {
        (void)self;
        (void)time_received;
        (void)unused;

        wxl::host::ModHost* host = wxl::core::Host();
        Message             received;
        if (!host || !ReadMessage(message, kIncomingOpcode, received)) return false;

        host->Raise(WXL_EV_MessageReceived, &received.args);
        // A number a mod claimed goes to that mod and no further, before the client's own
        // dispatch -- which drops every number from 0x51F on, and would hand a claimed number
        // of its own to its handler.
        if (!received.suppressed &&
            !wxl::core::services::net::Receive(received.args.opcode, received.args.data, received.args.size))
            return false;

        // What the client answers for an opcode no handler claimed, and the only caller
        // discards it. Leaving the store where it is costs nothing: it dies with the call.
        *result = 0;
        return true;
    }
}

namespace wxl::seam::NetClient::Send
{
    bool WXL_CALL Gate(void** self, void** message)
    {
        (void)self;

        // Called on the main thread by everything that talks to the server, and on the
        // connection's own thread by the one path that gives up on a redirect. A handler that
        // reads the world from there is answered absent by the binding rather than crashing.
        wxl::host::ModHost* host = wxl::core::Host();
        Message             sending;
        if (!host || !ReadMessage(message, kOutgoingOpcode, sending)) return false;

        host->Raise(WXL_EV_MessageSent, &sending.args);
        return sending.suppressed != WXL_FALSE;
    }
}

namespace wxl::seam::NetClient::HandleConnect
{
    void WXL_CALL After(int32_t, void*)
    {
        // Raised after, so that a handler asking the client about itself is told it is
        // connected. The address is not carried: nothing the client hands this function knows
        // it, and the contract says an empty address means the runtime does not expose one.
        wxl::core::services::net::OnConnected();
        if (wxl::host::ModHost* host = wxl::core::Host())
        {
            wxl_connection_args args{};
            host->Raise(WXL_EV_Connected, &args);
        }
    }
}

namespace wxl::seam::NetClient::HandleDisconnect
{
    void WXL_CALL Before(void*)
    {
        if (wxl::host::ModHost* host = wxl::core::Host())
        {
            wxl_connection_args args{};
            host->Raise(WXL_EV_Disconnected, &args);
        }
        // Every send gate closes with the connection.
        wxl::core::services::net::OnDisconnected();
    }
}
