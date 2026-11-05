// SPDX-License-Identifier: GPL-3.0-or-later
// The net service ("wxl.net"): server messages past the client's handler table, given to the extensions.

#pragma once

#include "runtime/Owners.hpp"
#include "wxl/abi/Net.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace wxl::runtime::net
{
    /// The client's own bound: NetClient::ProcessMessage drops every opcode from there on (12340).
    constexpr uint32_t kStockOpcodes = 0x51F;

    /// One opcode's handler and its owner.
    struct Handler
    {
        owners::Id     owner = owners::kCore;
        WXL_NetHandler fn    = nullptr;
        void*          user  = nullptr;
    };

    /// The opcodes the extensions took, from the first one past the client's table up. Set and Clear come
    /// from any thread, Find from the client's message dispatch.
    class Handlers
    {
    public:
        explicit Handlers(uint32_t first = kStockOpcodes) : m_first(first) {}

        /// The client's bound as read from its code, once known.
        void SetFirst(uint32_t first);
        uint32_t First() const { return m_first.load(std::memory_order_relaxed); }

        /// WXL_OK, WXL_E_INVALID_ARGUMENT or WXL_E_DENIED (another owner holds it); the owner's own is replaced.
        int Set(owners::Id owner, uint32_t opcode, WXL_NetHandler fn, void* user);
        /// WXL_OK, or WXL_E_NOT_FOUND when the owner holds no handler for it.
        int Clear(owners::Id owner, uint32_t opcode);
        /// Takes back an owner's opcodes; answers how many.
        uint32_t DropOwner(owners::Id owner);

        /// The handler of `opcode`, copied out; false when none.
        bool Find(uint32_t opcode, Handler& out) const;
        size_t Count() const;

    private:
        std::atomic<uint32_t>       m_first;
        mutable std::mutex          m_lock;
        std::map<uint32_t, Handler> m_handlers;
    };

    /// A received message as the dispatch reads it: its opcode, then its body.
    struct Message
    {
        uint32_t       opcode = 0;
        const uint8_t* body   = nullptr;
        uint32_t       size   = 0;
    };

    /// The opcode and body of a CDataStore's unread bytes (`data` holds bytes `base` on, `read` and `size`
    /// count from the stream's start); false when fewer than the two opcode bytes are left.
    bool Split(const uint8_t* data, uint32_t base, uint32_t read, uint32_t size, Message& out);

    /// What NetClient::Send is handed: the opcode as four bytes, then the body.
    std::vector<uint8_t> Frame(uint32_t opcode, const void* data, uint32_t size);

    /// Publishes wxl.net and registers its take-back; the dispatch hook is the "net" feature.
    void Publish();
}
