// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A frame: the scene, the present pass, the submission and the present.

#include "gx/vk/present/Display.hpp"

#include "gx/vk/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace wxl::vk::present
{
    namespace
    {
        void Barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                     VkAccessFlags2 dstAccess)
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

        constexpr VkPipelineStageFlags2 kColorStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        constexpr VkAccessFlags2        kColorWrite = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        constexpr VkPipelineStageFlags2 kDepthStage =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        constexpr VkAccessFlags2 kDepthWrite =
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

        /// The scene's depth: drawn into, and copied from (wxl.render's copy of it for sampling).
        constexpr VkImageUsageFlags kDepthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        bool HasStencil(VkFormat format)
        {
            return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D16_UNORM_S8_UINT;
        }

        VkImageAspectFlags DepthAspect(VkFormat format)
        {
            return VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        }

        uint64_t NowNs()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }
    }

    const char* FrameEndName(FrameEnd end)
    {
        switch (end)
        {
        case FrameEnd::Presented: return "presented";
        case FrameEnd::Drawn: return "drawn";
        case FrameEnd::Dropped: return "dropped";
        default: return "device lost";
        }
    }

    bool Display::Create(device::Context& context, resources::Allocator& allocator, frame::Timeline& timeline,
                         const Options& options, char* why, uint32_t why_size)
    {
        Destroy();
        context_   = &context;
        allocator_ = &allocator;
        timeline_  = &timeline;
        if (!frames_.Create(timeline, options.frames, why, why_size)) return false;
        if (!pass_.Create(context, allocator, frames_.Count(), why, why_size))
        {
            frames_.Destroy();
            return false;
        }
        pass_.SetFilter(options.filter);

        // The scene's depth: the first of D24S8, D32S8 and D32 the device draws depth into.
        static const VkFormat kDepth[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT};
        depth_format_ = VK_FORMAT_UNDEFINED;
        for (VkFormat f : kDepth)
        {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(context.Physical(), f, &props);
            if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            {
                depth_format_ = f;
                break;
            }
        }
        VkFormatProperties sceneProps{};
        vkGetPhysicalDeviceFormatProperties(context.Physical(), kSceneFormat, &sceneProps);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if (depth_format_ == VK_FORMAT_UNDEFINED || (sceneProps.optimalTilingFeatures & need) != need)
        {
            std::snprintf(why, why_size, "the device cannot draw the scene (B8G8R8A8 colour and a depth format)");
            Destroy();
            return false;
        }
        stats_ = Stats{};
        return true;
    }

    void Display::Destroy()
    {
        if (!context_) return;
        if (context_->Ready())
        {
            if (frames_.Open()) frames_.Abandon();
            submitted_.clear();
            const frame::Wait wait = timeline_->Last() ? timeline_->WaitFor(timeline_->Last()) : frame::Wait::Done;
            if (wait != frame::Wait::Done && !context_->Lost())
                context_->MarkLost("the last frame did not complete before the display was released");
            for (auto& release : after_) graveyard_.Bury(0, std::move(release));
            after_.clear();
            graveyard_.Flush();
            swapchain_.Destroy();
            if (surface_) vkDestroySurfaceKHR(context_->Instance(), surface_, nullptr);
            allocator_->Destroy(scene_);
            allocator_->Destroy(resolve_);
            allocator_->Destroy(depth_);
            allocator_->Destroy(target_);
            allocator_->Destroy(readback_);
            allocator_->Destroy(pixels_);
            if (queries_) vkDestroyQueryPool(context_->Device(), queries_, nullptr);
            pass_.Destroy();
            frames_.Destroy();
        }
        surface_      = VK_NULL_HANDLE;
        make_surface_ = nullptr;
        queries_      = VK_NULL_HANDLE;
        sink_         = nullptr;
        offscreen_    = false;
        scene_extent_ = {0, 0};
        samples_      = VK_SAMPLE_COUNT_1_BIT;
        msaa_dirty_   = false;
        touched_      = false;
        rendering_    = Rendering::None;
        context_      = nullptr;
    }

    // --- Where frames go ---------------------------------------------------------------------------

    void Display::DropSurface()
    {
        const uint64_t serial = timeline_->Last();
        swapchain_.Retire(graveyard_, serial);
        if (surface_)
        {
            const VkInstance   instance = context_->Instance();
            const VkSurfaceKHR surface  = surface_;
            graveyard_.Bury(serial, [instance, surface] { vkDestroySurfaceKHR(instance, surface, nullptr); });
        }
        surface_ = VK_NULL_HANDLE;
    }

    bool Display::SetSurface(SurfaceMaker make, void* user, char* why, uint32_t why_size)
    {
        DropSurface();
        make_surface_ = make;
        surface_user_ = user;
        rebuild_      = true;
        if (!make) return true;
        surface_ = make(context_->Instance(), user, why, why_size);
        return surface_ != VK_NULL_HANDLE;
    }

    bool Display::SetOffscreen(VkExtent2D extent)
    {
        allocator_->Destroy(target_);
        allocator_->Destroy(readback_);
        offscreen_ = extent.width && extent.height;
        if (!offscreen_) return true;
        window_ = extent;
        return allocator_->CreateImage(extent.width, extent.height, kSceneFormat,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, target_) &&
               allocator_->CreateBuffer(static_cast<VkDeviceSize>(extent.width) * extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        resources::Access::Readback, readback_);
    }

    void Display::SetWindowExtent(VkExtent2D extent)
    {
        if (extent.width == window_.width && extent.height == window_.height) return;
        window_  = extent;
        rebuild_ = true;
    }

    void Display::SetSceneExtent(VkExtent2D extent) { scene_wanted_ = extent; }

    void Display::SetSamples(uint32_t count) { samples_wanted_ = count ? count : 1; }

    uint32_t Display::SampleCounts() const
    {
        if (!context_ || !context_->Ready()) return VK_SAMPLE_COUNT_1_BIT;
        const VkPhysicalDeviceLimits& limits = context_->Properties().limits;
        return (limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts) | VK_SAMPLE_COUNT_1_BIT;
    }

    void Display::SetSync(Sync sync)
    {
        if (sync == sync_) return;
        sync_    = sync;
        rebuild_ = true;
    }

    bool Display::SetTimestamps(TimestampSink* sink)
    {
        sink_ = nullptr;
        if (!sink) return true;
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(context_->Physical(), &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(context_->Physical(), &familyCount, families.data());
        if (context_->QueueFamily() >= familyCount || families[context_->QueueFamily()].timestampValidBits == 0) return false;
        if (!queries_)
        {
            VkQueryPoolCreateInfo info{};
            info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = 2 * frame::Frames::kMax + 1; // two per slot, and one for SampleGpuTime
            if (vkCreateQueryPool(context_->Device(), &info, nullptr, &queries_) != VK_SUCCESS)
            {
                queries_ = VK_NULL_HANDLE;
                return false;
            }
            std::fill(std::begin(written_), std::end(written_), false);
        }
        sink_ = sink;
        return true;
    }

    bool Display::SampleGpuTime(int64_t& ticks)
    {
        if (!queries_ || context_->Lost() || frames_.Open()) return false;
        // A command buffer of its own: the frames' slots stay as they are.
        const VkDevice          dev = context_->Device();
        VkCommandPool           pool = VK_NULL_HANDLE;
        VkCommandBuffer         cmd  = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = context_->QueueFamily();
        if (vkCreateCommandPool(dev, &poolInfo, nullptr, &pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool        = pool;
        allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        bool ok                     = vkAllocateCommandBuffers(dev, &allocate, &cmd) == VK_SUCCESS;
        // The query after the slots' pairs is this one's.
        const uint32_t query = 2 * frame::Frames::kMax;
        if (ok)
        {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &begin);
            vkCmdResetQueryPool(cmd, queries_, query, 1);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries_, query);
            vkEndCommandBuffer(cmd);
            const uint64_t serial = timeline_->Submit(&cmd, 1);
            ok = serial && timeline_->WaitFor(serial) == frame::Wait::Done;
            uint64_t value = 0;
            ok = ok && vkGetQueryPoolResults(dev, queries_, query, 1, sizeof value, &value, sizeof value, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
            ticks = static_cast<int64_t>(value);
        }
        vkDestroyCommandPool(dev, pool, nullptr);
        return ok;
    }

    // --- A frame -------------------------------------------------------------------------------------

    bool Display::BeginFrame(frame::Wait& wait)
    {
        if (!frames_.Ready())
        {
            wait = frame::Wait::Failed;
            return false;
        }
        const VkCommandBuffer cmd = frames_.Begin(wait);
        if (!cmd)
        {
            if (wait == frame::Wait::Timeout)
                context_->MarkLost("a frame did not complete within the bound (the GPU is hung)");
            return false;
        }
        graveyard_.Collect(timeline_->Completed());
        touched_   = false;
        rendering_ = Rendering::None;
        ++stats_.frames;
        ++epoch_;
        if (hook_) hook_->FrameBegun(frames_.Index(), frames_.Upload());

        if (sink_ && queries_)
        {
            const uint32_t slot = frames_.Index();
            if (written_[slot])
            {
                uint64_t times[2] = {};
                // The slot's previous frame is complete: its queries are available without a wait.
                if (vkGetQueryPoolResults(context_->Device(), queries_, slot * 2, 2, sizeof times, times, sizeof times[0],
                                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                {
                    sink_->Time(static_cast<uint16_t>(slot * 2), static_cast<int64_t>(times[0]));
                    sink_->Time(static_cast<uint16_t>(slot * 2 + 1), static_cast<int64_t>(times[1]));
                }
                written_[slot] = false;
            }
            vkCmdResetQueryPool(cmd, queries_, slot * 2, 2);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries_, slot * 2);
            sink_->Begin(static_cast<uint16_t>(slot * 2));
            written_[slot] = true;
        }
        return true;
    }

    bool Display::EnsureScene()
    {
        const VkExtent2D want = scene_wanted_.width && scene_wanted_.height ? scene_wanted_ : window_;
        if (!want.width || !want.height) return scene_.image != VK_NULL_HANDLE;
        // The most samples the device draws both colour and depth with, at most what the engine asks.
        const uint32_t        counts  = SampleCounts();
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        for (uint32_t n = 64; n > 1; n >>= 1)
            if (n <= samples_wanted_ && (counts & n))
            {
                samples = static_cast<VkSampleCountFlagBits>(n);
                break;
            }
        if (scene_.image && want.width == scene_extent_.width && want.height == scene_extent_.height && samples == samples_) return true;

        EndRendering();
        // The old images may be named by this frame already: they go when it completes.
        if (scene_.image || depth_.image || resolve_.image)
        {
            resources::Image     oldScene = scene_, oldDepth = depth_, oldResolve = resolve_;
            resources::Allocator* allocator = allocator_;
            AfterFrame([allocator, oldScene, oldDepth, oldResolve]() mutable {
                allocator->Destroy(oldScene);
                allocator->Destroy(oldDepth);
                allocator->Destroy(oldResolve);
            });
            scene_   = resources::Image{};
            depth_   = resources::Image{};
            resolve_ = resources::Image{};
        }
        // Multisampled, the scene is drawn into and resolved into a one-sample image, which is shown,
        // copied from and read back; otherwise the scene is all of these.
        const bool              multi = samples > VK_SAMPLE_COUNT_1_BIT;
        const VkImageUsageFlags shown = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        // A draw with its samples on the pixel's centre (GxRs_Multisample 0) needs a depth made for
        // sample locations other than the standard ones.
        centre_samples_ = false;
        if (multi && (context_->Chosen().centre_samples & samples))
        {
            VkImageFormatProperties properties{};
            centre_samples_ = vkGetPhysicalDeviceImageFormatProperties(context_->Physical(), depth_format_, VK_IMAGE_TYPE_2D,
                                                                       VK_IMAGE_TILING_OPTIMAL, kDepthUsage,
                                                                       VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT,
                                                                       &properties) == VK_SUCCESS &&
                              (properties.sampleCounts & samples);
        }
        bool made = allocator_->CreateImage(want.width, want.height, kSceneFormat,
                                            multi ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT : shown, scene_,
                                            VK_IMAGE_ASPECT_COLOR_BIT, samples) &&
                    allocator_->CreateImage(want.width, want.height, depth_format_, kDepthUsage, depth_,
                                            DepthAspect(depth_format_), samples,
                                            centre_samples_ ? VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT : 0);
        if (made && multi) made = allocator_->CreateImage(want.width, want.height, kSceneFormat, shown, resolve_);
        if (!made)
        {
            allocator_->Destroy(scene_);
            allocator_->Destroy(depth_);
            allocator_->Destroy(resolve_);
            scene_extent_   = {0, 0};
            samples_        = VK_SAMPLE_COUNT_1_BIT;
            centre_samples_ = false;
            return false;
        }
        scene_extent_ = want;
        samples_      = samples;
        msaa_dirty_   = false;
        touched_      = false;
        ++stats_.scenes;
        Log(LogLevel::Debug, "scene %ux%u, %u sample(s) per pixel", want.width, want.height, static_cast<unsigned>(samples));
        return true;
    }

    void Display::Touch()
    {
        if (touched_ || !scene_.image) return;
        const VkCommandBuffer cmd = frames_.Current();
        // The back buffer's contents do not outlive a frame (D3D9's discard swap effect): undefined.
        Barrier(cmd, scene_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE, kColorStage, kColorWrite);
        Barrier(cmd, depth_.image, DepthAspect(depth_format_), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                kDepthStage, VK_ACCESS_2_NONE, kDepthStage, kDepthWrite);
        if (resolve_.image)
            Barrier(cmd, resolve_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE, kColorStage, kColorWrite);
        msaa_dirty_ = false;
        touched_    = true;
    }

    void Display::Begin(Rendering kind)
    {
        if (rendering_ == kind) return;
        EndRendering();
        // Both kinds draw into the scene, multisampled or not, as D3D9 draws an overlay (Dear
        // ImGui) into its back buffer: what is drawn after it (the software cursor) lies over it,
        // and the resolve for the image shown or a readback carries both.
        if (samples_ > VK_SAMPLE_COUNT_1_BIT) msaa_dirty_ = true;
        VkRenderingAttachmentInfo color{};
        color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView   = scene_.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo depth{};
        depth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView   = depth_.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo info{};
        info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea           = {{0, 0}, scene_extent_};
        info.layerCount           = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments    = &color;
        if (kind == Rendering::Full)
        {
            info.pDepthAttachment = &depth;
            if (HasStencil(depth_format_)) info.pStencilAttachment = &depth;
        }
        vkCmdBeginRendering(frames_.Current(), &info);
        rendering_ = kind;
        ++epoch_;
    }

    void Display::EndRendering()
    {
        if (rendering_ == Rendering::None) return;
        const VkCommandBuffer cmd = frames_.Current();
        if (hook_) hook_->RenderingEnding(cmd);
        vkCmdEndRendering(cmd);
        rendering_  = Rendering::None;
        target_key_ = 0;
        if (hook_) hook_->RenderingEnded(cmd);
    }

    VkCommandBuffer Display::BeginTargetRendering(const TargetRendering& target)
    {
        if (!frames_.Open() || !target.color || !target.extent.width || !target.extent.height) return VK_NULL_HANDLE;
        if (rendering_ == Rendering::Target && target_key_ == target.key) return frames_.Current();
        EndRendering();
        VkRenderingAttachmentInfo color{};
        color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView   = target.color;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo depth{};
        depth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView   = target.depth;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo info{};
        info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea           = {{0, 0}, target.extent};
        info.layerCount           = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments    = &color;
        if (target.depth)
        {
            info.pDepthAttachment = &depth;
            if (HasStencil(target.depth_format)) info.pStencilAttachment = &depth;
        }
        vkCmdBeginRendering(frames_.Current(), &info);
        rendering_     = Rendering::Target;
        target_key_    = target.key;
        target_extent_ = target.extent;
        ++epoch_;
        return frames_.Current();
    }

    VkCommandBuffer Display::BeginOwnRendering(const VkRenderingInfo& info, uint64_t key)
    {
        if (!frames_.Open() || !key) return VK_NULL_HANDLE;
        EndRendering();
        vkCmdBeginRendering(frames_.Current(), &info);
        rendering_     = Rendering::Target;
        target_key_    = key;
        target_extent_ = info.renderArea.extent;
        ++epoch_;
        return frames_.Current();
    }

    bool Display::PrepareScene()
    {
        if (!frames_.Open() || !EnsureScene()) return false;
        EndRendering();
        Touch();
        return true;
    }

    const resources::Image* Display::ResolveScene()
    {
        if (!PrepareScene()) return nullptr;
        if (samples_ == VK_SAMPLE_COUNT_1_BIT) return &scene_;
        if (!msaa_dirty_) return &resolve_;
        // D3D9's resolve of a multisampled back buffer: each pixel the mean of its samples.
        const VkCommandBuffer cmd = frames_.Current();
        Barrier(cmd, scene_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                kColorStage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_RESOLVE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        Barrier(cmd, resolve_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                kColorStage | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_RESOLVE_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageResolve region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent         = {scene_extent_.width, scene_extent_.height, 1};
        vkCmdResolveImage(cmd, scene_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resolve_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                          &region);
        Barrier(cmd, resolve_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_RESOLVE_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kColorStage | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                kColorWrite | VK_ACCESS_2_TRANSFER_READ_BIT);
        Barrier(cmd, scene_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_RESOLVE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, kColorStage, kColorWrite);
        msaa_dirty_ = false;
        ++stats_.resolves;
        return &resolve_;
    }

    bool Display::Flush()
    {
        if (!frames_.Open()) return false;
        EndRendering();
        if (hook_) hook_->FrameFlushing(frames_.Upload());
        const uint64_t serial = frames_.Submit();
        frame::Wait    wait   = frame::Wait::Failed;
        if (!serial || !frames_.Resume(wait))
        {
            if (wait == frame::Wait::Timeout) context_->MarkLost("a readback's submission did not complete within the bound");
            // The frame cannot go on: what it named goes once the last submission completes.
            submitted_.push_back(Submitted{stats_.frames, timeline_->Last()});
            for (auto& release : after_) graveyard_.Bury(timeline_->Last(), std::move(release));
            after_.clear();
            return false;
        }
        last_serial_ = serial;
        graveyard_.Collect(timeline_->Completed());
        ++stats_.flushes;
        // A new command buffer: whatever a recorder bound is to be bound again.
        ++epoch_;
        return true;
    }

    bool Display::ReadScene(VkRect2D rect, uint32_t* bgra)
    {
        if (!bgra || !rect.extent.width || !rect.extent.height) return false;
        const resources::Image* image = ResolveScene();
        if (!image) return false;
        // Inside the scene only: what lies outside reads as nothing written (zero).
        const int32_t x0 = std::clamp<int32_t>(rect.offset.x, 0, static_cast<int32_t>(scene_extent_.width));
        const int32_t y0 = std::clamp<int32_t>(rect.offset.y, 0, static_cast<int32_t>(scene_extent_.height));
        const int32_t x1 = std::clamp<int32_t>(rect.offset.x + static_cast<int32_t>(rect.extent.width), 0, static_cast<int32_t>(scene_extent_.width));
        const int32_t y1 = std::clamp<int32_t>(rect.offset.y + static_cast<int32_t>(rect.extent.height), 0, static_cast<int32_t>(scene_extent_.height));
        std::memset(bgra, 0, static_cast<size_t>(rect.extent.width) * rect.extent.height * 4);
        if (x1 <= x0 || y1 <= y0) return true;
        const uint32_t     w = static_cast<uint32_t>(x1 - x0), h = static_cast<uint32_t>(y1 - y0);
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
        if (pixels_.size < bytes)
        {
            // The previous buffer is not read by anything in flight: readbacks wait for their copy.
            allocator_->Destroy(pixels_);
            if (!allocator_->CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, resources::Access::Readback, pixels_) || !pixels_.mapped)
            {
                allocator_->Destroy(pixels_);
                return false;
            }
        }
        const VkCommandBuffer cmd = frames_.Current();
        Barrier(cmd, image->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                kColorStage | VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset      = {x0, y0, 0};
        region.imageExtent      = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pixels_.buffer, 1, &region);
        Barrier(cmd, image->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, kColorStage, kColorWrite);
        VkMemoryBarrier2 host{};
        host.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        host.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host.dstStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT;
        host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{};
        dependency.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers    = &host;
        vkCmdPipelineBarrier2(cmd, &dependency);
        if (!Flush()) return false;
        allocator_->Invalidate(pixels_);
        const uint8_t* from = static_cast<const uint8_t*>(pixels_.mapped);
        for (uint32_t row = 0; row < h; ++row)
            std::memcpy(bgra + static_cast<size_t>(y0 - rect.offset.y + static_cast<int32_t>(row)) * rect.extent.width + (x0 - rect.offset.x),
                        from + static_cast<size_t>(row) * w * 4, static_cast<size_t>(w) * 4);
        ++stats_.readbacks;
        return true;
    }

    void Display::Clear(bool color, bool depth, const float rgba[4], float z, VkRect2D rect)
    {
        if (!frames_.Open() || (!color && !depth) || !EnsureScene()) return;
        // Clamped to the scene; an empty rectangle clears nothing.
        const int32_t x0 = std::clamp<int32_t>(rect.offset.x, 0, static_cast<int32_t>(scene_extent_.width));
        const int32_t y0 = std::clamp<int32_t>(rect.offset.y, 0, static_cast<int32_t>(scene_extent_.height));
        const int32_t x1 = std::clamp<int32_t>(rect.offset.x + static_cast<int32_t>(rect.extent.width), 0, static_cast<int32_t>(scene_extent_.width));
        const int32_t y1 = std::clamp<int32_t>(rect.offset.y + static_cast<int32_t>(rect.extent.height), 0, static_cast<int32_t>(scene_extent_.height));
        if (x1 <= x0 || y1 <= y0) return;
        Touch();
        Begin(Rendering::Full);

        VkClearAttachment attachments[2]{};
        uint32_t          count = 0;
        if (color)
        {
            attachments[count].aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
            attachments[count].colorAttachment = 0;
            std::memcpy(attachments[count].clearValue.color.float32, rgba, sizeof(float) * 4);
            ++count;
        }
        if (depth)
        {
            attachments[count].aspectMask                    = DepthAspect(depth_format_);
            attachments[count].clearValue.depthStencil.depth = z;
            ++count;
        }
        VkClearRect clear{};
        clear.rect       = {{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
        clear.layerCount = 1;
        vkCmdClearAttachments(frames_.Current(), count, attachments, 1, &clear);
    }

    VkCommandBuffer Display::BeginColorRendering()
    {
        if (!frames_.Open() || !EnsureScene()) return VK_NULL_HANDLE;
        Touch();
        Begin(Rendering::Color);
        return frames_.Current();
    }

    VkCommandBuffer Display::BeginSceneRendering()
    {
        if (!frames_.Open() || !EnsureScene()) return VK_NULL_HANDLE;
        Touch();
        Begin(Rendering::Full);
        return frames_.Current();
    }

    uint64_t Display::FramesCompleted()
    {
        if (!timeline_) return completed_frame_;
        const uint64_t done = context_ && context_->Lost() ? UINT64_MAX : timeline_->Completed();
        size_t         kept = 0;
        for (const Submitted& s : submitted_)
        {
            if (s.serial <= done) completed_frame_ = std::max(completed_frame_, s.frame);
            else submitted_[kept++] = s;
        }
        submitted_.resize(kept);
        return completed_frame_;
    }

    void Display::AfterFrame(std::function<void()> release)
    {
        if (!release) return;
        if (frames_.Open()) after_.push_back(std::move(release));
        else graveyard_.Bury(timeline_->Last(), std::move(release));
    }

    void Display::CollectNow()
    {
        if (context_ && context_->Ready()) graveyard_.Collect(context_->Lost() ? UINT64_MAX : timeline_->Completed());
    }

    uint64_t Display::Submit(const frame::Timeline::WaitOn* waits, uint32_t wait_count, const VkSemaphore* signals,
                             uint32_t signal_count)
    {
        const uint64_t serial = frames_.Submit(waits, wait_count, signals, signal_count);
        // What this frame named goes once it completes; with no submission, once the last one has.
        const uint64_t at = serial ? serial : timeline_->Last();
        submitted_.push_back(Submitted{stats_.frames, at});
        for (auto& release : after_) graveyard_.Bury(at, std::move(release));
        after_.clear();
        if (serial) last_serial_ = serial;
        return serial;
    }

    void Display::EndTimestamp()
    {
        if (!sink_ || !queries_) return;
        const uint32_t slot = frames_.Index();
        vkCmdWriteTimestamp2(frames_.Current(), VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, queries_, slot * 2 + 1);
        sink_->End(static_cast<uint16_t>(slot * 2 + 1));
    }

    void Display::Sleep(uint64_t ns)
    {
        // The bulk asleep, the last millisecond spun: the platform's sleep is coarse.
        const uint64_t until = NowNs() + ns;
        if (ns > 1'500'000) std::this_thread::sleep_for(std::chrono::nanoseconds(ns - 1'000'000));
        while (NowNs() < until) std::this_thread::yield();
    }

    bool Display::RebuildSwapchain(char* why, uint32_t why_size)
    {
        if (!surface_ && make_surface_) surface_ = make_surface_(context_->Instance(), surface_user_, why, why_size);
        if (!surface_) return false;
        const bool built = swapchain_.Build(*context_, surface_, window_, sync_, graveyard_, timeline_->Last(), why, why_size);
        rebuild_         = false;
        if (built) ++stats_.rebuilds;
        return built;
    }

    FrameEnd Display::EndFrame()
    {
        if (!frames_.Open()) return context_ && context_->Lost() ? FrameEnd::Lost : FrameEnd::Dropped;
        const VkCommandBuffer cmd = frames_.Current();
        EndRendering();
        if (hook_) hook_->FrameEnding(frames_.Upload());

        // A frame that never touched the scene still shows something defined: black.
        if (EnsureScene() && !touched_)
        {
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            Clear(true, true, black, 1.0f, VkRect2D{{0, 0}, scene_extent_});
            EndRendering();
        }
        if (!scene_.image)
        {
            EndTimestamp();
            Submit(nullptr, 0, nullptr, 0);
            return context_->Lost() ? FrameEnd::Lost : FrameEnd::Drawn;
        }
        // A multisampled scene is shown resolved, as D3D9's Present resolves its back buffer.
        if (samples_ > VK_SAMPLE_COUNT_1_BIT) ResolveScene();
        Barrier(cmd, Shown().image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kColorStage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        pass_.Prepare(frames_.Index());

        const uint64_t delay = limiter_.Delay(NowNs());
        if (delay) Sleep(delay);

        if (offscreen_)
        {
            Barrier(cmd, target_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_NONE, kColorStage, kColorWrite);
            pass_.Record(cmd, frames_.Index(), Shown().view, target_.view, kSceneFormat, {target_.width, target_.height});
            Barrier(cmd, target_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kColorStage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent      = {target_.width, target_.height, 1};
            vkCmdCopyImageToBuffer(cmd, target_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &region);
            EndTimestamp();
            const uint64_t serial = Submit(nullptr, 0, nullptr, 0);
            limiter_.Shown(NowNs());
            return serial ? FrameEnd::Drawn : (context_->Lost() ? FrameEnd::Lost : FrameEnd::Dropped);
        }

        if (!make_surface_ && !surface_)
        {
            EndTimestamp();
            Submit(nullptr, 0, nullptr, 0);
            return context_->Lost() ? FrameEnd::Lost : FrameEnd::Drawn;
        }

        char why[256] = {};
        if ((rebuild_ || !swapchain_.Ready()) && !RebuildSwapchain(why, sizeof why))
        {
            if (why[0]) Log(LogLevel::Debug, "no swapchain this frame: %s", why);
            EndTimestamp();
            Submit(nullptr, 0, nullptr, 0);
            if (context_->Lost()) return FrameEnd::Lost;
            // Minimised: nothing to show and nothing wrong.
            return swapchain_.Zero() ? FrameEnd::Drawn : FrameEnd::Dropped;
        }

        uint32_t      index   = 0;
        const Outcome acquire = swapchain_.Acquire(frames_.Acquire(), kAcquireNs, index);
        if (acquire != Outcome::Ok && acquire != Outcome::Suboptimal)
        {
            if (acquire == Outcome::Lost)
            {
                frames_.Abandon();
                submitted_.push_back(Submitted{stats_.frames, timeline_->Last()});
                for (auto& release : after_) graveyard_.Bury(timeline_->Last(), std::move(release));
                after_.clear();
                return FrameEnd::Lost;
            }
            if (acquire == Outcome::Timeout) ++stats_.acquire_timeouts;
            if (acquire == Outcome::Rebuild) rebuild_ = true;
            if (acquire == Outcome::SurfaceLost)
            {
                ++stats_.surfaces_lost;
                DropSurface();
                rebuild_ = true;
            }
            if (acquire == Outcome::Failed) Log(LogLevel::Debug, "vkAcquireNextImageKHR failed; the frame is dropped");
            EndTimestamp();
            Submit(nullptr, 0, nullptr, 0);
            ++stats_.dropped;
            return FrameEnd::Dropped;
        }
        if (acquire == Outcome::Suboptimal) rebuild_ = true;

        const VkImage image = swapchain_.Image(index);
        Barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColorStage,
                VK_ACCESS_2_NONE, kColorStage, kColorWrite);
        pass_.Record(cmd, frames_.Index(), Shown().view, swapchain_.View(index), swapchain_.Format(), swapchain_.Extent());
        Barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                kColorStage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
        EndTimestamp();

        const frame::Timeline::WaitOn acquired{frames_.Acquire(), kColorStage};
        const VkSemaphore             rendered = swapchain_.Rendered(index);
        const uint64_t                serial   = Submit(&acquired, 1, &rendered, 1);
        if (!serial)
        {
            // The image was acquired and its semaphore signalled, but nothing waits on it now.
            if (const VkSemaphore spent = frames_.RenewAcquire())
            {
                const VkDevice dev = context_->Device();
                graveyard_.Bury(timeline_->Last(), [dev, spent] { vkDestroySemaphore(dev, spent, nullptr); });
            }
            return context_->Lost() ? FrameEnd::Lost : FrameEnd::Dropped;
        }

        const Outcome present = swapchain_.Present(index);
        limiter_.Shown(NowNs());
        if (present == Outcome::Lost) return FrameEnd::Lost;
        if (present == Outcome::Rebuild || present == Outcome::Suboptimal) rebuild_ = true;
        if (present == Outcome::SurfaceLost)
        {
            ++stats_.surfaces_lost;
            DropSurface();
            rebuild_ = true;
        }
        if (present == Outcome::Ok || present == Outcome::Suboptimal)
        {
            ++stats_.presented;
            return FrameEnd::Presented;
        }
        ++stats_.dropped;
        return FrameEnd::Dropped;
    }

    bool Display::ReadOffscreen(std::vector<uint32_t>& pixels)
    {
        if (!offscreen_ || !readback_.mapped || !last_serial_) return false;
        if (timeline_->WaitFor(last_serial_) != frame::Wait::Done) return false;
        allocator_->Invalidate(readback_);
        pixels.resize(static_cast<size_t>(target_.width) * target_.height);
        std::memcpy(pixels.data(), readback_.mapped, pixels.size() * 4);
        return true;
    }

    bool Display::WaitSubmitted()
    {
        if (!timeline_) return false;
        const uint64_t last = timeline_->Last();
        return !last || timeline_->WaitFor(last) == frame::Wait::Done;
    }
}
