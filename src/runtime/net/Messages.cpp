// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/net/Messages.hpp"

#include "wxl/abi/Plugin.h"

#include <cstring>

namespace wxl::runtime::net
{
    void Handlers::SetFirst(uint32_t first) { m_first.store(first, std::memory_order_relaxed); }

    int Handlers::Set(owners::Id owner, uint32_t opcode, WXL_NetHandler fn, void* user)
    {
        if (!fn || opcode > WXL_NET_MAX_SERVER_OPCODE) return WXL_E_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> guard(m_lock);
        if (opcode < m_first) return WXL_E_INVALID_ARGUMENT;
        auto it = m_handlers.find(opcode);
        if (it != m_handlers.end() && it->second.owner != owner) return WXL_E_DENIED;
        m_handlers[opcode] = Handler{ owner, fn, user };
        return WXL_OK;
    }

    int Handlers::Clear(owners::Id owner, uint32_t opcode)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        auto it = m_handlers.find(opcode);
        if (it == m_handlers.end() || it->second.owner != owner) return WXL_E_NOT_FOUND;
        m_handlers.erase(it);
        return WXL_OK;
    }

    uint32_t Handlers::DropOwner(owners::Id owner)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        uint32_t dropped = 0;
        for (auto it = m_handlers.begin(); it != m_handlers.end();)
        {
            if (it->second.owner == owner)
            {
                it = m_handlers.erase(it);
                ++dropped;
            }
            else ++it;
        }
        return dropped;
    }

    bool Handlers::Find(uint32_t opcode, Handler& out) const
    {
        std::lock_guard<std::mutex> guard(m_lock);
        if (opcode < m_first) return false;
        auto it = m_handlers.find(opcode);
        if (it == m_handlers.end()) return false;
        out = it->second;
        return true;
    }

    size_t Handlers::Count() const
    {
        std::lock_guard<std::mutex> guard(m_lock);
        return m_handlers.size();
    }

    bool Split(const uint8_t* data, uint32_t base, uint32_t read, uint32_t size, Message& out)
    {
        if (!data || read < base || read > size || size - read < 2) return false;
        const uint8_t* at = data + (read - base);
        uint16_t       opcode;
        std::memcpy(&opcode, at, sizeof opcode);
        out.opcode = opcode;
        out.body   = at + 2;
        out.size   = size - read - 2;
        return true;
    }

    std::vector<uint8_t> Frame(uint32_t opcode, const void* data, uint32_t size)
    {
        std::vector<uint8_t> bytes(sizeof opcode + size);
        std::memcpy(bytes.data(), &opcode, sizeof opcode);
        if (size) std::memcpy(bytes.data() + sizeof opcode, data, size);
        return bytes;
    }
}
