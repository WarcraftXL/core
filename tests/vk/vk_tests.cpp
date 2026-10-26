// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The renderer's lower modules (src/gx/vk/) against a real Vulkan driver, headless: built natively
// for Linux by tests/vk/CMakeLists.txt and run on Mesa's lavapipe, and built for Windows in core's
// own trees (ctest `vk`), where it runs on any driver, Wine's winevulkan included.
//
//   - the floor and the choice among devices, on made-up devices: what is refused, why, and which
//     device wins;
//   - the instance and the device on the machine's driver, a device refused by a floor above it;
//   - memory through VMA, a submission on the timeline and a wait that runs out;
//   - an image cleared by dynamic rendering, a part of it cleared again inside the rendering, and
//     read back to the CPU byte for byte;
//   - the present module and Dear ImGui (present_tests.cpp);
//   - the frame capture's reader (capture_tests.cpp);
//   - the engine's draws through the renderer, and a capture replayed (draw_tests.cpp);
//   - every program held to spirv-val (spirv_tests.cpp, in the native tree).
//
// Exit 0 when every check passes, 1 when one fails, 77 (ctest's skip) when the machine has no
// Vulkan 1.3 device to run the live half on.

#include "gx/vk/Log.hpp"
#include "gx/vk/device/Device.hpp"
#include "gx/vk/frame/Frame.hpp"
#include "gx/vk/present/Present.hpp"
#include "gx/vk/resources/Resources.hpp"

