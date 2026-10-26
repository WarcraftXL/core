// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The per-frame rings.

#include "gx/vk/resources/Ring.hpp"

#include <algorithm>

namespace wxl::vk::resources
{
    bool Ring::Create(Allocator& allocator, VkBufferUsageFlags usage, VkDeviceSize chunk, uint32_t slots)
    {
        Destroy();
        allocator_  = &allocator;
        usage_      = usage;
        chunk_      = chunk;
        slot_count_ = std::clamp(slots, 1u, 3u);
        slot_       = 0;
        current_    = 0;
        return true;
    }

    void Ring::Destroy()
    {
        if (allocator_)
            for (std::vector<Chunk>& chunks : slots_)
            {
                for (Chunk& c : chunks) allocator_->Destroy(c.buffer);
                chunks.clear();
            }
        allocator_ = nullptr;
        stats_     = Stats{};
    }

    void Ring::Begin(uint32_t slot)
    {
        slot_    = slot % (slot_count_ ? slot_count_ : 1);
        current_ = 0;
        for (Chunk& c : slots_[slot_]) c.used = 0;
        stats_.bytes = 0;
    }

    bool Ring::Allocate(VkDeviceSize size, VkDeviceSize align, Span& out)
    {
        if (!allocator_ || !size) return false;
        if (align < 4) align = 4;
        std::vector<Chunk>& chunks = slots_[slot_];
        for (; current_ < chunks.size(); ++current_)
        {
            Chunk&             c     = chunks[current_];
            const VkDeviceSize start = (c.used + align - 1) / align * align;
            if (start + size <= c.buffer.size)
            {
                c.used      = start + size;
                out.buffer  = c.buffer.buffer;
                out.offset  = start;
                out.data    = static_cast<uint8_t*>(c.buffer.mapped) + start;
                out.address = c.buffer.address ? c.buffer.address + start : 0;
                stats_.bytes += size;
                stats_.peak = std::max(stats_.peak, stats_.bytes);
                return true;
            }
        }
        // A new chunk at the end of the slot's list: the usual size, or the request's when it is larger.
        Chunk fresh;
        if (!allocator_->CreateBuffer(std::max(chunk_, size), usage_, Access::Upload, fresh.buffer) || !fresh.buffer.mapped)
        {
            allocator_->Destroy(fresh.buffer);
            return false;
        }
        chunks.push_back(fresh);
        ++stats_.chunks;
        current_ = chunks.size() - 1;
        return Allocate(size, align, out);
    }

    void Ring::Flush()
    {
        if (!allocator_) return;
        for (const Chunk& c : slots_[slot_])
            if (c.used) allocator_->Flush(c.buffer, 0, c.used);
    }
}
