// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The engine's draws turned into Vulkan.

#include "gx/vk/draw/Draw.hpp"

#include "gx/vk/Log.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wxl::vk::draw
{
    namespace
    {
        constexpr VkDeviceSize kStagingChunk = 8u << 20;
        constexpr VkDeviceSize kDataChunk    = 4u << 20;
        constexpr uint32_t     kZeroBinding  = 15; ///< the binding of the zeros an absent input reads

        VkImageAspectFlags AspectOf(const Texture& t)
        {
            if (!t.info.depth) return VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (t.info.format == VK_FORMAT_D24_UNORM_S8_UINT || t.info.format == VK_FORMAT_D32_SFLOAT_S8_UINT) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
            return aspect;
        }

        bool HasStencil(VkFormat format)
        {
            return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D16_UNORM_S8_UINT;
        }

        void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                          uint32_t level = 0, uint32_t levels = VK_REMAINING_MIP_LEVELS, uint32_t layer = 0,
                          uint32_t layers = VK_REMAINING_ARRAY_LAYERS)
        {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask     = srcStage;
            barrier.srcAccessMask    = srcAccess;
            barrier.dstStageMask     = dstStage;
            barrier.dstAccessMask    = dstAccess;
            barrier.oldLayout        = from;
            barrier.newLayout        = to;
            barrier.image            = image;
            barrier.subresourceRange = {aspect, level, levels, layer, layers};
            VkDependencyInfo dependency{};
            dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers    = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }

        void ImageBarrier(VkCommandBuffer cmd, const Texture& t, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                          VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, uint32_t level = 0,
                          uint32_t levels = VK_REMAINING_MIP_LEVELS)
        {
            ImageBarrier(cmd, t.image.image, AspectOf(t), from, to, srcStage, srcAccess, dstStage, dstAccess, level, levels);
        }

        void MemoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                           VkAccessFlags2 dstAccess)
        {
            VkMemoryBarrier2 barrier{};
            barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            barrier.srcStageMask  = srcStage;
            barrier.srcAccessMask = srcAccess;
            barrier.dstStageMask  = dstStage;
            barrier.dstAccessMask = dstAccess;
            VkDependencyInfo dependency{};
            dependency.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers    = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }

        constexpr VkPipelineStageFlags2 kReadStages =
            VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        constexpr VkAccessFlags2 kReads =
            VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT;
        constexpr VkPipelineStageFlags2 kTransfer = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
        constexpr VkPipelineStageFlags2 kColorStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        constexpr VkAccessFlags2 kColorAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        constexpr VkPipelineStageFlags2 kDepthStage =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        constexpr VkAccessFlags2 kDepthAccess =
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

        uint32_t MipLevels(uint32_t width, uint32_t height)
        {
            uint32_t levels = 1;
            for (uint32_t size = std::max(width, height); size > 1; size >>= 1) ++levels;
            return levels;
        }

        pipelines::TopologyClass ClassOf(uint32_t primitive)
        {
            if (primitive == kPoints) return pipelines::TopologyClass::Points;
            if (primitive == kLines || primitive == kLineStrip) return pipelines::TopologyClass::Lines;
            return pipelines::TopologyClass::Triangles;
        }

        VkPrimitiveTopology TopologyOf(uint32_t primitive)
        {
            switch (primitive)
            {
            case kPoints: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            case kLines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case kLineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case kTriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case kTriangleFan: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
            default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            }
        }

        uint32_t Bits(float value)
        {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof bits);
            return bits;
        }

        uint64_t KeyBits(const shaders::CacheKey& key) { return key.low ^ (key.high * 0x9E3779B97F4A7C15ull); }
    }

    bool ProgramByName(bool vertex, const char* name, uint16_t& program)
    {
        if (!name) return false;
        if (vertex)
        {
            if (std::strcmp(name, "UI:0") == 0)
            {
                program = static_cast<uint16_t>(pipelines::VertexProgram::Interface);
                return true;
            }
            if (std::strcmp(name, "UI:1") == 0)
            {
                program = static_cast<uint16_t>(pipelines::VertexProgram::InterfaceStereo);
                return true;
            }
            return false;
        }
        if (std::strcmp(name, "UI") == 0)
        {
            program = static_cast<uint16_t>(pipelines::PixelProgram::Interface);
            return true;
        }
        if (std::strcmp(name, "Desaturate") == 0)
        {
            program = static_cast<uint16_t>(pipelines::PixelProgram::Desaturate);
            return true;
        }
        return false;
    }

    // --- Life ------------------------------------------------------------------------------------------

    bool Renderer::Create(device::Context& context, resources::Allocator& allocator, present::Display& display, const Paths& paths,
                          char* why, uint32_t why_size)
    {
        Destroy();
        context_   = &context;
        allocator_ = &allocator;
        display_   = &display;
        formats_.Choose(context.Physical(), context.Chosen().features.textureCompressionBC, display.DepthFormat());

        VkFormatProperties color{};
        vkGetPhysicalDeviceFormatProperties(context.Physical(), VK_FORMAT_B8G8R8A8_UNORM, &color);
        color_rgba_ = (color.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) == 0;

        resources::ImageInfo black;
        black.format = VK_FORMAT_B8G8R8A8_UNORM;
        black.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        resources::ImageInfo cube = black;
        cube.cube                 = true;
        if (!allocator.CreateImage(black, black_) || !allocator.CreateImage(cube, black_cube_) ||
            !allocator.CreateBuffer(64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, resources::Access::Upload, zeros_) || !zeros_.mapped)
        {
            std::snprintf(why, why_size, "the renderer's default textures or its zero vertex could not be made");
            Destroy();
            return false;
        }
        std::memset(zeros_.mapped, 0, 64);
        allocator.Flush(zeros_, 0, 64);
        black_ready_ = false;

        const uint32_t slots = display.FrameCount();
        if (!bindless_.Create(context, 1u << 16, black_.view, why, why_size) ||
            !staging_.Create(allocator, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kStagingChunk, slots) ||
            !data_.Create(allocator, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kDataChunk, slots) ||
            !pipelines_.Create(context, bindless_.Layout(), paths.pipeline_cache,
                               pipelines::Settings{paths.async_pipelines, paths.pipeline_libraries, 0, paths.pipeline_keys}, why, why_size))
        {
            if (why[0] == '\0') std::snprintf(why, why_size, "the renderer's rings could not be made");
            Destroy();
            return false;
        }
        black_cube_slot_ = bindless_.AddImage(black_cube_.view);

        // Occlusion queries count samples only where the device counts them precisely.
        queries_supported_ = context.Chosen().features.occlusionQueryPrecise;
        if (queries_supported_)
        {
            VkQueryPoolCreateInfo info{};
            info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            info.queryType  = VK_QUERY_TYPE_OCCLUSION;
            info.queryCount = kQueriesPerFrame * frame::Frames::kMax;
            queries_supported_ = vkCreateQueryPool(context.Device(), &info, nullptr, &query_pool_) == VK_SUCCESS;
        }

        shaders::Options options;
        options.clip_distance = context.Chosen().features.shaderClipDistance;
        cache_.Open(paths.shader_pack, options);

        display.SetFrameHook(this);
        stats_ = Stats{};
        vertex_regs_ = RegisterFile{};
        pixel_regs_  = RegisterFile{};
        return true;
    }

    void Renderer::Destroy()
    {
        if (display_) display_->SetFrameHook(nullptr);
        cache_.Close();
        if (allocator_ && context_ && context_->Ready())
        {
            Collect(true);
            pipelines_.Destroy();
            bindless_.Destroy();
            staging_.Destroy();
            data_.Destroy();
            allocator_->Destroy(black_);
            allocator_->Destroy(black_cube_);
            allocator_->Destroy(target_depth_);
            allocator_->Destroy(zeros_);
            if (query_pool_) vkDestroyQueryPool(context_->Device(), query_pool_, nullptr);
        }
        query_pool_ = VK_NULL_HANDLE;
        queries_.clear();
        programs_.clear();
        dead_.clear();
        opened_.clear();
        target_ = open_ = Target{};
        target_depth_ready_ = false;
        context_   = nullptr;
        allocator_ = nullptr;
        display_   = nullptr;
        epoch_     = 0;
        last_attribute_count_ = UINT32_MAX;
    }

    void Renderer::Retire(resources::Buffer buffer)
    {
        if (!buffer.buffer) return;
        Dead d{};
        d.frame  = display_->FrameNumber();
        d.buffer = buffer;
        d.slots[0] = d.slots[1] = resources::Bindless::kNoSlot;
        dead_.push_back(d);
    }

    void Renderer::Retire(resources::Image image, VkImageView flat, uint32_t slot, uint32_t cube_slot, const VkImageView* attach)
    {
        Dead d{};
        d.frame    = display_->FrameNumber();
        d.image    = image;
        d.views[0] = flat;
        for (int i = 0; i < 6; ++i) d.views[1 + i] = attach ? attach[i] : VK_NULL_HANDLE;
        d.slots[0] = slot;
        d.slots[1] = cube_slot;
        dead_.push_back(d);
    }

    void Renderer::Collect(bool all)
    {
        const uint64_t done = all ? UINT64_MAX : display_->FramesCompleted();
        size_t         kept = 0;
        for (Dead& d : dead_)
        {
            if (d.frame > done)
            {
                dead_[kept++] = d;
                continue;
            }
            allocator_->Destroy(d.buffer);
            for (VkImageView v : d.views)
                if (v) vkDestroyImageView(context_->Device(), v, nullptr);
            allocator_->Destroy(d.image);
            bindless_.ReleaseImage(d.slots[0]);
            bindless_.ReleaseImage(d.slots[1]);
        }
        dead_.resize(kept);
    }

    // --- Programs --------------------------------------------------------------------------------------

    int Renderer::Program(bool pixel, const char* name, const void* code, size_t size, std::string* why)
    {
        uint16_t own = 0;
        if (ProgramByName(!pixel, name, own)) return own;
        if (!Ready() || !code || !size)
        {
            if (why) *why = "no record";
            return -1;
        }
        shaders::Options options;
        options.clip_distance = context_->Chosen().features.shaderClipDistance;
        // A record is one program per stage it is asked for (the wrong stage is refused once).
        const uint64_t key   = KeyBits(shaders::Cache::KeyOf(code, size, options)) ^ (pixel ? 0x8000000000000000ull : 0);
        const auto     found = programs_.find(key);
        if (found != programs_.end())
        {
            if (found->second < 0 && why) *why = "refused before";
            return found->second;
        }
        std::string reason;
        const auto  translation = cache_.Get(code, size, reason);
        int         id          = -1;
        if (!translation) {}
        else if (translation->info.pixel != pixel)
            reason = pixel ? "a vertex program where a pixel one is wanted" : "a pixel program where a vertex one is wanted";
        else
        {
            id = pipelines_.AddProgram(translation->words, translation->info, key);
            if (id < 0) reason = "its module could not be made";
        }
        if (id < 0)
        {
            ++stats_.programs_refused;
            Log(LogLevel::Debug, "the %s program %s is not translated: %s", pixel ? "pixel" : "vertex", name ? name : "?", reason.c_str());
        }
        else
            ++stats_.programs;
        programs_.emplace(key, id);
        if (why) *why = reason;
        return id;
    }

    void Renderer::Prewarm(std::vector<std::vector<uint8_t>> records) { cache_.Prewarm(std::move(records)); }

    void Renderer::Constants(bool pixel, uint32_t first, uint32_t rows, const float* values)
    {
        if (!values || first >= shaders::kRegisterRows) return;
        rows               = std::min(rows, shaders::kRegisterRows - first);
        RegisterFile& file = pixel ? pixel_regs_ : vertex_regs_;
        std::memcpy(file.rows + first * 4, values, rows * 4 * sizeof(float));
        file.dirty = true;
    }

    VkDeviceAddress Renderer::Snapshot(RegisterFile& file, uint32_t rows)
    {
        if (!rows) return 0;
        const uint64_t frame = display_->FrameNumber();
        if (!file.dirty && file.frame == frame && file.copied >= rows && file.address) return file.address;
        // A program that reads more rows than the copy holds, or registers that changed: a new copy.
        rows = std::max(rows, file.frame == frame && !file.dirty ? file.copied : 0u);
        resources::Ring::Span span;
        if (!data_.Allocate(rows * 16ull, 16, span)) return 0;
        std::memcpy(span.data, file.rows, rows * 16ull);
        stats_.register_bytes += rows * 16ull;
        file.address = span.address;
        file.copied  = rows;
        file.frame   = frame;
        file.dirty   = false;
        return file.address;
    }

    // --- The frame ---------------------------------------------------------------------------------

    void Renderer::FrameBegun(uint32_t slot, VkCommandBuffer upload)
    {
        Collect(false);
        // What the pipeline workers finished comes in between frames; what it replaced goes once done.
        pipelines_.BeginFrame(display_->FrameNumber(), display_->FramesCompleted());
        staging_.Begin(slot);
        data_.Begin(slot);
        epoch_        = 0;
        last_address_ = 0;
        slot_         = slot;
        query_next_   = 0;
        opened_.clear();
        // Nothing of this frame's upload pass may overwrite what the frames before it still read.
        MemoryBarrier(upload, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE, kTransfer,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        if (query_pool_) vkCmdResetQueryPool(upload, query_pool_, slot * kQueriesPerFrame, kQueriesPerFrame);
        // A query issued in the frame this slot last held and never read is lost with the reset.
        for (Query& q : queries_)
            if (q.alive && q.state == Query::Issued && q.slot == slot)
            {
                q.state = Query::Idle;
                q.samples = 0;
            }
        if (!black_ready_)
        {
            const VkClearColorValue       opaque{{0.0f, 0.0f, 0.0f, 1.0f}};
            for (const resources::Image* image : {&black_, &black_cube_})
            {
                ImageBarrier(upload, image->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, kTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
                vkCmdClearColorImage(upload, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &opaque, 1, &range);
                ImageBarrier(upload, image->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, kReadStages,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }
            black_ready_ = true;
        }
    }

    void Renderer::FrameEnding(VkCommandBuffer upload)
    {
        CloseUploads(upload);
        // A query begun and never drawn into is not issued.
        for (Query& q : queries_)
            if (q.alive && q.state == Query::Pending) q.state = Query::Idle;
    }

    void Renderer::FrameFlushing(VkCommandBuffer upload) { CloseUploads(upload); }

    void Renderer::CloseUploads(VkCommandBuffer upload)
    {
        for (Texture* t : opened_)
        {
            if (t->desc.generate_mips && t->image.levels > 1) GenerateMips(t, upload);
            else
                ImageBarrier(upload, *t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kTransfer,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT, kReadStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            t->defined = true;
            t->opened  = 0;
        }
        opened_.clear();
        MemoryBarrier(upload, kTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, kReadStages, kReads);
        staging_.Flush();
        data_.Flush();
    }

    void Renderer::RenderingEnding(VkCommandBuffer cmd) { EndQueries(cmd); }

    void Renderer::RenderingEnded(VkCommandBuffer cmd)
    {
        if (!open_.color) return;
        // A texture drawn into is sampled next: SHADER_READ_ONLY again, as every texture is between renderings.
        ImageBarrier(cmd, open_.color->image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kColorStage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     kReadStages | kTransfer | kColorStage, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT, 0, 1,
                     open_.color->desc.cube ? open_.face : 0, 1);
        if (open_.depth)
            ImageBarrier(cmd, *open_.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         kDepthStage, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kReadStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, 1);
        open_ = Target{};
    }

    VkCommandBuffer Renderer::Inline()
    {
        if (display_->InRendering())
        {
            display_->EndRendering();
            ++stats_.pass_breaks;
        }
        return display_->Cmd();
    }

    // --- Textures ----------------------------------------------------------------------------------------

    Texture* Renderer::CreateTexture(const TextureDesc& desc)
    {
        if (!Ready() || !desc.width || !desc.height) return nullptr;
        const FormatInfo& info = formats_.Of(desc.format);
        if (info.format == VK_FORMAT_UNDEFINED) return nullptr;

        Texture* t = new Texture();
        t->desc    = desc;
        t->info    = info;
        resources::ImageInfo image;
        image.width   = desc.width;
        image.height  = desc.height;
        image.cube    = desc.cube;
        image.format  = info.format;
        image.swizzle = info.swizzle;
        image.levels  = desc.generate_mips ? MipLevels(desc.width, desc.height) : std::max(desc.levels, 1u);
        image.usage   = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (desc.generate_mips) image.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (info.depth)
        {
            image.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            image.levels = 1;
            if (desc.target) image.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        }
        else if (desc.target)
            image.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!allocator_->CreateImage(image, t->image))
        {
            delete t;
            return nullptr;
        }
        if (desc.cube) t->flat = allocator_->CreateLayerView(t->image, 0, info.swizzle);
        t->slot = bindless_.AddImage(desc.cube ? t->flat : t->image.view);
        if (desc.cube && t->slot != resources::Bindless::kNoSlot) t->cube_slot = bindless_.AddImage(t->image.view);
        if (t->slot == resources::Bindless::kNoSlot || (desc.cube && t->cube_slot == resources::Bindless::kNoSlot))
        {
            Log(LogLevel::Warn, "the bindless table is full (%u images): a %ux%u texture is not made", bindless_.Capacity(), desc.width,
                desc.height);
            bindless_.ReleaseImage(t->slot);
            if (t->flat) vkDestroyImageView(context_->Device(), t->flat, nullptr);
            allocator_->Destroy(t->image);
            delete t;
            return nullptr;
        }
        ++stats_.textures;
        return t;
    }

    void Renderer::DestroyTexture(Texture* texture)
    {
        if (!texture) return;
        opened_.erase(std::remove(opened_.begin(), opened_.end(), texture), opened_.end());
        if (target_.color == texture || target_.depth == texture) target_ = Target{};
        if (open_.color == texture || open_.depth == texture) display_->EndRendering();
        Retire(texture->image, texture->flat, texture->slot, texture->cube_slot, texture->attach);
        --stats_.textures;
        delete texture;
    }

    void Renderer::OpenInUpload(Texture* t)
    {
        const uint64_t frame = display_->FrameNumber();
        if (t->opened == frame) return;
        const VkCommandBuffer upload = display_->Upload();
        ImageBarrier(upload, *t, t->defined ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE, kTransfer,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (!t->defined)
        {
            // What was never written reads as transparent black (D3D9's new texture holds whatever,
            // a render target the engine has not drawn into yet included); a depth as far.
            const VkImageSubresourceRange range{AspectOf(*t), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            if (t->info.depth)
            {
                const VkClearDepthStencilValue far{1.0f, 0};
                vkCmdClearDepthStencilImage(upload, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1, &range);
            }
            else if (t->info.block == 1)
            {
                // A compressed image cannot be cleared: the engine writes its levels at once.
                const VkClearColorValue zero{};
                vkCmdClearColorImage(upload, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            }
            // The copies of the engine's texels come after the clear, not alongside it.
            ImageBarrier(upload, *t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kTransfer,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, kTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
        t->opened = frame;
        opened_.push_back(t);
    }

    uint8_t* Renderer::LockTexture(Texture* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                   uint32_t& pitch)
    {
        if (!t || t->locked || !display_->InFrame() || t->info.depth || level >= t->image.levels || face >= t->image.layers) return nullptr;
        const uint32_t mipWidth  = std::max(t->desc.width >> level, 1u);
        const uint32_t mipHeight = std::max(t->desc.height >> level, 1u);
        if (x >= mipWidth || y >= mipHeight) return nullptr;
        width  = std::min(width, mipWidth - x);
        height = std::min(height, mipHeight - y);
        if (!width || !height) return nullptr;
        pitch               = Formats::Pitch(t->info, width);
        const uint32_t rows = Formats::Rows(t->info, height);
        if (!staging_.Allocate(static_cast<VkDeviceSize>(pitch) * rows, 16, t->lock_span)) return nullptr;
        t->locked     = true;
        t->lock_face  = face;
        t->lock_level = level;
        t->lock_at    = {static_cast<int32_t>(x), static_cast<int32_t>(y)};
        t->lock_size  = {width, height};
        return t->lock_span.data;
    }

    void Renderer::CopyIntoImage(Texture* t, VkCommandBuffer cmd, const resources::Ring::Span& span, uint32_t pitch)
    {
        VkBufferImageCopy region{};
        region.bufferOffset      = span.offset;
        region.bufferRowLength   = pitch / t->info.bytes * t->info.block;
        region.bufferImageHeight = 0;
        region.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, t->lock_level, t->lock_face, 1};
        region.imageOffset       = {t->lock_at.x, t->lock_at.y, 0};
        region.imageExtent       = {t->lock_size.width, t->lock_size.height, 1};
        vkCmdCopyBufferToImage(cmd, span.buffer, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        stats_.upload_bytes += static_cast<uint64_t>(pitch) * Formats::Rows(t->info, t->lock_size.height);
    }

    void Renderer::UnlockTexture(Texture* t)
    {
        if (!t || !t->locked) return;
        t->locked = false;
        if (!display_->InFrame()) return;
        const uint32_t pitch = Formats::Pitch(t->info, t->lock_size.width);
        if (t->used == display_->FrameNumber())
        {
            // A draw of this frame read it already (the glyph cache, filled while text draws): the
            // copy goes where it is, between the draws, the rendering ended around it.
            const VkCommandBuffer cmd = Inline();
            ImageBarrier(cmd, *t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kReadStages,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            CopyIntoImage(t, cmd, t->lock_span, pitch);
            if (t->desc.generate_mips && t->image.levels > 1 && t->lock_level == 0) GenerateMips(t, cmd);
            else
                ImageBarrier(cmd, *t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kTransfer,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT, kReadStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            return;
        }
        OpenInUpload(t);
        CopyIntoImage(t, display_->Upload(), t->lock_span, pitch);
    }

    void Renderer::GenerateMips(Texture* t, VkCommandBuffer cmd)
    {
        // Each level from the one above, by a linear blit; every level ends SHADER_READ_ONLY.
        uint32_t width = t->desc.width, height = t->desc.height;
        for (uint32_t level = 1; level < t->image.levels; ++level)
        {
            ImageBarrier(cmd, *t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kTransfer,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, kTransfer, VK_ACCESS_2_TRANSFER_READ_BIT, level - 1, 1);
            const uint32_t next_width = std::max(width >> 1, 1u), next_height = std::max(height >> 1, 1u);
            VkImageBlit    blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, t->image.layers};
            blit.srcOffsets[1]  = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, t->image.layers};
            blit.dstOffsets[1]  = {static_cast<int32_t>(next_width), static_cast<int32_t>(next_height), 1};
            vkCmdBlitImage(cmd, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &blit, VK_FILTER_LINEAR);
            ImageBarrier(cmd, *t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kTransfer,
                         VK_ACCESS_2_TRANSFER_READ_BIT, kReadStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, level - 1, 1);
            width  = next_width;
            height = next_height;
        }
        ImageBarrier(cmd, *t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kTransfer,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, kReadStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, t->image.levels - 1, 1);
    }

    // --- Pools ---------------------------------------------------------------------------------------------

    Pool* Renderer::CreatePool(uint32_t target, uint32_t usage, uint32_t size)
    {
        Pool* p   = new Pool();
        p->target = target;
        p->usage  = usage;
        p->size   = size;
        if (usage != kStream) p->shadow.assign(size, 0);
        ++stats_.pools;
        return p;
    }

    void Renderer::DestroyPool(Pool* pool)
    {
        if (!pool) return;
        Retire(pool->current);
        for (Pool::Retired& r : pool->retired) Retire(r.buffer);
        Retire(pool->device);
        --stats_.pools;
        delete pool;
    }

    void Renderer::ResizePool(Pool* pool, uint32_t size)
    {
        if (!pool || pool->size == size) return;
        pool->size = size;
        if (pool->usage == kStream)
        {
            Retire(pool->current);
            pool->current = resources::Buffer{};
            for (Pool::Retired& r : pool->retired) Retire(r.buffer);
            pool->retired.clear();
            return;
        }
        pool->shadow.resize(size, 0);
        Retire(pool->device);
        pool->device     = resources::Buffer{};
        pool->dirty_from = 0;
        pool->dirty_to   = size;
    }

    uint8_t* Renderer::PoolMemory(Pool* pool)
    {
        if (!pool || !pool->size) return nullptr;
        if (pool->usage != kStream) return pool->shadow.data();
        if (pool->current.buffer) return static_cast<uint8_t*>(pool->current.mapped);
        // A buffer the frames have stopped reading, or a new one.
        const uint64_t done = display_->FramesCompleted();
        for (size_t i = 0; i < pool->retired.size(); ++i)
            if (pool->retired[i].frame <= done && pool->retired[i].buffer.size == pool->size)
            {
                pool->current = pool->retired[i].buffer;
                pool->retired.erase(pool->retired.begin() + static_cast<ptrdiff_t>(i));
                return static_cast<uint8_t*>(pool->current.mapped);
            }
        const VkBufferUsageFlags usage = pool->target == kIndexPool ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (!allocator_->CreateBuffer(pool->size, usage, resources::Access::Upload, pool->current) || !pool->current.mapped)
        {
            allocator_->Destroy(pool->current);
            return nullptr;
        }
        return static_cast<uint8_t*>(pool->current.mapped);
    }

    void Renderer::DiscardPool(Pool* pool)
    {
        if (!pool || pool->usage != kStream || !pool->current.buffer) return;
        pool->retired.push_back(Pool::Retired{display_->FrameNumber(), pool->current});
        pool->current = resources::Buffer{};
    }

    void Renderer::PoolWritten(Pool* pool, uint32_t offset, uint32_t size)
    {
        if (!pool || !size) return;
        if (pool->usage == kStream)
        {
            if (pool->current.buffer) allocator_->Flush(pool->current, offset, size);
            return;
        }
        const uint32_t end = std::min(offset + size, pool->size);
        if (offset >= end) return;
        if (pool->dirty_to <= pool->dirty_from)
        {
            pool->dirty_from = offset;
            pool->dirty_to   = end;
        }
        else
        {
            pool->dirty_from = std::min(pool->dirty_from, offset);
            pool->dirty_to   = std::max(pool->dirty_to, end);
        }
    }

    void Renderer::Forget(Pool* pool)
    {
        if (!pool || !allocator_) return;
        allocator_->Destroy(pool->current);
        for (Pool::Retired& r : pool->retired) allocator_->Destroy(r.buffer);
        pool->retired.clear();
        allocator_->Destroy(pool->device);
        pool->used       = 0;
        pool->dirty_from = 0;
        pool->dirty_to   = pool->usage == kStream ? 0 : pool->size;
    }

    bool Renderer::EnsureDevice(Pool* pool)
    {
        if (pool->device.buffer) return true;
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         (pool->target == kIndexPool ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (!allocator_->CreateBuffer(std::max(pool->size, 4u), usage, resources::Access::Device, pool->device)) return false;
        pool->dirty_from = 0;
        pool->dirty_to   = pool->size;
        return true;
    }

    bool Renderer::FlushPool(Pool* pool)
    {
        if (pool->usage == kStream) return pool->current.buffer != VK_NULL_HANDLE;
        if (!EnsureDevice(pool)) return false;
        if (pool->dirty_to <= pool->dirty_from) return true;
        // Copies of a multiple of 4 bytes at a multiple of 4.
        const uint32_t from = pool->dirty_from & ~3u;
        const uint32_t to   = std::min((pool->dirty_to + 3u) & ~3u, static_cast<uint32_t>(pool->device.size));
        resources::Ring::Span span;
        if (to <= from || !staging_.Allocate(to - from, 16, span)) return false;
        std::memcpy(span.data, pool->shadow.data() + from, std::min<size_t>(to - from, pool->shadow.size() - from));
        VkBufferCopy copy{span.offset, from, to - from};
        if (pool->used == display_->FrameNumber())
        {
            // A draw of this frame read it: the copy goes between the draws.
            const VkCommandBuffer cmd = Inline();
            MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdCopyBuffer(cmd, span.buffer, pool->device.buffer, 1, &copy);
            MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                          VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT);
        }
        else
            vkCmdCopyBuffer(display_->Upload(), span.buffer, pool->device.buffer, 1, &copy);
        stats_.upload_bytes += to - from;
        pool->dirty_from = pool->dirty_to = 0;
        return true;
    }

    // --- Targets ----------------------------------------------------------------------------------------

    void Renderer::SetTarget(Texture* color, uint32_t face, Texture* depth)
    {
        target_.color = color;
        target_.face  = color && color->desc.cube ? std::min(face, 5u) : 0;
        target_.depth = depth;
    }

    VkExtent2D Renderer::TargetExtent() const
    {
        if (target_.color) return {target_.color->desc.width, target_.color->desc.height};
        if (target_.depth) return {target_.depth->desc.width, target_.depth->desc.height};
        return display_ ? display_->SceneExtent() : VkExtent2D{0, 0};
    }

    VkImageView Renderer::AttachView(Texture* t, uint32_t face)
    {
        VkImageView& view = t->attach[face];
        if (view) return view;
        VkImageViewCreateInfo info{};
        info.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image            = t->image.image;
        info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        info.format           = t->info.format;
        info.subresourceRange = {AspectOf(*t), 0, 1, face, 1};
        if (vkCreateImageView(context_->Device(), &info, nullptr, &view) != VK_SUCCESS) view = VK_NULL_HANDLE;
        return view;
    }

    bool Renderer::EnsureTargetDepth(VkExtent2D extent)
    {
        // D3D9 keeps one depth surface for colour targets, grown to the largest asked for.
        if (target_depth_.image && target_depth_.width >= extent.width && target_depth_.height >= extent.height) return true;
        if (target_depth_.image)
        {
            Dead d{};
            d.frame  = display_->FrameNumber();
            d.image  = target_depth_;
            d.slots[0] = d.slots[1] = resources::Bindless::kNoSlot;
            dead_.push_back(d);
            target_depth_ = resources::Image{};
        }
        const VkFormat format = display_->DepthFormat();
        const uint32_t w = std::max(extent.width, target_depth_.width), h = std::max(extent.height, target_depth_.height);
        if (!allocator_->CreateImage(w, h, format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, target_depth_,
                                     VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)))
            return false;
        target_depth_ready_ = false;
        return true;
    }

    VkCommandBuffer Renderer::Rendering(uint8_t& target_id)
    {
        if (!target_.color)
        {
            if (target_.depth) return VK_NULL_HANDLE; // a depth target alone: not drawn
            const VkCommandBuffer cmd = display_->BeginSceneRendering();
            // The scene's samples are known once it is made (a new format may change them).
            target_id = pipelines_.TargetId(present::Display::kSceneFormat, display_->DepthFormat(), display_->Samples());
            return cmd;
        }
        Texture*       color = target_.color;
        Texture*       depth = target_.depth && target_.depth->info.depth ? target_.depth : nullptr;
        const uint64_t key   = reinterpret_cast<uintptr_t>(color) ^ (static_cast<uint64_t>(target_.face) << 60) ^
                             (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(depth)) * 0x9E3779B97F4A7C15ull);
        const VkFormat depth_format = display_->DepthFormat();
        target_id = pipelines_.TargetId(color->info.format, depth_format);
        if (display_->InRendering() && display_->TargetKey() == key) return display_->Cmd();

        const VkExtent2D extent{color->desc.width, color->desc.height};
        const VkImageView color_view = AttachView(color, target_.face);
        VkImageView       depth_view = VK_NULL_HANDLE;
        if (depth) depth_view = AttachView(depth, 0);
        else if (EnsureTargetDepth(extent)) depth_view = target_depth_.view;
        if (!color_view || !depth_view || !(color->image.image) || color->info.depth) return VK_NULL_HANDLE;

        display_->EndRendering();
        // Every texture is SHADER_READ_ONLY between renderings; one never written gets its first
        // contents (transparent black) in the frame's upload pass.
        if (!color->defined && color->opened != display_->FrameNumber()) OpenInUpload(color);
        if (depth && !depth->defined && depth->opened != display_->FrameNumber()) OpenInUpload(depth);
        const VkCommandBuffer cmd = display_->Cmd();
        ImageBarrier(cmd, color->image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kReadStages | kTransfer, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kColorStage,
                     kColorAccess, 0, 1, color->desc.cube ? target_.face : 0, 1);
        if (depth)
            ImageBarrier(cmd, *depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kReadStages,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kDepthStage, kDepthAccess, 0, 1);
        else if (!target_depth_ready_)
        {
            ImageBarrier(cmd, target_depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(depth_format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0),
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kDepthStage, VK_ACCESS_2_NONE, kDepthStage,
                         kDepthAccess);
            target_depth_ready_ = true;
        }
        else
            MemoryBarrier(cmd, kDepthStage, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kDepthStage, kDepthAccess);

        present::TargetRendering rendering;
        rendering.color        = color_view;
        rendering.depth        = depth_view;
        rendering.depth_format = depth_format;
        rendering.extent       = extent;
        rendering.key          = key;
        const VkCommandBuffer open = display_->BeginTargetRendering(rendering);
        if (open)
        {
            open_.color = color;
            open_.face  = color->desc.cube ? target_.face : 0;
            open_.depth = depth;
            color->used = display_->FrameNumber();
            ++stats_.target_renderings;
        }
        return open;
    }

    void Renderer::Clear(bool color, bool depth, const float rgba[4], float z, VkRect2D rect)
    {
        if (!display_->InFrame() || (!color && !depth)) return;
        if (TargetIsScene())
        {
            display_->Clear(color, depth, rgba, z, rect);
            return;
        }
        uint8_t               id  = 0;
        const VkCommandBuffer cmd = Rendering(id);
        if (!cmd) return;
        const VkExtent2D extent = display_->RenderingExtent();
        const int32_t x0 = std::clamp<int32_t>(rect.offset.x, 0, static_cast<int32_t>(extent.width));
        const int32_t y0 = std::clamp<int32_t>(rect.offset.y, 0, static_cast<int32_t>(extent.height));
        const int32_t x1 = std::clamp<int32_t>(rect.offset.x + static_cast<int32_t>(rect.extent.width), 0, static_cast<int32_t>(extent.width));
        const int32_t y1 = std::clamp<int32_t>(rect.offset.y + static_cast<int32_t>(rect.extent.height), 0, static_cast<int32_t>(extent.height));
        if (x1 <= x0 || y1 <= y0) return;
        VkClearAttachment attachments[2]{};
        uint32_t          count = 0;
        if (color)
        {
            attachments[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            std::memcpy(attachments[count].clearValue.color.float32, rgba, sizeof(float) * 4);
            ++count;
        }
        if (depth)
        {
            attachments[count].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(display_->DepthFormat()) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            attachments[count].clearValue.depthStencil.depth = z;
            ++count;
        }
        VkClearRect clear{};
        clear.rect       = {{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
        clear.layerCount = 1;
        vkCmdClearAttachments(cmd, count, attachments, 1, &clear);
    }

    bool Renderer::CopyScene(Texture* dest, uint32_t level, uint32_t face, VkRect2D from, VkRect2D to, bool linear)
    {
        if (!dest || !display_->InFrame() || level >= dest->image.levels || face >= dest->image.layers || dest->info.depth) return false;
        // The back buffer as D3D9's StretchRect reads it: resolved when multisampled.
        const resources::Image* resolved = display_->ResolveScene();
        if (!resolved) return false;
        const resources::Image& scene = *resolved;
        // Both rectangles kept inside their images.
        const int32_t sw = static_cast<int32_t>(scene.width), sh = static_cast<int32_t>(scene.height);
        const int32_t dw = static_cast<int32_t>(std::max(dest->desc.width >> level, 1u)), dh = static_cast<int32_t>(std::max(dest->desc.height >> level, 1u));
        auto clip = [](VkRect2D r, int32_t w, int32_t h, VkOffset3D& a, VkOffset3D& b) {
            a = {std::clamp(r.offset.x, 0, w), std::clamp(r.offset.y, 0, h), 0};
            b = {std::clamp(r.offset.x + static_cast<int32_t>(r.extent.width), 0, w), std::clamp(r.offset.y + static_cast<int32_t>(r.extent.height), 0, h), 1};
            return b.x > a.x && b.y > a.y;
        };
        VkImageBlit blit{};
        if (!clip(from, sw, sh, blit.srcOffsets[0], blit.srcOffsets[1]) || !clip(to, dw, dh, blit.dstOffsets[0], blit.dstOffsets[1])) return false;
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1};

        if (!dest->defined && dest->opened != display_->FrameNumber()) OpenInUpload(dest);
        const VkCommandBuffer cmd = display_->Cmd();
        ImageBarrier(cmd, scene.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     kColorStage | kTransfer, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
        ImageBarrier(cmd, dest->image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     kReadStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, level, 1, face, 1);
        vkCmdBlitImage(cmd, scene.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        ImageBarrier(cmd, dest->image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kReadStages | kColorStage, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, level,
                     1, face, 1);
        ImageBarrier(cmd, scene.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, kColorStage, kColorAccess);
        ++stats_.copies;
        return true;
    }

    // --- Queries ------------------------------------------------------------------------------------------

    uint32_t Renderer::CreateQuery()
    {
        for (uint32_t i = 0; i < queries_.size(); ++i)
            if (!queries_[i].alive)
            {
                queries_[i]       = Query{};
                queries_[i].alive = true;
                return i + 1;
            }
        Query q;
        q.alive = true;
        queries_.push_back(q);
        return static_cast<uint32_t>(queries_.size());
    }

    void Renderer::DestroyQuery(uint32_t query)
    {
        if (query && query <= queries_.size()) queries_[query - 1].alive = false;
    }

    bool Renderer::BeginQuery(uint32_t query)
    {
        if (!queries_supported_ || !query || query > queries_.size() || !display_->InFrame()) return false;
        Query& q = queries_[query - 1];
        if (q.state == Query::Running || q.state == Query::Pending) return false;
        q.state = Query::Pending;
        return true;
    }

    bool Renderer::EndQuery(uint32_t query)
    {
        if (!query || query > queries_.size()) return false;
        Query& q = queries_[query - 1];
        if (q.state == Query::Pending)
        {
            // Nothing was drawn between its begin and its end: no sample passed.
            q.state   = Query::Idle;
            q.samples = 0;
            return true;
        }
        if (q.state != Query::Running) return false;
        if (display_->InRendering() && display_->RenderingEpoch() == q.epoch) vkCmdEndQuery(display_->Cmd(), query_pool_, q.index);
        q.state = Query::Issued;
        return true;
    }

    void Renderer::EndQueries(VkCommandBuffer cmd)
    {
        for (Query& q : queries_)
            if (q.alive && q.state == Query::Running && q.epoch == display_->RenderingEpoch())
            {
                vkCmdEndQuery(cmd, query_pool_, q.index);
                q.state = Query::Issued;
            }
    }

    bool Renderer::QueryResult(uint32_t query, uint32_t& samples)
    {
        samples = 0;
        if (!query || query > queries_.size()) return false;
        Query& q = queries_[query - 1];
        if (q.state == Query::Pending || q.state == Query::Running) return false;
        if (q.state == Query::Issued)
        {
            if (display_->FramesCompleted() < q.frame) return false;
            uint64_t value = 0;
            if (vkGetQueryPoolResults(context_->Device(), query_pool_, q.index, 1, sizeof value, &value, sizeof value, VK_QUERY_RESULT_64_BIT) ==
                VK_SUCCESS)
                q.samples = static_cast<uint32_t>(std::min<uint64_t>(value, UINT32_MAX));
            q.state = Query::Idle;
        }
        samples = q.samples;
        return true;
    }

    // --- Draws ----------------------------------------------------------------------------------------------

    uint16_t Renderer::LayoutOf(const Packet& packet, uint32_t needed, uint32_t& carried)
    {
        bool same = packet.attribute_count == last_attribute_count_ && packet.stream_count == last_stream_count_ && needed == last_needed_;
        for (uint32_t i = 0; same && i < packet.attribute_count; ++i)
            same = std::memcmp(&packet.attributes[i], &last_attributes_[i], sizeof(Attribute)) == 0;
        for (uint32_t i = 0; same && i < packet.stream_count; ++i) same = packet.streams[i].stride == last_strides_[i];
        if (same)
        {
            carried = last_carried_;
            return last_layout_;
        }

        pipelines::VertexLayout layout;
        bool                    zeros = false;
        carried                       = 0;
        for (uint32_t location = 0; location < shaders::kVertexInputs; ++location)
        {
            if (!(needed & (1u << location))) continue;
            const Attribute* found = nullptr;
            for (uint32_t i = 0; i < packet.attribute_count && !found; ++i)
                if (packet.attributes[i].location == location && packet.attributes[i].stream < packet.stream_count) found = &packet.attributes[i];
            VkVertexInputAttributeDescription& a = layout.attributes[layout.attribute_count++];
            a.location                           = location;
            VkFormat format                      = VK_FORMAT_UNDEFINED;
            if (found)
            {
                switch (found->type)
                {
                case kD3dColor: format = color_rgba_ ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM; break;
                case kUbyte4: format = VK_FORMAT_R8G8B8A8_USCALED; break;
                case kUbyte4N: format = VK_FORMAT_R8G8B8A8_UNORM; break;
                case kFloat2: format = VK_FORMAT_R32G32_SFLOAT; break;
                case kFloat3: format = VK_FORMAT_R32G32B32_SFLOAT; break;
                case kShort2: format = VK_FORMAT_R16G16_SSCALED; break;
                case kFloat1: format = VK_FORMAT_R32_SFLOAT; break;
                default: break;
                }
            }
            if (format != VK_FORMAT_UNDEFINED)
            {
                a.binding = found->stream;
                a.format  = format;
                a.offset  = found->offset;
                carried |= 1u << location;
                if (found->type == kD3dColor && color_rgba_) carried |= 1u << (shaders::kFlagSwizzleShift + location);
            }
            else
            {
                a.binding = kZeroBinding;
                a.format  = VK_FORMAT_R32G32B32A32_SFLOAT;
                a.offset  = 0;
                zeros     = true;
            }
        }
        for (uint32_t i = 0; i < packet.stream_count; ++i)
            layout.bindings[layout.binding_count++] = {i, packet.streams[i].stride, VK_VERTEX_INPUT_RATE_VERTEX};
        if (zeros) layout.bindings[layout.binding_count++] = {kZeroBinding, 0, VK_VERTEX_INPUT_RATE_VERTEX};

        last_layout_          = pipelines_.LayoutId(layout);
        last_carried_         = carried;
        last_needed_          = needed;
        last_attribute_count_ = packet.attribute_count;
        last_stream_count_    = packet.stream_count;
        std::memcpy(last_attributes_, packet.attributes, sizeof(Attribute) * packet.attribute_count);
        for (uint32_t i = 0; i < packet.stream_count; ++i) last_strides_[i] = packet.streams[i].stride;
        return last_layout_;
    }

    bool Renderer::Draw(const Packet& packet)
    {
        if (!Ready() || !display_->InFrame() || !packet.count) return false;
        if (packet.vertex >= pipelines_.Programs(false) || packet.pixel >= pipelines_.Programs(true))
        {
            ++stats_.refused;
            return false;
        }
        const uint64_t              frame  = display_->FrameNumber();
        const shaders::ProgramInfo& vinfo  = pipelines_.Info(false, packet.vertex);
        const shaders::ProgramInfo& pinfo  = pipelines_.Info(true, packet.pixel);
        const bool                  own    = packet.vertex < static_cast<uint16_t>(pipelines::VertexProgram::Count) ||
                          packet.pixel < static_cast<uint16_t>(pipelines::PixelProgram::Count);

        // The buffers: a static or dynamic pool's writes reach the device first.
        VkBuffer     buffers[14] = {};
        VkDeviceSize offsets[14] = {};
        for (uint32_t i = 0; i < packet.stream_count; ++i)
        {
            Pool* pool = packet.streams[i].pool;
            if (!pool || !FlushPool(pool))
            {
                ++stats_.refused;
                return false;
            }
            buffers[i] = pool->usage == kStream ? pool->current.buffer : pool->device.buffer;
            offsets[i] = packet.streams[i].offset;
        }
        if (packet.indexed && (!packet.indices || !FlushPool(packet.indices)))
        {
            ++stats_.refused;
            return false;
        }
        for (Texture* t : packet.textures)
            if (t && !t->defined && t->opened != frame) OpenInUpload(t);

        uint32_t       carried = 0;
        pipelines::Key key;
        key.vertex     = packet.vertex;
        key.pixel      = packet.pixel;
        key.layout     = LayoutOf(packet, vinfo.inputs, carried);
        key.blend      = static_cast<uint8_t>(packet.blend < kBlendModes ? packet.blend : 0);
        key.color_mask = static_cast<uint8_t>(packet.color_mask & 0xF);
        key.topology   = ClassOf(packet.primitive);
        key.wireframe  = packet.wireframe;

        // The draw's data (the renderer's own programs), written once for the draws that share it.
        if (own)
        {
            DrawData data  = packet.data;
            data.inputs[3] = static_cast<int32_t>(carried & 0xFFFF);
            if (!last_address_ || std::memcmp(&data, &last_data_, sizeof data) != 0)
            {
                resources::Ring::Span span;
                if (!data_.Allocate(sizeof data, 16, span))
                {
                    ++stats_.refused;
                    return false;
                }
                std::memcpy(span.data, &data, sizeof data);
                last_data_    = data;
                last_address_ = span.address;
            }
        }
        // The engine's registers, as many rows as the programs read.
        const VkDeviceAddress vregs = Snapshot(vertex_regs_, vinfo.const_rows);
        const VkDeviceAddress pregs = Snapshot(pixel_regs_, pinfo.const_rows);
        if ((vinfo.const_rows && !vregs) || (pinfo.const_rows && !pregs))
        {
            ++stats_.refused;
            return false;
        }

        uint8_t               target_id = 0;
        const VkCommandBuffer cmd       = Rendering(target_id);
        if (!cmd)
        {
            ++stats_.refused;
            return false;
        }
        key.target = target_id;
        // A draw into the scene may be left out while its pipeline compiles (the next frames have
        // it); one into a texture may not: the engine keeps what it drew there (a portrait, the map).
        pipelines::Got   got      = pipelines::Got::Ready;
        const VkPipeline pipeline = pipelines_.Get(key, !TargetIsScene(), &got);
        if (!pipeline)
        {
            ++(got == pipelines::Got::Pending ? stats_.skipped : stats_.refused);
            return false;
        }
        stats_.pipelines = pipelines_.Count();

        for (uint32_t i = 0; i < packet.stream_count; ++i) packet.streams[i].pool->used = frame;
        if (packet.indexed) packet.indices->used = frame;
        for (Texture* t : packet.textures)
            if (t) t->used = frame;

        if (epoch_ != display_->RenderingEpoch())
        {
            // A new rendering (or another recorder in between): everything is set again.
            epoch_    = display_->RenderingEpoch();
            pipeline_ = VK_NULL_HANDLE;
            std::fill(std::begin(vertex_buffers_), std::end(vertex_buffers_), VK_NULL_HANDLE);
            index_buffer_ = VK_NULL_HANDLE;
            std::fill(std::begin(viewport_), std::end(viewport_), -1.0f);
            std::fill(std::begin(scissor_), std::end(scissor_), -1);
            std::fill(std::begin(dynamic_), std::end(dynamic_), 0xFFFFFFFFu);
            const VkDescriptorSet set = bindless_.Set();
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_.Layout(), 0, 1, &set, 0, nullptr);
            vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
            vkCmdSetStencilTestEnable(cmd, VK_FALSE);
            vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                              VK_COMPARE_OP_ALWAYS);
            vkCmdSetDepthBoundsTestEnable(cmd, VK_FALSE);
            vkCmdSetPrimitiveRestartEnable(cmd, VK_FALSE);
            vkCmdSetRasterizerDiscardEnable(cmd, VK_FALSE);
            const VkBuffer     zero   = zeros_.buffer;
            const VkDeviceSize origin = 0;
            vkCmdBindVertexBuffers(cmd, kZeroBinding, 1, &zero, &origin);
        }
        // Occlusion queries the engine began: counted from this draw, inside this rendering.
        for (Query& q : queries_)
        {
            if (!q.alive || q.state != Query::Pending) continue;
            if (query_next_ >= kQueriesPerFrame)
            {
                q.state = Query::Idle;
                continue;
            }
            q.slot  = slot_;
            q.index = slot_ * kQueriesPerFrame + query_next_++;
            q.frame = frame;
            q.epoch = display_->RenderingEpoch();
            vkCmdBeginQuery(cmd, query_pool_, q.index, VK_QUERY_CONTROL_PRECISE_BIT);
            q.state = Query::Running;
            ++stats_.queries;
        }
        if (pipeline != pipeline_)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            pipeline_ = pipeline;
        }
        for (uint32_t i = 0; i < packet.stream_count; ++i)
            if (vertex_buffers_[i] != buffers[i] || vertex_offsets_[i] != offsets[i])
            {
                vkCmdBindVertexBuffers(cmd, i, 1, &buffers[i], &offsets[i]);
                vertex_buffers_[i] = buffers[i];
                vertex_offsets_[i] = offsets[i];
            }
        if (packet.indexed)
        {
            const VkBuffer index = packet.indices->usage == kStream ? packet.indices->current.buffer : packet.indices->device.buffer;
            if (index != index_buffer_)
            {
                vkCmdBindIndexBuffer(cmd, index, 0, VK_INDEX_TYPE_UINT16);
                index_buffer_ = index;
            }
        }

        // D3D9 puts a pixel's centre on its integer coordinates, Vulkan half a pixel further: the
        // viewport moves by half a pixel so that a vertex lands where D3D9 put it among the pixels.
        if (std::memcmp(viewport_, packet.viewport, sizeof viewport_) != 0)
        {
            VkViewport v{};
            v.x        = packet.viewport[0] + 0.5f;
            v.y        = packet.viewport[1] + 0.5f;
            v.width    = std::max(packet.viewport[2], 1.0f);
            v.height   = std::max(packet.viewport[3], 1.0f);
            v.minDepth = std::clamp(packet.viewport[4], 0.0f, 1.0f);
            v.maxDepth = std::clamp(packet.viewport[5], 0.0f, 1.0f);
            vkCmdSetViewport(cmd, 0, 1, &v);
            std::memcpy(viewport_, packet.viewport, sizeof viewport_);
        }
        if (std::memcmp(scissor_, packet.scissor, sizeof scissor_) != 0)
        {
            const VkExtent2D extent = display_->RenderingExtent();
            const int32_t    x0     = std::clamp(packet.scissor[0], 0, static_cast<int32_t>(extent.width));
            const int32_t    y0     = std::clamp(packet.scissor[1], 0, static_cast<int32_t>(extent.height));
            const int32_t    x1     = std::clamp(packet.scissor[0] + packet.scissor[2], x0, static_cast<int32_t>(extent.width));
            const int32_t    y1     = std::clamp(packet.scissor[1] + packet.scissor[3], y0, static_cast<int32_t>(extent.height));
            const VkRect2D   rect{{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
            vkCmdSetScissor(cmd, 0, 1, &rect);
            std::memcpy(scissor_, packet.scissor, sizeof scissor_);
        }
        const uint32_t states[8] = {packet.cull,
                                    static_cast<uint32_t>(TopologyOf(packet.primitive)),
                                    packet.depth_test || packet.depth_write ? 1u : 0u,
                                    packet.depth_write ? 1u : 0u,
                                    static_cast<uint32_t>(packet.depth_test ? packet.depth_compare : VK_COMPARE_OP_ALWAYS),
                                    packet.depth_bias != 0.0f ? 1u : 0u,
                                    Bits(packet.depth_bias),
                                    Bits(packet.blend_factor)};
        if (states[0] != dynamic_[0]) vkCmdSetCullMode(cmd, packet.cull);
        if (states[1] != dynamic_[1]) vkCmdSetPrimitiveTopology(cmd, TopologyOf(packet.primitive));
        if (states[2] != dynamic_[2]) vkCmdSetDepthTestEnable(cmd, states[2]);
        if (states[3] != dynamic_[3]) vkCmdSetDepthWriteEnable(cmd, states[3]);
        if (states[4] != dynamic_[4]) vkCmdSetDepthCompareOp(cmd, static_cast<VkCompareOp>(states[4]));
        if (states[5] != dynamic_[5]) vkCmdSetDepthBiasEnable(cmd, states[5]);
        if (states[6] != dynamic_[6])
        {
            // D3D9's DEPTHBIAS is in depth units; Vulkan's constant factor in the format's smallest
            // step, 2^-24 for the 24-bit depth the scene has (D32 is near enough).
            vkCmdSetDepthBias(cmd, packet.depth_bias * 16777216.0f, 0.0f, 0.0f);
        }
        if (states[7] != dynamic_[7])
        {
            const float constants[4] = {packet.blend_factor, packet.blend_factor, packet.blend_factor, packet.blend_factor};
            vkCmdSetBlendConstants(cmd, constants);
        }
        std::memcpy(dynamic_, states, sizeof states);

        shaders::Push push{};
        push.data[0]   = static_cast<uint32_t>(own ? last_address_ : 0);
        push.data[1]   = static_cast<uint32_t>(own ? last_address_ >> 32 : 0);
        push.vertex[0] = static_cast<uint32_t>(vregs);
        push.vertex[1] = static_cast<uint32_t>(vregs >> 32);
        push.pixel[0]  = static_cast<uint32_t>(pregs);
        push.pixel[1]  = static_cast<uint32_t>(pregs >> 32);
        push.alpha_ref = packet.alpha_ref;
        push.flags     = packet.flags | (carried & ~0xFFFFu);
        if (!pinfo.fixed_fog) push.flags &= ~shaders::kFlagFog;
        std::memcpy(push.clip_plane, packet.clip_plane, sizeof push.clip_plane);
        push.fog_color  = packet.fog_color;
        push.point_size = packet.point_size;
        // The device's own range bounds D3D9's (its MaxPointSize, the caps' point scale maximum).
        const float* limit   = context_->Properties().limits.pointSizeRange;
        const float  ceiling = context_->Chosen().features.largePoints ? limit[1] : 1.0f;
        push.point_range[0]  = std::clamp(packet.point_range[0], limit[0], ceiling);
        push.point_range[1]  = std::clamp(packet.point_range[1], push.point_range[0], ceiling);
        for (uint32_t s = 0; s < kStages; ++s)
        {
            const Texture* t     = packet.textures[s];
            const bool     cube  = (pinfo.cube >> s) & 1;
            uint32_t       image = cube ? black_cube_slot_ : 0;
            // A texture drawn into by this rendering reads as the default (D3D9 leaves it undefined).
            if (t && t != open_.color && t != open_.depth) image = cube ? (t->desc.cube ? t->cube_slot : black_cube_slot_) : t->slot;
            push.stages[s] = shaders::StageWord(image, t ? bindless_.Sampler(packet.samplers[s]) : 0);
        }
        vkCmdPushConstants(cmd, pipelines_.Layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);

        if (packet.indexed) vkCmdDrawIndexed(cmd, packet.count, 1, packet.first_index, 0, 0);
        else vkCmdDraw(cmd, packet.count, 1, 0, 0);
        ++stats_.draws;
        return true;
    }
}