#include "Check.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    namespace vk = wxl::vk;
    using vk::device::Candidate;

    int g_failures = 0;
    int g_passes   = 0;

    using wxl::vk::test::Check;

    void Print(vk::LogLevel level, const char* text, void*)
    {
        // The renderer's debug lines only when asked for: WXL_VK_VERBOSE=1.
        if (level == vk::LogLevel::Debug && !wxl::vk::test::Environment("WXL_VK_VERBOSE")) return;
        static const char* const kNames[] = {"debug", "info", "warn", "error"};
        std::printf("  [%s] %s\n", kNames[static_cast<int>(level)], text);
    }

    /// A device that meets the floor, to be spoiled by each case.
    Candidate Good(uint32_t index, const char* name, VkPhysicalDeviceType type, uint64_t mib)
    {
        Candidate c;
        c.index = index;
        std::snprintf(c.name, sizeof c.name, "%s", name);
        c.type         = type;
        c.api          = VK_API_VERSION_1_3;
        c.device_local = mib << 20;
        c.family       = 0;
        c.presents     = true;
#define WXL_VK_FEATURE_ON(member, version, required, where) c.features.member = true;
        WXL_VK_FEATURES(WXL_VK_FEATURE_ON)
#undef WXL_VK_FEATURE_ON
        c.extensions.swapchain = true;
        return c;
    }

    void FloorCases()
    {
        const vk::device::Floor floor;

        Candidate old = Good(0, "Old GPU", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 2048);
        old.api       = VK_MAKE_API_VERSION(0, 1, 2, 198);
        vk::device::Judge(old, floor);
        Check(std::strcmp(old.reject, "Vulkan 1.2.198, below the 1.3 floor") == 0, "a 1.2 device is refused on its version");

        Candidate noBda                    = Good(1, "No BDA", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 2048);
        noBda.features.bufferDeviceAddress = false;
        vk::device::Judge(noBda, floor);
        Check(std::strcmp(noBda.reject, "missing feature bufferDeviceAddress (Vulkan 1.2)") == 0,
              "a missing required feature is named");

        Candidate noAniso                  = Good(2, "No anisotropy", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 2048);
        noAniso.features.samplerAnisotropy = false;
        vk::device::Judge(noAniso, floor);
        Check(noAniso.reject[0] == '\0', "a missing optional feature does not refuse");

        Candidate noPresent = Good(3, "Compute only", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 2048);
        noPresent.presents  = false;
        vk::device::Judge(noPresent, floor);
        Check(std::strstr(noPresent.reject, "cannot present") != nullptr, "a device that cannot present is refused");
        vk::device::Floor headless;
        headless.present = false;
        vk::device::Judge(noPresent, headless);
        Check(noPresent.reject[0] == '\0', "a headless floor takes it");

        Candidate noQueue = Good(4, "No graphics", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 2048);
        noQueue.family    = UINT32_MAX;
        vk::device::Judge(noQueue, floor);
        Check(std::strcmp(noQueue.reject, "no graphics queue") == 0, "a device without a graphics queue is refused");

        // The choice: discrete over integrated over CPU, whatever the order.
        Candidate list[4] = {Good(0, "llvmpipe (LLVM 19)", VK_PHYSICAL_DEVICE_TYPE_CPU, 0),
                             Good(1, "Intel(R) UHD Graphics", VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 512),
                             Good(2, "NVIDIA GeForce RTX 3060", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 12288),
                             Good(3, "AMD Radeon RX 480", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 8192)};
        list[3].api = VK_MAKE_API_VERSION(0, 1, 2, 0);
        for (Candidate& c : list) vk::device::Judge(c, floor);
        vk::device::Choice choice = vk::device::Choose(list, 4, nullptr);
        Check(choice.index == 2, "the discrete GPU that qualifies wins");
        Check(std::strcmp(choice.why, "the better device type") == 0, "the rule that decided is named");

        choice = vk::device::Choose(list, 4, "intel");
        Check(choice.index == 1 && std::strstr(choice.why, "[gx] gpu") != nullptr, "[gx] gpu names a device by a part of its name");
        choice = vk::device::Choose(list, 4, "0");
        Check(choice.index == 0, "[gx] gpu names a device by its index");
        choice = vk::device::Choose(list, 4, "Radeon");
        Check(choice.index == 2 && std::strstr(choice.note, "does not qualify") != nullptr,
              "a preferred device below the floor is passed over, and said so");
        choice = vk::device::Choose(list, 4, "Matrox");
        Check(choice.index == 2 && std::strstr(choice.note, "names no device") != nullptr, "a preference naming nothing is said so");

        Candidate twins[2] = {Good(0, "GPU A", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 4096),
                              Good(1, "GPU B", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 8192)};
        for (Candidate& c : twins) vk::device::Judge(c, floor);
        choice = vk::device::Choose(twins, 2, nullptr);
        Check(choice.index == 1 && std::strcmp(choice.why, "more device memory") == 0, "the larger device memory breaks a tie");

        // Nothing qualifies: the refusal names every device and its reason, for the fallback line.
        Candidate none[2] = {old, noBda};
        choice = vk::device::Choose(none, 2, nullptr);
        Check(choice.index < 0, "no device qualifies");
        char refusals[400];
        vk::device::Refusals(none, 2, refusals, sizeof refusals);
        Check(std::strcmp(refusals, "Old GPU: Vulkan 1.2.198, below the 1.3 floor; No BDA: missing feature bufferDeviceAddress "
                                    "(Vulkan 1.2)") == 0,
              "the refusals read as one line");

        char driver[32];
        vk::device::FormatDriver(0x10DE, (580u << 22) | (95u << 14) | (5u << 6), driver, sizeof driver);
        Check(std::strcmp(driver, "580.95.5") == 0, "NVIDIA's driver version is read in its packing");
    }

    /// Clears a 64x32 image by dynamic rendering, clears a quarter of it again inside the
    /// rendering, copies it to a host buffer and reads it.
    void ClearAndRead(vk::resources::Allocator& allocator, vk::frame::Submitter& submitter)
    {
        constexpr uint32_t kWidth = 64, kHeight = 32;
        vk::resources::Image  image;
        vk::resources::Buffer readback;
        const bool made = allocator.CreateImage(kWidth, kHeight, VK_FORMAT_R8G8B8A8_UNORM,
                                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, image) &&
                          allocator.CreateBuffer(kWidth * kHeight * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                 vk::resources::Access::Readback, readback);
        Check(made && readback.mapped, "an image and a mapped readback buffer are made");
        if (!made || !readback.mapped) return;

        VkCommandBuffer cmd = submitter.Begin();
        Check(cmd != VK_NULL_HANDLE, "a command buffer is handed out");
        if (!cmd) return;

        VkImageMemoryBarrier2 toTarget{};
        toTarget.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toTarget.srcStageMask     = VK_PIPELINE_STAGE_2_NONE;
        toTarget.dstStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toTarget.dstAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toTarget.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
        toTarget.newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toTarget.image            = image.image;
        toTarget.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{};
        dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers    = &toTarget;
        vkCmdPipelineBarrier2(cmd, &dependency);

        VkRenderingAttachmentInfo color{};
        color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView   = image.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.25f, 0.5f, 0.75f, 1.0f}};
        VkRenderingInfo rendering{};
        rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea           = {{0, 0}, {kWidth, kHeight}};
        rendering.layerCount           = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments    = &color;
        vkCmdBeginRendering(cmd, &rendering);
        // The engine's SceneClear of a part of the target, inside a rendering already begun.
        VkClearAttachment part{};
        part.aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
        part.colorAttachment = 0;
        part.clearValue.color = {{1.0f, 0.0f, 0.0f, 1.0f}};
        VkClearRect rect{};
        rect.rect       = {{kWidth / 2, kHeight / 2}, {kWidth / 2, kHeight / 2}};
        rect.layerCount = 1;
        vkCmdClearAttachments(cmd, 1, &part, 1, &rect);
        vkCmdEndRendering(cmd);

        VkImageMemoryBarrier2 toCopy = toTarget;
        toCopy.srcStageMask          = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toCopy.srcAccessMask         = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toCopy.dstStageMask          = VK_PIPELINE_STAGE_2_COPY_BIT;
        toCopy.dstAccessMask         = VK_ACCESS_2_TRANSFER_READ_BIT;
        toCopy.oldLayout             = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toCopy.newLayout             = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        dependency.pImageMemoryBarriers = &toCopy;
        vkCmdPipelineBarrier2(cmd, &dependency);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent      = {kWidth, kHeight, 1};
        vkCmdCopyImageToBuffer(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);

        VkMemoryBarrier2 toHost{};
        toHost.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        toHost.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toHost.dstStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT;
        toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo hostDependency{};
        hostDependency.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        hostDependency.memoryBarrierCount = 1;
        hostDependency.pMemoryBarriers    = &toHost;
        vkCmdPipelineBarrier2(cmd, &hostDependency);

        const uint64_t serial = submitter.Submit();
        Check(serial != 0, "the clear is submitted");
        const vk::frame::Wait wait = submitter.WaitFor(serial);
        Check(wait == vk::frame::Wait::Done, "the clear completes within the bound");
        Check(submitter.Completed() >= serial, "the timeline reached the clear's serial");
        allocator.Invalidate(readback);

        const auto* pixels = static_cast<const uint8_t*>(readback.mapped);
        auto        near   = [](uint8_t a, int b) { return a >= b - 1 && a <= b + 1; };
        bool        base = true, red = true;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const uint8_t* p     = pixels + (y * kWidth + x) * 4;
                const bool     inRed = x >= kWidth / 2 && y >= kHeight / 2;
                if (inRed) red = red && p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255;
                else base = base && near(p[0], 64) && near(p[1], 128) && near(p[2], 191) && p[3] == 255;
            }
        Check(base, "the cleared image reads back as (64, 128, 191, 255)");
        Check(red, "the quarter cleared inside the rendering reads back red");
        std::printf("  pixel (0,0) = %u %u %u %u, pixel (63,31) = %u %u %u %u\n", pixels[0], pixels[1], pixels[2], pixels[3],
                    pixels[(31 * kWidth + 63) * 4], pixels[(31 * kWidth + 63) * 4 + 1], pixels[(31 * kWidth + 63) * 4 + 2],
                    pixels[(31 * kWidth + 63) * 4 + 3]);

        allocator.Destroy(readback);
        allocator.Destroy(image);
    }

    using wxl::vk::test::Environment;

    /// The live half; false when the machine has no device to run it on.
    bool Live()
    {
        vk::device::Options options;
        options.application   = "wxl-vk-tests";
        options.floor.present = false;
        options.validation    = Environment("WXL_VK_VALIDATION");
        // The window's extensions first, as the runtime asks for them, so that the presentation
        // query is valid. Wine with no display behind it lists VK_KHR_win32_surface but refuses
        // it at vkCreateInstance (its host has no surface to map it to): then headless.
        options.instance_extensions = vk::present::InstanceExtensions(options.instance_extension_count);

        char                why[512] = {};
        vk::device::Context context;
        bool                made = context.Create(options, why, sizeof why);
        if (!made && options.instance_extension_count)
        {
            std::printf("  with the window's instance extensions: %s; again headless\n", why);
            options.instance_extensions      = nullptr;
            options.instance_extension_count = 0;
            made                             = context.Create(options, why, sizeof why);
        }
        if (!made)
        {
            std::printf("SKIP live: %s\n", why);
            return false;
        }
        const Candidate& c = context.Chosen();
        char api[32];
        vk::FormatVersion(c.api, api, sizeof api);
        std::printf("  device: %s (%s, Vulkan %s), queue family %u\n", c.name, vk::device::TypeName(c.type), api, c.family);
        Check(context.Ready() && context.Queue() != VK_NULL_HANDLE, "a device and its queue");
        Check(context.InstanceApi() >= vk::kFloorApi, "the loader is at the floor");
        std::printf("  presents to a window: %s\n",
                    !vk::present::Available() ? "headless build"
                    : !options.instance_extension_count
                        ? "not asked (no window extensions)"
                        : vk::present::Presents(context.Instance(), context.Physical(), c.family) ? "yes"
                                                                                                    : "no");

        vk::resources::Allocator allocator;
        Check(allocator.Create(context, why, sizeof why), "VMA starts on the device");
        vk::frame::Timeline timeline;
        Check(timeline.Create(context, why, sizeof why), "a timeline semaphore");
        vk::frame::Submitter submitter;
        Check(submitter.Create(timeline, why, sizeof why), "a command pool on the timeline");
        if (allocator.Ready())
        {
            vk::resources::Buffer upload;
            Check(allocator.CreateBuffer(1 << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, vk::resources::Access::Upload, upload) &&
                      upload.mapped,
                  "a mapped upload buffer");
            allocator.Destroy(upload);
            const vk::resources::Budget budget = allocator.CurrentBudget();
            std::printf("  memory budget %llu MiB, used %llu MiB\n", static_cast<unsigned long long>(budget.budget >> 20),
                        static_cast<unsigned long long>(budget.usage >> 20));
            ClearAndRead(allocator, submitter);
        }

        // A serial nobody will signal: the wait comes back when its time is up.
        Check(submitter.WaitFor(submitter.Last() + 1, 20'000'000ull) == vk::frame::Wait::Timeout,
              "a wait on a serial never submitted times out");
        Check(!context.Lost(), "the device was not lost");

        submitter.Destroy();
        timeline.Destroy();
        allocator.Destroy();
        context.Destroy();
        Check(!context.Ready(), "the device is released");

        // The same machine under a floor above every device: refused, every device named.
        vk::device::Options higher = options;
        higher.floor.api           = VK_MAKE_API_VERSION(0, 1, 9, 0);
        why[0]                     = '\0';
        Check(!context.Create(higher, why, sizeof why), "a floor above the driver refuses the device");
        std::printf("  refused: %s\n", why);
        Check(std::strstr(why, "no device meets the Vulkan 1.9 floor") != nullptr && std::strstr(why, c.name) != nullptr,
              "the refusal names the floor and the device");

        // [gx] gpu naming the device taken before, by its name.
        vk::device::Options named = options;
        named.gpu                 = c.name;
        Check(context.Create(named, why, sizeof why) && context.Chosen().index == c.index, "[gx] gpu takes the device it names");
        context.Destroy();
        return true;
    }
}

