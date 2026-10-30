// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.net service: opcode numbers mods claim, the server's messages of them handed to their
// handlers by the NetClient.ProcessMessage gate, and the mods' own sent through the client's
// ClientServices::Send, behind the send gate. The same on both builds; the binding sends.

#pragma once

#include "wxl/services/net.h"

#include <cstdint>

namespace wxl::core::services
{
    /// The table, WXL_NET_SERVICE version WXL_NET_SERVICE_VERSION.
    const wxl_net_api& NetApi();

    namespace net
    {
        /// The world connection came up (NetClient::HandleConnect): a new connection number,
        /// every send gate closed.
        void OnConnected();

        /// It went (NetClient::HandleDisconnect): no connection, every send gate closed.
        void OnDisconnected();

        /**
         * @brief A message from the server, after MessageReceived let it through: true when a mod
         *        claimed its number and it must go no further.
         *
         * Main thread. @p body is what follows the u16 opcode. Hands the message to the claiming
         * mod's handler (inflated first for a compressed number), opens that mod's send gate, and
         * notes the session accepted on the client's own SMSG_AUTH_RESPONSE.
         */
        bool Receive(uint32_t opcode, const void* body, uint32_t size);

        /// A mod goes: its numbers are given back, its gate forgotten.
        void ForgetMod(uint32_t mod);

        /// Everything goes, without calling anyone: the way out, and the harness.
        void Clear();
    }
}
