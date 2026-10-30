// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.render on the device (see Render.hpp).

#include "gx/vk/render/Render.hpp"

#include "gx/vk/Log.hpp"
#include "gx/vk/shaders/Interface.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace wxl::vk::render
{
    namespace spirv
    {
        // The runtime's own wxl.render programs, compiled when it is built (cmake/wxl_vk.cmake).
#include "fullscreen_vert.h"
    }

    namespace
    {
        constexpr VkPipelineStageFlags2 kColorStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        constexpr VkAccessFlags2        kColorAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        constexpr VkPipelineStageFlags2 kDepthStage =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        constexpr VkAccessFlags2 kDepthAccess =
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        constexpr VkPipelineStageFlags2 kReadStages =
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        constexpr VkPipelineStageFlags2 kResolveStage = kColorStage | kDepthStage;
        /// A resolve writes as a colour attachment, a depth one's included.
        constexpr VkAccessFlags2 kResolveAccess = kColorAccess | kDepthAccess;

        void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
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
            barrier.subresourceRange = {aspect, 0, 1, 0, 1};
            VkDependencyInfo dependency{};
            dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers    = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
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

        bool HasStencil(VkFormat format)
        {
            return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D16_UNORM_S8_UINT;
        }

        bool IsDepth(VkFormat format)
        {
            return format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_X8_D24_UNORM_PACK32 || HasStencil(format);
        }

        VkImageAspectFlags AttachAspect(VkFormat format)
        {
            if (!IsDepth(format)) return VK_IMAGE_ASPECT_COLOR_BIT;
            return VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        }

        VkShaderModule Module(VkDevice device, const uint32_t* code, size_t words)
        {
            if (!code || !words) return VK_NULL_HANDLE;
            VkShaderModuleCreateInfo info{};
            info.sType          = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            info.codeSize       = words * sizeof(uint32_t);
            info.pCode          = code;
            VkShaderModule made = VK_NULL_HANDLE;
            return vkCreateShaderModule(device, &info, nullptr, &made) == VK_SUCCESS ? made : VK_NULL_HANDLE;
        }

        VkSampleCountFlagBits SamplesOf(uint32_t count)
        {
            switch (count)
            {
            case 2: return VK_SAMPLE_COUNT_2_BIT;
            case 4: return VK_SAMPLE_COUNT_4_BIT;
            case 8: return VK_SAMPLE_COUNT_8_BIT;
            case 16: return VK_SAMPLE_COUNT_16_BIT;
            default: return VK_SAMPLE_COUNT_1_BIT;
            }
        }

        VkCompareOp CompareOf(uint32_t test)
        {
            switch (test)
            {
            case kDepthLess: return VK_COMPARE_OP_LESS;
            case kDepthLessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case kDepthEqual: return VK_COMPARE_OP_EQUAL;
            case kDepthGreater: return VK_COMPARE_OP_GREATER;
            case kDepthGreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            default: return VK_COMPARE_OP_ALWAYS;
            }
        }

        VkCullModeFlags CullOf(uint32_t cull)
        {
            return cull == kCullBack ? VK_CULL_MODE_BACK_BIT : cull == kCullFront ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
        }

        /// EGxPrim as a Vulkan topology.
        VkPrimitiveTopology EngineTopology(uint32_t primitive)
        {
            switch (primitive)
            {
            case draw::kPoints: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            case draw::kLines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case draw::kLineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case draw::kTriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case draw::kTriangleFan: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
            default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            }
        }

        VkPrimitiveTopology OwnTopology(uint32_t topology)
        {
            switch (topology)
            {
            case kTriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case kLines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case kPoints: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            }
        }

        /// The class a pipeline is made for: its topology at the draw is dynamic within it.
        uint32_t ClassOf(VkPrimitiveTopology topology)
        {
            if (topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST) return 0;
            if (topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP) return 1;
            return 2;
        }

        struct BlendFactors
        {
            bool          enable;
            VkBlendFactor source;
            VkBlendFactor destination;
            VkBlendFactor alpha_source;
            VkBlendFactor alpha_destination;
        };

        BlendFactors BlendOf(uint32_t mode)
        {
            switch (mode)
            {
            case kBlendAlpha:
                return {true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
            case kBlendAdd: return {true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE};
            case kBlendPremultiplied:
                return {true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
            case kBlendMultiply: return {true, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ZERO};
            case kBlendAlphaKeepAlpha:
                return {true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE};
            case kBlendAddKeepAlpha:
                return {true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE};
            case kBlendMultiplyKeepAlpha:
                return {true, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE};
            default: return {false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO};
            }
        }

        using Clock = std::chrono::steady_clock;

        /// A variant's key: its target, and for a batch program the engine's vertex program,
        /// layout and topology class.
        uint64_t VariantKey(uint32_t target, const draw::Batch* batch)
        {
            uint64_t key = target;
            if (batch)
                key |= static_cast<uint64_t>(batch->vertex) << 16 | static_cast<uint64_t>(batch->layout) << 32 |
                       static_cast<uint64_t>(ClassOf(EngineTopology(batch->primitive))) << 48 | 1ull << 63;
            return key;
        }

        uint64_t g_pass_keys = 0; ///< renderings of the passes, told apart from the engine's targets
    }

    uint32_t Render::TargetId(const VkFormat* colors, uint32_t count, VkFormat depth, VkSampleCountFlagBits samples)
    {
        uint32_t words[kMaxColors + 3] = {};
        for (uint32_t i = 0; i < count; ++i) words[i] = static_cast<uint32_t>(colors[i]);
        words[kMaxColors]     = count;
        words[kMaxColors + 1] = static_cast<uint32_t>(depth);
        words[kMaxColors + 2] = static_cast<uint32_t>(samples);
        const std::string key(reinterpret_cast<const char*>(words), sizeof words);
        const auto found = targets_.find(key);
        if (found != targets_.end()) return found->second;
        const uint32_t id = static_cast<uint32_t>(targets_.size());
        targets_.emplace(key, id);
        return id;
    }

    VkFormat FormatOf(uint32_t format)
    {
        switch (format)
        {
        case kRgba8: return VK_FORMAT_R8G8B8A8_UNORM;
        case kBgra8: return VK_FORMAT_B8G8R8A8_UNORM;
        case kR8: return VK_FORMAT_R8_UNORM;
        case kRg8: return VK_FORMAT_R8G8_UNORM;
        case kR16f: return VK_FORMAT_R16_SFLOAT;
        case kRg16f: return VK_FORMAT_R16G16_SFLOAT;
        case kRgba16f: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case kR32f: return VK_FORMAT_R32_SFLOAT;
        case kRgba32f: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case kDepth32f: return VK_FORMAT_D32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
        }
    }

    // --- Life -------------------------------------------------------------------------------------------

    bool Render::Create(device::Context& context, resources::Allocator& allocator, frame::Timeline& timeline, present::Display& display,
                        draw::Renderer& renderer)
    {
        Release();
        context_   = &context;
        allocator_ = &allocator;
        timeline_  = &timeline;
        display_   = &display;
        renderer_  = &renderer;
        fullscreen_ = Module(context.Device(), spirv::fullscreen_vert, sizeof spirv::fullscreen_vert / 4);

        // Timestamps for GPU zones, where the queue has them.
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(context.Physical(), &families, nullptr);
        std::vector<VkQueueFamilyProperties> props(families);
        vkGetPhysicalDeviceQueueFamilyProperties(context.Physical(), &families, props.data());
        if (context.QueueFamily() < families && props[context.QueueFamily()].timestampValidBits > 0)
        {
            VkQueryPoolCreateInfo info{};
            info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = kZonesPerFrame * 2 * frame::Frames::kMax;
            if (vkCreateQueryPool(context.Device(), &info, nullptr, &query_pool_) != VK_SUCCESS) query_pool_ = VK_NULL_HANDLE;
        }
        for (auto& zones : zones_) zones.clear();
        open_zones_.clear();
        seen_frame_ = 0;
        // Compute programs where the queue runs them: the bindless set, 128 bytes of push constants.
        if (context.QueueFamily() < families && (props[context.QueueFamily()].queueFlags & VK_QUEUE_COMPUTE_BIT))
        {
            const VkDescriptorSetLayout set = renderer.Table().Layout();
            const VkPushConstantRange   push{VK_SHADER_STAGE_COMPUTE_BIT, 0, kPushBytes};
            VkPipelineLayoutCreateInfo  info{};
            info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            info.setLayoutCount         = 1;
            info.pSetLayouts            = &set;
            info.pushConstantRangeCount = 1;
            info.pPushConstantRanges    = &push;
            if (vkCreatePipelineLayout(context.Device(), &info, nullptr, &compute_layout_) != VK_SUCCESS) compute_layout_ = VK_NULL_HANDLE;
        }
        // What a mod made on the previous device: its programs again (the variants come at their
        // first draw or dispatch, the images at their first use, the buffers' zeros with the next frame).
        for (Pipeline* pipeline : pipelines_)
        {
            if (pipeline->spec.kind == kCompute)
            {
                pipeline->compute = Module(context.Device(), pipeline->spec.compute.data(), pipeline->spec.compute.size());
                continue;
            }
            pipeline->fragment = Module(context.Device(), pipeline->spec.fragment.data(), pipeline->spec.fragment.size());
            if (pipeline->spec.kind == kGraphics)
                pipeline->vertex = Module(context.Device(), pipeline->spec.vertex.data(), pipeline->spec.vertex.size());
        }
        return fullscreen_ != VK_NULL_HANDLE;
    }

    void Render::Release()
    {
        if (!renderer_) return;
        if (pass_open_) End();
        releasing_ = true;
        for (Dead& d : dead_) d.release();
        dead_.clear();
        writes_.clear();
        for (Image* image : images_) DropImage(image);
        DropImage(&scene_color_);
        DropImage(&scene_depth_);
        // A dropped image's release went to the graveyard: the device is idle or lost, it runs now.
        for (Dead& d : dead_) d.release();
        dead_.clear();
        for (Buffer* buffer : buffers_)
        {
            allocator_->Destroy(buffer->buffer);
            buffer->cleared = false;
        }
        const VkDevice device = context_->Device();
        for (Pipeline* pipeline : pipelines_)
        {
            for (auto& [key, made] : pipeline->variants) vkDestroyPipeline(device, made, nullptr);
            pipeline->variants.clear();
            if (pipeline->vertex) vkDestroyShaderModule(device, pipeline->vertex, nullptr);
            if (pipeline->fragment) vkDestroyShaderModule(device, pipeline->fragment, nullptr);
            if (pipeline->compute) vkDestroyShaderModule(device, pipeline->compute, nullptr);
            pipeline->vertex = pipeline->fragment = pipeline->compute = VK_NULL_HANDLE;
        }
        if (fullscreen_) vkDestroyShaderModule(device, fullscreen_, nullptr);
        fullscreen_ = VK_NULL_HANDLE;
        if (compute_layout_) vkDestroyPipelineLayout(device, compute_layout_, nullptr);
        compute_layout_ = VK_NULL_HANDLE;
        if (query_pool_) vkDestroyQueryPool(device, query_pool_, nullptr);
        query_pool_ = VK_NULL_HANDLE;
        for (auto& zones : zones_) zones.clear();
        open_zones_.clear();
        targets_.clear();
        releasing_ = false;
        renderer_  = nullptr;
    }

    void Render::Destroy()
    {
        Release();
        for (Image* image : images_) delete image;
        for (Buffer* buffer : buffers_) delete buffer;
        for (Pipeline* pipeline : pipelines_) delete pipeline;
        images_.clear();
        buffers_.clear();
        pipelines_.clear();
        stats_ = Stats{};
    }

    void Render::Bury(std::function<void()> release)
    {
        if (!renderer_) return;
        if (releasing_)
        {
            release(); // the device is idle or lost
            return;
        }
        if (display_->InFrame())
        {
            dead_.push_back(Dead{0, display_->FrameNumber(), std::move(release)});
            return;
        }
        const uint64_t serial = timeline_->Last();
        if (timeline_->Completed() >= serial)
        {
            release();
            return;
        }
        dead_.push_back(Dead{serial, 0, std::move(release)});
    }

    void Render::Collect()
    {
        const uint64_t completed = timeline_->Completed();
        const uint64_t frames    = display_->FramesCompleted();
        size_t         kept      = 0;
        for (Dead& d : dead_)
        {
            if (d.serial ? d.serial <= completed : d.frame <= frames) d.release();
            else dead_[kept++] = std::move(d);
        }
        dead_.resize(kept);
    }

    // --- Images ------------------------------------------------------------------------------------------

    VkExtent2D Render::WantedExtent(const Image* image) const
    {
        if (!image->spec.divisor) return {image->spec.width, image->spec.height};
        const VkExtent2D scene = display_->InFrame() && display_->SceneExtent().width ? display_->SceneExtent() : display_->WantedSceneExtent();
        const uint32_t   d     = image->spec.divisor;
        return {(scene.width + d - 1) / d, (scene.height + d - 1) / d};
    }

    Image* Render::CreateImage(const ImageSpec& spec, std::string* why)
    {
        const VkFormat format = FormatOf(spec.format);
        if (format == VK_FORMAT_UNDEFINED)
        {
            if (why) *why = "no such image format";
            return nullptr;
        }
        if (!spec.divisor && (!spec.width || !spec.height || spec.width > 16384 || spec.height > 16384))
        {
            if (why) *why = "an image is 1 to 16384 pixels on each side";
            return nullptr;
        }
        if (spec.samples != 0 && spec.samples != 1 && spec.samples != 2 && spec.samples != 4 && spec.samples != 8)
        {
            if (why) *why = "samples is 0 (the frame's), 1, 2, 4 or 8";
            return nullptr;
        }
        auto* image   = new Image();
        image->spec   = spec;
        image->format = format;
        image->depth  = IsDepth(format);
        images_.push_back(image);
        stats_.images = images_.size();
        if (renderer_ && !spec.divisor) EnsureImage(image); // a fixed size is made now; a screen image when the scene is
        return image;
    }

    void Render::DestroyImage(Image* image)
    {
        auto at = std::find(images_.begin(), images_.end(), image);
        if (at == images_.end()) return;
        images_.erase(at);
        for (size_t i = 0; i < pass_.color_count; ++i)
            if (pass_open_ && pass_.colors[i] == image) End();
        if (pass_open_ && pass_.depth == image) End();
        DropImage(image);
        delete image;
        stats_.images = images_.size();
    }

    void Render::DropImage(Image* image)
    {
        if (!renderer_ || (!image->drawn.image && !image->resolved.image))
        {
            image->ready = false;
            return;
        }
        resources::Image drawn    = image->drawn;
        resources::Image resolved = image->resolved;
        VkImageView      sampled  = image->sampled;
        const uint32_t   slot     = image->slot;
        resources::Allocator* allocator = allocator_;
        resources::Bindless*  table     = &renderer_->Table();
        const VkDevice        device    = context_->Device();
        const bool            own_view  = sampled && sampled != drawn.view && sampled != resolved.view;
        Bury([=]() mutable {
            if (slot != kNoSlot) table->ReleaseImage(slot);
            if (own_view) vkDestroyImageView(device, sampled, nullptr);
            allocator->Destroy(drawn);
            allocator->Destroy(resolved);
        });
        image->drawn    = {};
        image->resolved = {};
        image->sampled  = VK_NULL_HANDLE;
        image->slot     = kNoSlot;
        image->ready    = false;
        image->queued   = false;
        image->extent   = {0, 0};
    }

    bool Render::MakeImage(Image* image, VkExtent2D extent, std::string* why)
    {
        const uint32_t        count   = image->spec.samples ? image->spec.samples : display_->Samples();
        VkSampleCountFlagBits samples = SamplesOf(count);
        const VkPhysicalDeviceLimits& limits = context_->Properties().limits;
        const VkSampleCountFlags allowed = image->depth ? limits.framebufferDepthSampleCounts : limits.framebufferColorSampleCounts;
        if (!(allowed & samples))
        {
            if (why) *why = "the device draws no image of that format with that many samples";
            return false;
        }
        const VkImageAspectFlags attach = AttachAspect(image->format);
        const VkImageUsageFlags  target = image->depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        const VkImageUsageFlags  one    = target | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        bool made = false;
        if (samples == VK_SAMPLE_COUNT_1_BIT)
            made = allocator_->CreateImage(extent.width, extent.height, image->format, one, image->drawn, attach);
        else
            made = allocator_->CreateImage(extent.width, extent.height, image->format, target | VK_IMAGE_USAGE_TRANSFER_DST_BIT, image->drawn,
                                           attach, samples) &&
                   allocator_->CreateImage(extent.width, extent.height, image->format, one, image->resolved, attach);
        if (!made)
        {
            allocator_->Destroy(image->drawn);
            allocator_->Destroy(image->resolved);
            if (why) *why = "the device has no memory for it";
            return false;
        }
        const resources::Image& read = samples == VK_SAMPLE_COUNT_1_BIT ? image->drawn : image->resolved;
        image->sampled               = read.view;
        if (HasStencil(image->format))
        {
            // A program reads the depth alone.
            VkImageViewCreateInfo view{};
            view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image            = read.image;
            view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            view.format           = image->format;
            view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(context_->Device(), &view, nullptr, &image->sampled) != VK_SUCCESS) image->sampled = VK_NULL_HANDLE;
        }
        image->slot    = image->sampled ? renderer_->Table().AddImage(image->sampled) : kNoSlot;
        image->samples = samples;
        image->extent  = extent;
        if (image->slot == kNoSlot)
        {
            DropImage(image);
            if (why) *why = "the bindless table is full";
            return false;
        }
        return true;
    }

    void Render::InitImage(Image* image, VkCommandBuffer cmd)
    {
        // Transparent black (depth 1), then the layout it keeps between passes.
        const VkImageAspectFlags aspect = AttachAspect(image->format);
        auto clear = [&](VkImage target, VkImageLayout final) {
            ImageBarrier(cmd, target, aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
                         VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            const VkImageSubresourceRange range{aspect, 0, 1, 0, 1};
            if (image->depth)
            {
                const VkClearDepthStencilValue value{1.0f, 0};
                vkCmdClearDepthStencilImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
            }
            else
            {
                const VkClearColorValue value{};
                vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
            }
            ImageBarrier(cmd, target, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, final, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, kReadStages | kColorStage | kDepthStage | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | kColorAccess | kDepthAccess);
        };
        const VkImageLayout attach =
            image->depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (image->samples == VK_SAMPLE_COUNT_1_BIT) clear(image->drawn.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        else
        {
            clear(image->drawn.image, attach);
            clear(image->resolved.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        image->ready  = true;
        image->queued = false;
    }

    bool Render::EnsureImage(Image* image)
    {
        if (!renderer_) return false;
        const VkExtent2D            wanted  = WantedExtent(image);
        const VkSampleCountFlagBits samples = SamplesOf(image->spec.samples ? image->spec.samples : display_->Samples());
        if (!wanted.width || !wanted.height) return false;
        const bool made = image->slot != kNoSlot;
        if (made && image->extent.width == wanted.width && image->extent.height == wanted.height && image->samples == samples)
        {
            if (!image->ready && display_->InFrame()) InitImage(image, display_->Upload());
            return true;
        }
        if (made) DropImage(image);
        std::string why;
        if (!MakeImage(image, wanted, &why))
        {
            Log(LogLevel::Warn, "wxl.render: an image of %ux%u could not be made: %s", wanted.width, wanted.height, why.c_str());
            return false;
        }
        if (display_->InFrame()) InitImage(image, display_->Upload());
        else image->queued = true;
        return true;
    }

    uint32_t Render::Slot(Image* image)
    {
        if (!image || !EnsureImage(image)) return kNoSlot;
        return image->slot;
    }

    VkExtent2D Render::Extent(Image* image)
    {
        if (!image) return {0, 0};
        if (renderer_) EnsureImage(image);
        return image->extent.width ? image->extent : WantedExtent(image);
    }

    // --- Buffers -----------------------------------------------------------------------------------------

    Buffer* Render::CreateBuffer(uint32_t size)
    {
        if (!size || size > (256u << 20)) return nullptr;
        auto* buffer = new Buffer();
        buffer->size = (size + 3u) & ~3u;
        buffers_.push_back(buffer);
        stats_.buffers = buffers_.size();
        return buffer;
    }

    void Render::DestroyBuffer(Buffer* buffer)
    {
        auto at = std::find(buffers_.begin(), buffers_.end(), buffer);
        if (at == buffers_.end()) return;
        buffers_.erase(at);
        writes_.erase(std::remove_if(writes_.begin(), writes_.end(), [&](const Write& w) { return w.buffer == buffer; }), writes_.end());
        if (renderer_ && buffer->buffer.buffer)
        {
            resources::Allocator* allocator = allocator_;
            resources::Buffer     dead      = buffer->buffer;
            Bury([=]() mutable { allocator->Destroy(dead); });
        }
        delete buffer;
        stats_.buffers = buffers_.size();
    }

    bool Render::WriteBuffer(Buffer* buffer, uint32_t offset, const void* data, uint32_t size)
    {
        if (!buffer || !data || offset > buffer->size || size > buffer->size - offset) return false;
        if (!size) return true;
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        writes_.push_back(Write{buffer, offset, std::vector<uint8_t>(bytes, bytes + size)});
        stats_.uploaded += size;
        // In a frame the copy goes into its upload pass now; otherwise into the next frame's.
        if (renderer_ && display_->InFrame()) FrameBegun();
        return true;
    }

    uint32_t Render::Sampler(uint32_t flags)
    {
        if (!renderer_) return 0;
        resources::SamplerKey key;
        const bool linear = flags & kSamplerLinear, mips = flags & kSamplerMips;
        key.filter = static_cast<uint8_t>(linear ? (mips ? 4 : 1) : (mips ? 2 : 0));
        key.wrap_u = key.wrap_v = (flags & kSamplerWrap) ? 1 : 0;
        return renderer_->Table().Sampler(key);
    }

    // --- The frame ---------------------------------------------------------------------------------------

    void Render::FrameBegun()
    {
        if (!renderer_ || !display_->InFrame()) return;
        Collect();
        const VkCommandBuffer upload = display_->Upload();
        if (seen_frame_ != display_->FrameNumber())
        {
            // A new frame: its slot's previous frame has completed, so that frame's zones' times are
            // read and the slot's queries reset.
            seen_frame_ = display_->FrameNumber();
            slot_       = display_->Slot();
            ReadZones(slot_);
            if (query_pool_) vkCmdResetQueryPool(upload, query_pool_, slot_ * kZonesPerFrame * 2, kZonesPerFrame * 2);
            zone_next_ = 0;
            open_zones_.clear();
        }
        for (Image* image : images_)
            if (image->queued && image->slot != kNoSlot) InitImage(image, upload);

        bool waited = false;
        for (Write& w : writes_)
        {
            Buffer* buffer = w.buffer;
            if (!buffer->buffer.buffer)
            {
                const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
                if (!allocator_->CreateBuffer(buffer->size, usage, resources::Access::Device, buffer->buffer)) continue;
            }
            if (!waited)
            {
                // The frames before this one read what is written over: the copies wait for them.
                MemoryBarrier(upload, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT);
                waited = true;
            }
            if (!buffer->cleared)
            {
                vkCmdFillBuffer(upload, buffer->buffer.buffer, 0, VK_WHOLE_SIZE, 0);
                MemoryBarrier(upload, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT);
                buffer->cleared = true;
            }
            resources::Ring::Span span;
            if (!renderer_->Stage(w.bytes.size(), span)) continue;
            std::memcpy(span.data, w.bytes.data(), w.bytes.size());
            const VkBufferCopy copy{span.offset, w.offset, w.bytes.size()};
            vkCmdCopyBuffer(upload, span.buffer, buffer->buffer.buffer, 1, &copy);
        }
        // A buffer made without a write is zeros too.
        for (Buffer* buffer : buffers_)
        {
            if (buffer->cleared) continue;
            if (!buffer->buffer.buffer)
            {
                const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
                if (!allocator_->CreateBuffer(buffer->size, usage, resources::Access::Device, buffer->buffer)) continue;
            }
            if (!waited)
            {
                MemoryBarrier(upload, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT);
                waited = true;
            }
            vkCmdFillBuffer(upload, buffer->buffer.buffer, 0, VK_WHOLE_SIZE, 0);
            buffer->cleared = true;
        }
        if (waited)
            MemoryBarrier(upload, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | kReadStages,
                          VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        writes_.clear();
    }

    // --- Pipelines ---------------------------------------------------------------------------------------

    Pipeline* Render::CreatePipeline(const PipelineSpec& spec, std::string& why)
    {
        if (!renderer_)
        {
            why = "no Vulkan device";
            return nullptr;
        }
        if (spec.kind > kCompute)
        {
            why = "no such pipeline kind";
            return nullptr;
        }
        if (spec.kind == kCompute) return CreateCompute(spec, why);
        if (spec.fragment.empty() || (spec.kind == kGraphics) != !spec.vertex.empty())
        {
            why = spec.kind == kGraphics ? "a graphics pipeline takes a vertex and a fragment program"
                                         : "a fullscreen or batch pipeline takes a fragment program and no vertex program";
            return nullptr;
        }
        if (spec.color_count > kMaxColors || (!spec.color_count && !spec.depth_format))
        {
            why = "a pipeline draws into 1 to 4 colour attachments, or a depth one alone";
            return nullptr;
        }
        for (uint32_t i = 0; i < spec.color_count; ++i)
            if (FormatOf(spec.color_formats[i]) == VK_FORMAT_UNDEFINED || spec.color_formats[i] == kDepth32f)
            {
                why = "a colour attachment's format is no colour format";
                return nullptr;
            }
        if (spec.depth_format && spec.depth_format != kDepth32f)
        {
            why = "the depth format is DEPTH32F or 0";
            return nullptr;
        }
        if ((spec.depth_test != kDepthOff || spec.depth_write) && !spec.depth_format)
        {
            why = "a depth test or write needs a depth format";
            return nullptr;
        }
        if (spec.samples != 0 && spec.samples != 1 && spec.samples != 2 && spec.samples != 4 && spec.samples != 8)
        {
            why = "samples is 0 (the frame's), 1, 2, 4 or 8";
            return nullptr;
        }
        auto spirv = [](const std::vector<uint32_t>& words) { return words.empty() || (words.size() >= 5 && words[0] == 0x07230203u); };
        if (!spirv(spec.vertex) || !spirv(spec.fragment))
        {
            why = "a program is no SPIR-V module (its header is missing or cut short)";
            return nullptr;
        }
        auto* pipeline = new Pipeline();
        pipeline->spec = spec;
        const VkDevice device = context_->Device();
        pipeline->fragment    = Module(device, spec.fragment.data(), spec.fragment.size());
        if (spec.kind == kGraphics) pipeline->vertex = Module(device, spec.vertex.data(), spec.vertex.size());
        if (!pipeline->fragment || (spec.kind == kGraphics && !pipeline->vertex))
        {
            why = "the device refused its SPIR-V";
            if (pipeline->fragment) vkDestroyShaderModule(device, pipeline->fragment, nullptr);
            if (pipeline->vertex) vkDestroyShaderModule(device, pipeline->vertex, nullptr);
            delete pipeline;
            return nullptr;
        }
        // Its first variant now, for the target it names at the scene's samples (and with the scene's
        // depth as its depth, when it tests depth): a refusal is the mod's to hear at once.
        if (spec.kind != kBatch)
        {
            VkFormat colors[kMaxColors] = {};
            for (uint32_t i = 0; i < spec.color_count; ++i) colors[i] = FormatOf(spec.color_formats[i]);
            const VkSampleCountFlagBits samples = SamplesOf(spec.samples ? spec.samples : display_->Samples());
            const VkFormat depths[2] = {spec.depth_format ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED, display_->DepthFormat()};
            for (uint32_t d = 0; d < (spec.depth_format && depths[1] != VK_FORMAT_UNDEFINED ? 2u : 1u); ++d)
            {
                const VkPipeline made = Make(*pipeline, nullptr, colors, spec.color_count, depths[d], samples);
                if (!made)
                {
                    why = "the device refused the pipeline (its programs or its state; the log may say more)";
                    vkDestroyShaderModule(device, pipeline->fragment, nullptr);
                    if (pipeline->vertex) vkDestroyShaderModule(device, pipeline->vertex, nullptr);
                    for (auto& [key, v] : pipeline->variants) vkDestroyPipeline(device, v, nullptr);
                    delete pipeline;
                    return nullptr;
                }
                pipeline->variants.emplace(VariantKey(TargetId(colors, spec.color_count, depths[d], samples), nullptr), made);
                ++stats_.variants;
            }
        }
        pipelines_.push_back(pipeline);
        stats_.pipelines = pipelines_.size();
        return pipeline;
    }

    Pipeline* Render::CreateCompute(const PipelineSpec& spec, std::string& why)
    {
        if (!compute_layout_)
        {
            why = "the device's queue runs no compute programs";
            return nullptr;
        }
        if (spec.compute.empty() || !spec.vertex.empty() || !spec.fragment.empty())
        {
            why = "a compute pipeline takes a compute program and no vertex or fragment program";
            return nullptr;
        }
        if (spec.compute.size() < 5 || spec.compute[0] != 0x07230203u)
        {
            why = "a program is no SPIR-V module (its header is missing or cut short)";
            return nullptr;
        }
        auto* pipeline     = new Pipeline();
        pipeline->spec     = spec;
        pipeline->compute  = Module(context_->Device(), spec.compute.data(), spec.compute.size());
        const VkPipeline made = pipeline->compute ? MakeCompute(*pipeline) : VK_NULL_HANDLE;
        if (!made)
        {
            why = pipeline->compute ? "the device refused the pipeline (its program; the log may say more)" : "the device refused its SPIR-V";
            if (pipeline->compute) vkDestroyShaderModule(context_->Device(), pipeline->compute, nullptr);
            delete pipeline;
            return nullptr;
        }
        pipeline->variants.emplace(0, made);
        ++stats_.variants;
        pipelines_.push_back(pipeline);
        stats_.pipelines = pipelines_.size();
        return pipeline;
    }

    VkPipeline Render::MakeCompute(const Pipeline& pipeline) const
    {
        VkComputePipelineCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = pipeline.compute;
        info.stage.pName  = "main";
        info.layout       = compute_layout_;
        VkPipeline made = VK_NULL_HANDLE;
        const VkResult result = vkCreateComputePipelines(context_->Device(), renderer_->PipelineCache().CacheHandle(), 1, &info, nullptr, &made);
        if (result != VK_SUCCESS)
        {
            Log(LogLevel::Warn, "wxl.render: vkCreateComputePipelines (%s): %s", pipeline.spec.name.empty() ? "a mod's pipeline" : pipeline.spec.name.c_str(),
                ResultName(result));
            return VK_NULL_HANDLE;
        }
        return made;
    }

    void Render::DestroyPipeline(Pipeline* pipeline)
    {
        auto at = std::find(pipelines_.begin(), pipelines_.end(), pipeline);
        if (at == pipelines_.end()) return;
        pipelines_.erase(at);
        if (renderer_)
        {
            std::vector<VkPipeline> variants;
            for (auto& [key, made] : pipeline->variants) variants.push_back(made);
            const VkDevice       device   = context_->Device();
            const VkShaderModule vertex   = pipeline->vertex;
            const VkShaderModule fragment = pipeline->fragment;
            const VkShaderModule compute  = pipeline->compute;
            Bury([=]() {
                for (VkPipeline p : variants) vkDestroyPipeline(device, p, nullptr);
                if (vertex) vkDestroyShaderModule(device, vertex, nullptr);
                if (fragment) vkDestroyShaderModule(device, fragment, nullptr);
                if (compute) vkDestroyShaderModule(device, compute, nullptr);
            });
        }
        if (bound_ && pipeline->variants.size())
            for (auto& [key, made] : pipeline->variants)
                if (made == bound_) bound_ = VK_NULL_HANDLE;
        delete pipeline;
        stats_.pipelines = pipelines_.size();
    }

    VkPipeline Render::Make(const Pipeline& pipeline, const draw::Batch* batch, const VkFormat* colors, uint32_t color_count, VkFormat depth,
                            VkSampleCountFlagBits samples) const
    {
        const PipelineSpec& spec = pipeline.spec;
        pipelines::Pipelines& engine = renderer_->PipelineCache();

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = spec.kind == kGraphics ? pipeline.vertex : spec.kind == kFullscreen ? fullscreen_ : engine.ProgramModule(false, batch->vertex);
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = pipeline.fragment;
        stages[1].pName  = "main";
        if (!stages[0].module || !stages[1].module) return VK_NULL_HANDLE;

        VkPipelineVertexInputStateCreateInfo input{};
        input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        if (batch)
        {
            const pipelines::VertexLayout* layout = engine.LayoutOf(batch->layout);
            if (!layout) return VK_NULL_HANDLE;
            input.vertexBindingDescriptionCount   = layout->binding_count;
            input.pVertexBindingDescriptions      = layout->bindings;
            input.vertexAttributeDescriptionCount = layout->attribute_count;
            input.pVertexAttributeDescriptions    = layout->attributes;
        }

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        const VkPrimitiveTopology topology =
            batch ? EngineTopology(batch->primitive) : spec.kind == kFullscreen ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : OwnTopology(spec.topology);
        const uint32_t cls = ClassOf(topology);
        assembly.topology  = cls == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : cls == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.frontFace   = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = samples;

        VkPipelineDepthStencilStateCreateInfo depthState{};
        depthState.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthState.depthTestEnable  = depth != VK_FORMAT_UNDEFINED && (spec.depth_test != kDepthOff || spec.depth_write) ? VK_TRUE : VK_FALSE;
        depthState.depthWriteEnable = depth != VK_FORMAT_UNDEFINED && spec.depth_write ? VK_TRUE : VK_FALSE;
        depthState.depthCompareOp   = CompareOf(spec.depth_test);

        VkPipelineColorBlendAttachmentState attachments[kMaxColors]{};
        for (uint32_t i = 0; i < color_count; ++i)
        {
            BlendFactors blend = BlendOf(spec.blend[i]);
            if (blend.enable)
            {
                VkFormatProperties props{};
                vkGetPhysicalDeviceFormatProperties(context_->Physical(), colors[i], &props);
                if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) blend.enable = false;
            }
            VkPipelineColorBlendAttachmentState& a = attachments[i];
            a.blendEnable         = blend.enable ? VK_TRUE : VK_FALSE;
            a.srcColorBlendFactor = blend.source;
            a.dstColorBlendFactor = blend.destination;
            a.colorBlendOp        = VK_BLEND_OP_ADD;
            a.srcAlphaBlendFactor = blend.alpha_source;
            a.dstAlphaBlendFactor = blend.alpha_destination;
            a.alphaBlendOp        = VK_BLEND_OP_ADD;
            a.colorWriteMask      = 0xF;
        }
        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = color_count;
        colorBlend.pAttachments    = attachments;

        const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
                                           VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 4;
        dynamic.pDynamicStates    = dynamics;

        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount    = color_count;
        rendering.pColorAttachmentFormats = colors;
        rendering.depthAttachmentFormat   = depth;
        rendering.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo info{};
        info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext               = &rendering;
        info.stageCount          = 2;
        info.pStages             = stages;
        info.pVertexInputState   = &input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState      = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState   = &multisample;
        info.pDepthStencilState  = &depthState;
        info.pColorBlendState    = &colorBlend;
        info.pDynamicState       = &dynamic;
        info.layout              = engine.Layout();
        VkPipeline made = VK_NULL_HANDLE;
        const VkResult result = vkCreateGraphicsPipelines(context_->Device(), engine.CacheHandle(), 1, &info, nullptr, &made);
        if (result != VK_SUCCESS)
        {
            Log(LogLevel::Warn, "wxl.render: vkCreateGraphicsPipelines (%s): %s", spec.name.empty() ? "a mod's pipeline" : spec.name.c_str(),
                ResultName(result));
            return VK_NULL_HANDLE;
        }
        return made;
    }

    VkPipeline Render::VariantOf(Pipeline* pipeline, const draw::Batch* batch)
    {
        const PipelineSpec& spec = pipeline->spec;
        // The pass must be the one the pipeline was made for.
        bool fits = spec.color_count == pass_color_count_ && (!spec.depth_format || pass_depth_ != VK_FORMAT_UNDEFINED) &&
                    (spec.samples ? SamplesOf(spec.samples) == pass_samples_ : pass_samples_ == SamplesOf(display_->Samples()));
        for (uint32_t i = 0; fits && i < spec.color_count; ++i) fits = FormatOf(spec.color_formats[i]) == pass_colors_[i];
        if (!fits)
        {
            if (!pipeline->said)
                Log(LogLevel::Warn, "wxl.render: %s does not draw into this pass (its colour formats, depth or samples differ); its draws are refused",
                    spec.name.empty() ? "a mod's pipeline" : spec.name.c_str());
            pipeline->said = true;
            return VK_NULL_HANDLE;
        }
        // A pipeline without depth in a pass with depth leaves the depth untested and unwritten.
        const VkFormat depth  = pass_depth_;
        const uint64_t key    = VariantKey(TargetId(pass_colors_, pass_color_count_, depth, pass_samples_), batch);
        const auto     found  = pipeline->variants.find(key);
        if (found != pipeline->variants.end()) return found->second;
        const auto       begin = Clock::now();
        const VkPipeline made  = Make(*pipeline, batch, pass_colors_, pass_color_count_, depth, pass_samples_);
        const double     ms    = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        ++stats_.stalls;
        stats_.stall_ms += ms;
        if (!made) return VK_NULL_HANDLE;
        pipeline->variants.emplace(key, made);
        ++stats_.variants;
        return made;
    }

    // --- Passes ------------------------------------------------------------------------------------------

    bool Render::Begin(const PassSpec& pass, std::string* why)
    {
        auto refuse = [&](const char* text) {
            if (why) *why = text;
            return false;
        };
        if (!renderer_ || !display_->InFrame()) return refuse("no frame is open");
        if (pass_open_) return refuse("a pass is open already: end it first");
        if (pass.color_count > kMaxColors) return refuse("a pass has at most 4 colour attachments");
        if (!pass.color_count && !pass.depth && !pass.scene_depth) return refuse("a pass has no attachment");
        bool scene = pass.scene_depth;
        for (uint32_t i = 0; i < pass.color_count; ++i)
        {
            if (!pass.scene_color[i] && !pass.colors[i]) return refuse("a colour attachment is no image");
            if (pass.colors[i] && pass.colors[i]->depth) return refuse("a colour attachment is a depth image");
            scene = scene || pass.scene_color[i];
        }
        if (pass.depth && !pass.depth->depth) return refuse("the depth attachment is no depth image");
        // The scene is made for this frame (and the engine's rendering ended) before it is drawn into.
        if (scene ? !display_->PrepareScene() : (display_->EndRendering(), false)) return refuse("the frame has no scene");

        VkExtent2D            extent{0, 0};
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        bool                  first   = true;
        auto take = [&](VkExtent2D e, VkSampleCountFlagBits s) {
            if (first)
            {
                extent  = e;
                samples = s;
                first   = false;
                return true;
            }
            return e.width == extent.width && e.height == extent.height && s == samples;
        };
        const VkSampleCountFlagBits sceneSamples = SamplesOf(display_->Samples());
        for (uint32_t i = 0; i < pass.color_count; ++i)
        {
            if (pass.scene_color[i])
            {
                if (!take(display_->SceneExtent(), sceneSamples)) return refuse("the attachments differ in size or samples");
                continue;
            }
            if (!EnsureImage(pass.colors[i])) return refuse("an image could not be made");
            if (!take(pass.colors[i]->extent, pass.colors[i]->samples)) return refuse("the attachments differ in size or samples");
        }
        if (pass.scene_depth && !take(display_->SceneExtent(), sceneSamples)) return refuse("the attachments differ in size or samples");
        if (pass.depth)
        {
            if (!EnsureImage(pass.depth)) return refuse("an image could not be made");
            if (!take(pass.depth->extent, pass.depth->samples)) return refuse("the attachments differ in size or samples");
        }

        const VkCommandBuffer cmd = display_->Cmd();
        VkRenderingAttachmentInfo colors[kMaxColors]{};
        for (uint32_t i = 0; i < pass.color_count; ++i)
        {
            VkRenderingAttachmentInfo& a = colors[i];
            a.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            a.loadOp      = (pass.clear >> i) & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
            std::memcpy(a.clearValue.color.float32, pass.clear_colors[i], sizeof(float) * 4);
            if (pass.scene_color[i])
            {
                a.imageView      = display_->SceneImage().view;
                pass_colors_[i]  = present::Display::kSceneFormat;
                MemoryBarrier(cmd, kColorStage, kColorAccess, kColorStage, kColorAccess);
                continue;
            }
            Image* image    = pass.colors[i];
            a.imageView     = image->drawn.view;
            pass_colors_[i] = image->format;
            if (image->samples == VK_SAMPLE_COUNT_1_BIT)
                ImageBarrier(cmd, image->drawn.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kReadStages | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kColorStage, kColorAccess);
            else
            {
                MemoryBarrier(cmd, kColorStage, kColorAccess, kColorStage, kColorAccess);
                ImageBarrier(cmd, image->resolved.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kReadStages | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kResolveStage, kColorAccess);
                a.resolveMode        = VK_RESOLVE_MODE_AVERAGE_BIT;
                a.resolveImageView   = image->resolved.view;
                a.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
        }
        VkRenderingAttachmentInfo depth{};
        depth.sType                   = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageLayout             = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp                  = (pass.clear & 0x10u) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp                 = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {pass.clear_depth, 0};
        pass_depth_                   = VK_FORMAT_UNDEFINED;
        if (pass.scene_depth)
        {
            depth.imageView = display_->DepthImage().view;
            pass_depth_     = display_->DepthFormat();
            MemoryBarrier(cmd, kDepthStage, kDepthAccess, kDepthStage, kDepthAccess);
        }
        else if (pass.depth)
        {
            Image* image    = pass.depth;
            depth.imageView = image->drawn.view;
            pass_depth_     = image->format;
            const VkImageAspectFlags aspect = AttachAspect(image->format);
            if (image->samples == VK_SAMPLE_COUNT_1_BIT)
                ImageBarrier(cmd, image->drawn.image, aspect, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kReadStages | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kDepthStage, kDepthAccess);
            else
            {
                MemoryBarrier(cmd, kDepthStage, kDepthAccess, kDepthStage, kDepthAccess);
                ImageBarrier(cmd, image->resolved.image, aspect, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kReadStages | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kResolveStage, kResolveAccess);
                depth.resolveMode        = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                depth.resolveImageView   = image->resolved.view;
                depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            }
        }

        VkRenderingInfo info{};
        info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea           = {{0, 0}, extent};
        info.layerCount           = 1;
        info.colorAttachmentCount = pass.color_count;
        info.pColorAttachments    = colors;
        if (pass_depth_ != VK_FORMAT_UNDEFINED) info.pDepthAttachment = &depth;
        const VkCommandBuffer open = display_->BeginOwnRendering(info, 0x8000000000000000ull | ++g_pass_keys);
        if (!open) return refuse("the pass could not begin");

        pass_             = pass;
        pass_open_        = true;
        pass_extent_      = extent;
        pass_color_count_ = pass.color_count;
        pass_samples_     = samples;
        bound_            = VK_NULL_HANDLE;
        bound_index_      = VK_NULL_HANDLE;
        const VkDescriptorSet set = renderer_->Table().Set();
        vkCmdBindDescriptorSets(open, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer_->PipelineCache().Layout(), 0, 1, &set, 0, nullptr);
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
        const VkRect2D   scissor{{0, 0}, extent};
        vkCmdSetViewport(open, 0, 1, &viewport);
        vkCmdSetScissor(open, 0, 1, &scissor);
        ++stats_.passes;
        return true;
    }

    void Render::End()
    {
        if (!pass_open_) return;
        pass_open_ = false;
        display_->EndRendering();
        const VkCommandBuffer cmd = display_->Cmd();
        bool                  scene = false;
        for (uint32_t i = 0; i < pass_.color_count; ++i)
        {
            if (pass_.scene_color[i])
            {
                scene = true;
                continue;
            }
            Image* image = pass_.colors[i];
            ImageBarrier(cmd, image->samples == VK_SAMPLE_COUNT_1_BIT ? image->drawn.image : image->resolved.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kResolveStage, kColorAccess,
                         kReadStages | VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        }
        if (pass_.depth)
        {
            Image* image = pass_.depth;
            ImageBarrier(cmd, image->samples == VK_SAMPLE_COUNT_1_BIT ? image->drawn.image : image->resolved.image, AttachAspect(image->format),
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kResolveStage, kResolveAccess,
                         kReadStages | VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        }
        if (scene) display_->SceneDrawn();
        if (pass_.scene_depth || scene) MemoryBarrier(cmd, kColorStage | kDepthStage, kColorAccess | kDepthAccess, kColorStage | kDepthStage,
                                                      kColorAccess | kDepthAccess);
        pass_ = PassSpec{};
    }

    // --- Draws -------------------------------------------------------------------------------------------

    bool Render::PreparePush(Pipeline* pipeline, VkPipeline& bound)
    {
        bound = VK_NULL_HANDLE;
        return pass_open_ && pipeline && renderer_;
    }

    void Render::PushOwn(const void* push, uint32_t size)
    {
        uint8_t block[kPushBytes] = {};
        if (push && size) std::memcpy(block, push, std::min(size, kPushBytes));
        vkCmdPushConstants(display_->Cmd(), renderer_->PipelineCache().Layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           kPushBytes, block);
    }

    bool Render::Draw(Pipeline* pipeline, uint32_t vertices, uint32_t instances, const void* push, uint32_t size)
    {
        VkPipeline made = VK_NULL_HANDLE;
        if (!PreparePush(pipeline, made) || pipeline->spec.kind == kBatch || pipeline->spec.kind == kCompute || size > kPushBytes)
            return ++stats_.refused, false;
        made = VariantOf(pipeline, nullptr);
        if (!made) return ++stats_.refused, false;
        const VkCommandBuffer cmd = display_->Cmd();
        if (made != bound_) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_ = made);
        vkCmdSetCullMode(cmd, CullOf(pipeline->spec.cull));
        vkCmdSetPrimitiveTopology(cmd, pipeline->spec.kind == kFullscreen ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : OwnTopology(pipeline->spec.topology));
        PushOwn(push, size);
        if (vertices && instances) vkCmdDraw(cmd, vertices, instances, 0, 0);
        ++stats_.draws;
        return true;
    }

    bool Render::DrawIndexed(Pipeline* pipeline, Buffer* indices, uint32_t first, uint32_t count, uint32_t instances, const void* push,
                             uint32_t size)
    {
        VkPipeline made = VK_NULL_HANDLE;
        if (!PreparePush(pipeline, made) || pipeline->spec.kind != kGraphics || size > kPushBytes || !indices || !indices->buffer.buffer ||
            (static_cast<uint64_t>(first) + count) * 4u > indices->size)
            return ++stats_.refused, false;
        made = VariantOf(pipeline, nullptr);
        if (!made) return ++stats_.refused, false;
        const VkCommandBuffer cmd = display_->Cmd();
        if (made != bound_) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_ = made);
        if (indices->buffer.buffer != bound_index_ || !bound_index32_)
        {
            vkCmdBindIndexBuffer(cmd, bound_index_ = indices->buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            bound_index32_ = true;
        }
        vkCmdSetCullMode(cmd, CullOf(pipeline->spec.cull));
        vkCmdSetPrimitiveTopology(cmd, OwnTopology(pipeline->spec.topology));
        PushOwn(push, size);
        if (count && instances) vkCmdDrawIndexed(cmd, count, instances, first, 0, 0);
        ++stats_.draws;
        return true;
    }

    bool Render::Fullscreen(Pipeline* pipeline, const void* push, uint32_t size)
    {
        if (!pipeline || pipeline->spec.kind != kFullscreen) return ++stats_.refused, false;
        return Draw(pipeline, 3, 1, push, size);
    }

    bool Render::DrawBatch(const draw::Batch& batch, Pipeline* pipeline, const void* constants, uint32_t size)
    {
        VkPipeline made = VK_NULL_HANDLE;
        if (!PreparePush(pipeline, made) || pipeline->spec.kind != kBatch || size > kBatchBytes || batch.frame != display_->FrameNumber() ||
            !batch.count)
            return ++stats_.refused, false;
        made = VariantOf(pipeline, &batch);
        if (!made) return ++stats_.refused, false;
        shaders::Push push = batch.push;
        VkDeviceAddress address = 0;
        if (!renderer_->FrameData(constants, size, address)) return ++stats_.refused, false;
        // A palette the batch read at the pixel registers' place moves to Push::data: the mod's
        // constants take that place, and the mod's program reads no DrawData.
        if (push.flags & shaders::kFlagPaletteAtPixel)
        {
            push.data[0] = push.pixel[0];
            push.data[1] = push.pixel[1];
            push.flags &= ~shaders::kFlagPaletteAtPixel;
        }
        push.pixel[0] = static_cast<uint32_t>(address);
        push.pixel[1] = static_cast<uint32_t>(address >> 32);

        const VkCommandBuffer cmd = display_->Cmd();
        if (made != bound_) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_ = made);
        for (uint32_t i = 0; i < batch.stream_count; ++i)
            if (batch.buffers[i]) vkCmdBindVertexBuffers(cmd, i, 1, &batch.buffers[i], &batch.offsets[i]);
        const VkBuffer     zero   = renderer_->ZeroBuffer();
        const VkDeviceSize origin = 0;
        vkCmdBindVertexBuffers(cmd, draw::Renderer::kZeroBinding, 1, &zero, &origin);
        if (batch.indexed && (batch.index != bound_index_ || batch.index32 != bound_index32_))
        {
            vkCmdBindIndexBuffer(cmd, batch.index, 0, batch.index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
            bound_index_   = batch.index;
            bound_index32_ = batch.index32;
        }
        // Where the client drew it on the scene, scaled to the pass, moved by D3D9's half pixel as
        // the client's draws are.
        const VkExtent2D scene = display_->SceneExtent();
        const float      sx    = scene.width ? static_cast<float>(pass_extent_.width) / static_cast<float>(scene.width) : 1.0f;
        const float      sy    = scene.height ? static_cast<float>(pass_extent_.height) / static_cast<float>(scene.height) : 1.0f;
        VkViewport v{};
        v.x        = batch.viewport[0] * sx + 0.5f;
        v.y        = batch.viewport[1] * sy + 0.5f;
        v.width    = std::max(batch.viewport[2] * sx, 1.0f);
        v.height   = std::max(batch.viewport[3] * sy, 1.0f);
        v.minDepth = std::clamp(batch.viewport[4], 0.0f, 1.0f);
        v.maxDepth = std::clamp(batch.viewport[5], 0.0f, 1.0f);
        vkCmdSetViewport(cmd, 0, 1, &v);
        vkCmdSetCullMode(cmd, pipeline->spec.cull == kCullBatch ? batch.cull : CullOf(pipeline->spec.cull));
        vkCmdSetPrimitiveTopology(cmd, EngineTopology(batch.primitive));
        vkCmdPushConstants(cmd, renderer_->PipelineCache().Layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push,
                           &push);
        if (batch.indexed) vkCmdDrawIndexed(cmd, batch.count, 1, batch.first_index, 0, 0);
        else vkCmdDraw(cmd, batch.count, 1, 0, 0);
        // The pass's own viewport for the draws after.
        const VkViewport whole{0.0f, 0.0f, static_cast<float>(pass_extent_.width), static_cast<float>(pass_extent_.height), 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &whole);
        ++stats_.draws;
        ++stats_.batches;
        return true;
    }

    // --- Compute -----------------------------------------------------------------------------------------

    bool Render::Dispatch(Pipeline* pipeline, uint32_t x, uint32_t y, uint32_t z, const void* push, uint32_t size, std::string* why)
    {
        auto refuse = [&](const char* text) {
            if (why) *why = text;
            ++stats_.refused;
            return false;
        };
        if (!renderer_ || !display_->InFrame()) return refuse("no frame is open");
        if (pass_open_) return refuse("a pass is open: end it first");
        if (!pipeline || pipeline->spec.kind != kCompute) return refuse("the pipeline is no compute pipeline");
        if (size > kPushBytes) return refuse("push constants are 128 bytes at most");
        const uint32_t* most = context_->Properties().limits.maxComputeWorkGroupCount;
        if (x > most[0] || y > most[1] || z > most[2]) return refuse("more workgroups than the device dispatches at once");
        const auto found = pipeline->variants.find(0);
        VkPipeline made  = found != pipeline->variants.end() ? found->second : VK_NULL_HANDLE;
        if (!made && pipeline->compute)
        {
            // The pipeline of a device made again since: compiled now, once.
            const auto   begin = Clock::now();
            made               = MakeCompute(*pipeline);
            ++stats_.stalls;
            stats_.stall_ms += std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            if (made)
            {
                pipeline->variants.emplace(0, made);
                ++stats_.variants;
            }
        }
        if (!made) return refuse("the pipeline could not be made on this device");

        // The engine's rendering ends; what the frame drew and wrote before is seen by the program,
        // and what the program writes by everything after it.
        display_->EndRendering();
        const VkCommandBuffer cmd = display_->Cmd();
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, made);
        const VkDescriptorSet set = renderer_->Table().Set();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout_, 0, 1, &set, 0, nullptr);
        uint8_t block[kPushBytes] = {};
        if (push && size) std::memcpy(block, push, size);
        vkCmdPushConstants(cmd, compute_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, kPushBytes, block);
        if (x && y && z) vkCmdDispatch(cmd, x, y, z);
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        ++stats_.dispatches;
        return true;
    }

    bool Render::BonePalette(const float* rows, uint32_t count, VkDeviceAddress& address)
    {
        address = 0;
        if (!renderer_ || !rows || !count || count > kPaletteBytes / 48u) return false;
        if (!renderer_->FrameData(rows, count * 48u, address)) return false;
        ++stats_.palettes;
        return true;
    }

    // --- The scene, for sampling ------------------------------------------------------------------------

    uint32_t Render::SceneColorSlot(uint64_t epoch)
    {
        if (!renderer_ || !display_->InFrame() || pass_open_) return kNoSlot;
        if (color_epoch_ == epoch && scene_color_.ready) return scene_color_.slot;
        const resources::Image* source = display_->ResolveScene();
        if (!source) return kNoSlot;
        scene_color_.spec   = ImageSpec{0, 0, 1, kBgra8, 1};
        scene_color_.format = present::Display::kSceneFormat;
        if (!EnsureImage(&scene_color_)) return kNoSlot;
        const VkCommandBuffer cmd = display_->Cmd();
        ImageBarrier(cmd, source->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     kColorStage | VK_PIPELINE_STAGE_2_RESOLVE_BIT, kColorAccess | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
        ImageBarrier(cmd, scene_color_.drawn.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kReadStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent         = {std::min(source->width, scene_color_.extent.width), std::min(source->height, scene_color_.extent.height), 1};
        vkCmdCopyImage(cmd, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene_color_.drawn.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region);
        ImageBarrier(cmd, source->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, kColorStage | VK_PIPELINE_STAGE_2_RESOLVE_BIT,
                     kColorAccess | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        ImageBarrier(cmd, scene_color_.drawn.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kReadStages,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        color_epoch_ = epoch;
        ++stats_.scene_copies;
        return scene_color_.slot;
    }

    uint32_t Render::SceneDepthSlot(uint64_t epoch)
    {
        if (!renderer_ || !display_->InFrame() || pass_open_) return kNoSlot;
        if (depth_epoch_ == epoch && scene_depth_.ready) return scene_depth_.slot;
        if (!display_->PrepareScene()) return kNoSlot;
        const uint32_t slot = CopySceneDepth();
        if (slot != kNoSlot)
        {
            depth_epoch_ = epoch;
            ++stats_.scene_copies;
        }
        return slot;
    }

    uint32_t Render::CopySceneDepth()
    {
        const VkFormat format = display_->DepthFormat();
        scene_depth_.spec     = ImageSpec{0, 0, 1, kDepth32f, 1};
        if (scene_depth_.format != format)
        {
            DropImage(&scene_depth_);
            scene_depth_.format = format;
            scene_depth_.depth  = true;
        }
        if (!EnsureImage(&scene_depth_)) return kNoSlot;
        const VkCommandBuffer    cmd    = display_->Cmd();
        const resources::Image&  source = display_->DepthImage();
        const VkImageAspectFlags aspect = AttachAspect(format);
        if (display_->Samples() <= 1)
        {
            ImageBarrier(cmd, source.image, aspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         kDepthStage, kDepthAccess, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            ImageBarrier(cmd, scene_depth_.drawn.image, aspect, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         kReadStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            region.extent         = {std::min(source.width, scene_depth_.extent.width), std::min(source.height, scene_depth_.extent.height), 1};
            vkCmdCopyImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene_depth_.drawn.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);
            ImageBarrier(cmd, source.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, kDepthStage, kDepthAccess);
            ImageBarrier(cmd, scene_depth_.drawn.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kReadStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            return scene_depth_.slot;
        }
        // Multisampled: an empty rendering of the scene's depth resolves its first sample into the copy.
        ImageBarrier(cmd, scene_depth_.drawn.image, aspect, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kReadStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kResolveStage,
                     kResolveAccess);
        MemoryBarrier(cmd, kDepthStage, kDepthAccess, kDepthStage, kDepthAccess);
        VkRenderingAttachmentInfo depth{};
        depth.sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView          = source.view;
        depth.imageLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp             = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp            = VK_ATTACHMENT_STORE_OP_STORE;
        depth.resolveMode        = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        depth.resolveImageView   = scene_depth_.drawn.view;
        depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkRenderingInfo info{};
        info.sType            = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea       = {{0, 0}, scene_depth_.extent};
        info.layerCount       = 1;
        info.pDepthAttachment = &depth;
        if (!display_->BeginOwnRendering(info, 0x8000000000000000ull | ++g_pass_keys)) return kNoSlot;
        display_->EndRendering();
        ImageBarrier(cmd, scene_depth_.drawn.image, aspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kResolveStage, kResolveAccess, kReadStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        return scene_depth_.slot;
    }

    // --- GPU zones ---------------------------------------------------------------------------------------

    bool Render::ZoneBegin(const char* name)
    {
        if (!sink_ || !query_pool_ || !renderer_ || !display_->InFrame() || zone_next_ + 2 > kZonesPerFrame * 2) return false;
        if (seen_frame_ != display_->FrameNumber()) FrameBegun(); // a frame not seen begin yet
        Zone zone;
        zone.name  = name ? name : "";
        zone.begin = zone_next_++;
        zone.end   = zone_next_++;
        vkCmdWriteTimestamp2(display_->Cmd(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, query_pool_, slot_ * kZonesPerFrame * 2 + zone.begin);
        zones_[slot_].push_back(std::move(zone));
        open_zones_.push_back(zones_[slot_].size() - 1);
        return true;
    }

    void Render::ZoneEnd()
    {
        if (open_zones_.empty() || !query_pool_ || !display_->InFrame()) return;
        Zone& zone = zones_[slot_][open_zones_.back()];
        open_zones_.pop_back();
        vkCmdWriteTimestamp2(display_->Cmd(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, query_pool_, slot_ * kZonesPerFrame * 2 + zone.end);
        zone.closed = true;
    }

    void Render::CloseZones()
    {
        while (!open_zones_.empty()) ZoneEnd();
        open_zones_.clear();
    }

    void Render::ReadZones(uint32_t slot)
    {
        std::vector<Zone>& zones = zones_[slot % frame::Frames::kMax];
        if (query_pool_ && sink_)
            for (const Zone& zone : zones)
            {
                if (!zone.closed) continue;
                uint64_t times[2] = {};
                const uint32_t base = (slot % frame::Frames::kMax) * kZonesPerFrame * 2;
                if (vkGetQueryPoolResults(context_->Device(), query_pool_, base + zone.begin, 1, sizeof times[0], &times[0], sizeof times[0],
                                          VK_QUERY_RESULT_64_BIT) != VK_SUCCESS ||
                    vkGetQueryPoolResults(context_->Device(), query_pool_, base + zone.end, 1, sizeof times[1], &times[1], sizeof times[1],
                                          VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
                    continue;
                sink_->Zone(zone.name.c_str(), static_cast<int64_t>(times[0]), static_cast<int64_t>(times[1]));
            }
        zones.clear();
    }
}