namespace wxl::vk::test
{
    void Check(bool ok, const char* what)
    {
        // WXL_VK_TRACE=1 names every check as it passes, to find where a run stops.
        static const bool trace = Environment("WXL_VK_TRACE");
        if (ok && trace) std::printf("ok   %s\n", what);
        if (ok) ++g_passes;
        else
        {
            ++g_failures;
            std::printf("FAIL %s\n", what);
        }
    }

    bool Environment(const char* name)
    {
#if defined(_WIN32)
        char   value[260];
        size_t length = 0;
        return getenv_s(&length, value, sizeof value, name) == 0 && length > 0;
#else
        return std::getenv(name) != nullptr;
#endif
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    vk::SetLogSink(Print, nullptr);

    FloorCases();
    wxl::vk::test::PresentPureCases();
    wxl::vk::test::CaptureCases();
    wxl::vk::test::DrawPureCases();
    wxl::vk::test::TranslatePureCases();
    wxl::vk::test::ParityPureCases();
    if (!wxl::vk::test::SpirvValidation()) std::printf("(spirv-val is not in this build: the native tree of tests/vk/ runs it)\n");
    bool live = Live();
    if (live) live = wxl::vk::test::PresentLiveCases();
    if (live) wxl::vk::test::DrawLiveCases();
    if (live) wxl::vk::test::V4LiveCases();
    if (live) wxl::vk::test::ParityLiveCases();
    if (live) wxl::vk::test::TranslateLiveCases();

    std::printf("%d passed, %d failed%s\n", g_passes, g_failures, live ? "" : " (the live half skipped)");
    if (g_failures) return 1;
    return live ? 0 : 77;
}
