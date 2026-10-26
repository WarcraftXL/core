// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Memory the CPU writes for one frame and the GPU reads in that frame: the bytes of the textures
// and buffers on their way to the device (staging), and each draw's data (its address pushed to
// the shaders). One list of mapped chunks per frame in flight; a frame takes its slot's list
// back once the slot's previous frame has completed, and grows it with a chunk when it needs
// more. Nothing waits, nothing is freed while the GPU may read it.

#pragma once

#include "gx/vk/resources/Resources.hpp"

#include <cstdint>
#include <vector>

namespace wxl::vk::resources
{
    class Ring
    {
    public:
        struct Span
        {
            VkBuffer        buffer = VK_NULL_HANDLE;
            VkDeviceSize    offset = 0;
            uint8_t*        data = nullptr;
            VkDeviceAddress address = 0; ///< when the ring was made with device addresses
        };

        struct Stats
        {
            uint64_t bytes = 0;  ///< handed out in the current frame
            uint64_t chunks = 0; ///< alive, over every slot
            uint64_t peak = 0;   ///< the most one frame took
        };

        Ring() = default;
        Ring(const Ring&)            = delete;
        Ring& operator=(const Ring&) = delete;
        ~Ring() { Destroy(); }

        /// @p slots frames in flight, chunks of @p chunk bytes of @p usage (with addresses when it says so).
        bool Create(Allocator& allocator, VkBufferUsageFlags usage, VkDeviceSize chunk, uint32_t slots);
        /// The GPU must be done with every chunk (or the device lost).
        void Destroy();
        bool Ready() const { return allocator_ != nullptr; }

        /// The frame of @p slot begins: what its previous frame took is free again.
        void Begin(uint32_t slot);

        /// @p size bytes aligned to @p align in the current frame; false when no memory could be had.
        bool Allocate(VkDeviceSize size, VkDeviceSize align, Span& out);

        /// What the frame wrote is made visible to the GPU (non-coherent memory); before the submission.
        void Flush();

        const Stats& Counters() const { return stats_; }

    private:
        struct Chunk
        {
            Buffer       buffer;
            VkDeviceSize used = 0;
        };

        Allocator*         allocator_ = nullptr;
        VkBufferUsageFlags usage_ = 0;
        VkDeviceSize       chunk_ = 0;
        std::vector<Chunk> slots_[3];
        uint32_t           slot_ = 0;
        uint32_t           slot_count_ = 0;
        size_t             current_ = 0; ///< the chunk of the slot being filled
        Stats              stats_;
    };
}
