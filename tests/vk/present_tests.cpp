// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The present module and Dear ImGui on the machine's driver (lavapipe here), and the pure rules
// beside them:
//
//   - the choices a swapchain is made by, how acquire and present results are read, the frame
//     limiter and the graveyard, without a device;
//   - the renderer's own SPIR-V turned into modules and pipelines;
//   - the present pass into an offscreen image read back: the scene stretched (nearest exactly,
//     bilinear inside), the gamma ramp;
//   - the frame loop with 1, 2 and 3 frames in flight, clears of a part of the scene, the scene
//     resized, GPU timestamps, nothing left allocated after;
//   - a lost device: the frame says so at once and the display goes without waiting;
//   - a real swapchain on a headless surface (when the driver has VK_EXT_headless_surface):
//     presents, a resize rebuilds it, vsync off picks MAILBOX or IMMEDIATE, minimised shows nothing;
//   - Dear ImGui's Vulkan backend drawing into the scene, its rectangles and its blending checked
//     pixel by pixel, its text present.
//
// With WXL_VK_TEST_OUT set to a folder, the images read back are written there as PPM files.

#include "gx/vk/Log.hpp"
#include "gx/vk/device/Device.hpp"
#include "gx/vk/frame/Frame.hpp"
#include "gx/vk/imgui/ImGuiVk.hpp"
#include "gx/vk/present/Display.hpp"
#include "gx/vk/present/Limiter.hpp"
#include "gx/vk/present/Present.hpp"
#include "gx/vk/present/Swapchain.hpp"
#include "gx/vk/resources/Resources.hpp"
#include "gx/vk/shaders/Own.hpp"

#include "Check.hpp"

#include <imgui.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    namespace vk = wxl::vk;
    using vk::test::Check;
    using vk::present::Display;
    using vk::present::FrameEnd;

    // --- Helpers -------------------------------------------------------------------------------------

    std::string OutDir()
    {
#if defined(_WIN32)
        char   value[260] = {};
        size_t length     = 0;
        if (getenv_s(&length, value, sizeof value, "WXL_VK_TEST_OUT") != 0 || length == 0) return {};
        return value;
#else
        const char* value = std::getenv("WXL_VK_TEST_OUT");
        return value ? value : "";
#endif
    }

    /// Writes B8G8R8A8 pixels as a PPM when WXL_VK_TEST_OUT is set.
    void Save(const char* name, const std::vector<uint32_t>& pixels, uint32_t width, uint32_t height)
    {
        const std::string dir = OutDir();
        if (dir.empty()) return;
        const std::string path = dir + "/" + name + ".ppm";
        FILE*             file = std::fopen(path.c_str(), "wb");
        if (!file) return;
        std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        for (uint32_t p : pixels)
        {
            const unsigned char rgb[3] = {static_cast<unsigned char>(p >> 16), static_cast<unsigned char>(p >> 8),
                                          static_cast<unsigned char>(p)};
            std::fwrite(rgb, 1, 3, file);
        }
        std::fclose(file);
        std::printf("  wrote %s\n", path.c_str());
    }

    bool Near(uint32_t a, uint32_t b, int tolerance)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            const int x = static_cast<int>((a >> shift) & 0xFF), y = static_cast<int>((b >> shift) & 0xFF);
            if (x - y > tolerance || y - x > tolerance) return false;
        }
        return true;
    }

    /// 0xAARRGGBB, as the scene's B8G8R8A8 reads back as a little-endian word.
    uint32_t Argb(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
    {
        return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
    }

    void Rgba(uint32_t argb, float out[4])
    {
        out[0] = ((argb >> 16) & 0xFF) / 255.0f;
        out[1] = ((argb >> 8) & 0xFF) / 255.0f;
        out[2] = (argb & 0xFF) / 255.0f;
        out[3] = ((argb >> 24) & 0xFF) / 255.0f;
    }

    void ClearScene(Display& display, uint32_t argb, VkRect2D rect)
    {
        float rgba[4];
        Rgba(argb, rgba);
        display.Clear(true, true, rgba, 1.0f, rect);
    }

    struct Counter final : vk::present::TimestampSink
    {
        int begins = 0, ends = 0, times = 0;
        int64_t last = 0;
        bool    ordered = true; ///< every frame's end came at or after its begin
        int64_t pending[16] = {};

        void Begin(uint16_t) override { ++begins; }
        void End(uint16_t) override { ++ends; }
        void Time(uint16_t query, int64_t ticks) override
        {
            ++times;
            if (query % 2 == 1 && ticks < pending[query - 1]) ordered = false;
            pending[query % 16] = ticks;
            last                = ticks;
        }
    };

    // --- Pure cases ------------------------------------------------------------------------------------

    void ChoiceCases()
    {
        using namespace vk::present;
        const VkPresentModeKHR all[]  = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR};
        const VkPresentModeKHR noMb[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
        const VkPresentModeKHR fifo[] = {VK_PRESENT_MODE_FIFO_KHR};
        Check(ChoosePresentMode(Sync::On, all, 3) == VK_PRESENT_MODE_FIFO_KHR, "vsync on is FIFO");
        Check(ChoosePresentMode(Sync::Off, all, 3) == VK_PRESENT_MODE_MAILBOX_KHR, "vsync off prefers MAILBOX");
        Check(ChoosePresentMode(Sync::Off, noMb, 2) == VK_PRESENT_MODE_IMMEDIATE_KHR, "vsync off without MAILBOX is IMMEDIATE");
        Check(ChoosePresentMode(Sync::Off, fifo, 1) == VK_PRESENT_MODE_FIFO_KHR, "vsync off with FIFO alone is FIFO");

        const VkSurfaceFormatKHR srgbFirst[] = {{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                                {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                                {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
        Check(ChooseFormat(srgbFirst, 3).format == VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM is preferred, an sRGB format is passed over");
        const VkSurfaceFormatKHR onlySrgb[] = {{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                               {VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
        Check(ChooseFormat(onlySrgb, 2).format == VK_FORMAT_A2R10G10B10_UNORM_PACK32, "a non-sRGB format wins over an sRGB one");

        VkSurfaceCapabilitiesKHR caps{};
        caps.currentExtent  = {800, 600};
        caps.minImageExtent = {1, 1};
        caps.maxImageExtent = {4096, 4096};
        caps.minImageCount  = 2;
        caps.maxImageCount  = 0;
        Check(ChooseExtent(caps, {1024, 768}).width == 800, "the surface's own extent wins");
        caps.currentExtent = {0xFFFFFFFFu, 0xFFFFFFFFu};
        Check(ChooseExtent(caps, {1024, 768}).width == 1024, "a surface that lets the swapchain decide takes the window's size");
        Check(ChooseExtent(caps, {9000, 768}).width == 4096, "clamped to the surface's maximum");
        Check(ChooseExtent(caps, {0, 0}).width == 0, "a minimised window gives a zero extent");
        caps.currentExtent = {0, 0};
        Check(ChooseExtent(caps, {1024, 768}).height == 0, "a surface reporting zero (minimised) gives zero");
        Check(ChooseImageCount(caps, VK_PRESENT_MODE_FIFO_KHR) == 3, "one image above the minimum");
        caps.maxImageCount = 2;
        Check(ChooseImageCount(caps, VK_PRESENT_MODE_MAILBOX_KHR) == 2, "never above the maximum");

        Check(Classify(VK_SUCCESS) == Outcome::Ok && Classify(VK_SUBOPTIMAL_KHR) == Outcome::Suboptimal &&
                  Classify(VK_ERROR_OUT_OF_DATE_KHR) == Outcome::Rebuild && Classify(VK_ERROR_SURFACE_LOST_KHR) == Outcome::SurfaceLost &&
                  Classify(VK_TIMEOUT) == Outcome::Timeout && Classify(VK_NOT_READY) == Outcome::Timeout &&
                  Classify(VK_ERROR_DEVICE_LOST) == Outcome::Lost &&
                  Classify(VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) == Outcome::Rebuild &&
                  Classify(VK_ERROR_OUT_OF_HOST_MEMORY) == Outcome::Failed,
              "every acquire and present result is read as a rebuild, a loss or a failure");
    }

    void LimiterCases()
    {
        vk::present::Limiter limiter;
        Check(limiter.Delay(1000) == 0, "the limiter off never waits");
        const uint64_t interval = 33'333'333; // vsync 2 at 60 Hz
        limiter.SetInterval(interval);
        Check(limiter.Delay(5) == 0, "the first frame does not wait");
        limiter.Shown(1'000'000'000);
        Check(limiter.Delay(1'000'000'000 + 10'000'000) == interval - 10'000'000, "the next frame waits out the interval");
        limiter.Shown(1'000'000'000 + interval);
        Check(limiter.Delay(1'000'000'000 + interval) == interval, "frames keep their cadence");
        limiter.Shown(5'000'000'000); // far behind: the cadence starts again from now
        Check(limiter.Delay(5'000'000'000) == interval, "a frame long late restarts the cadence");
        vk::present::Limiter longOne;
        longOne.SetInterval(10'000'000'000ull);
        longOne.Shown(0);
        Check(longOne.Delay(0) == vk::present::Limiter::kMaxDelayNs, "no wait is longer than the bound");
    }

    void GraveyardCases()
    {
        vk::resources::Graveyard graveyard;
        std::vector<int>         freed;
        graveyard.Bury(3, [&] { freed.push_back(3); });
        graveyard.Bury(5, [&] { freed.push_back(5); });
        graveyard.Bury(5, [&] { freed.push_back(55); });
        graveyard.Collect(2);
        Check(freed.empty() && graveyard.Size() == 3, "nothing is freed before its serial");
        graveyard.Collect(4);
        Check(freed.size() == 1 && freed[0] == 3, "a release runs once its serial completed");
        graveyard.Collect(9);
        Check(freed.size() == 3 && freed[2] == 55 && graveyard.Size() == 0, "in the order they came");
        graveyard.Bury(100, [&] { freed.push_back(100); });
        graveyard.Flush();
        Check(freed.size() == 4, "a flush frees everything");
    }

    void SpirvCases()
    {
        const std::vector<uint32_t> vs = vk::shaders::PresentVertex();
        const std::vector<uint32_t> fs = vk::shaders::PresentFragment();
        Check(vs.size() > 5 && vs[0] == 0x07230203u && vs[1] == 0x00010300u && vs[3] > 1, "the vertex module has a SPIR-V 1.3 header and a bound");
        Check(fs.size() > vs.size(), "the fragment module is written");
        // Every instruction's word count adds up to the module's length.
        auto walk = [](const std::vector<uint32_t>& m) {
            size_t at = 5;
            while (at < m.size())
            {
                const uint32_t count = m[at] >> 16;
                if (!count) return false;
                at += count;
            }
            return at == m.size();
        };
        Check(walk(vs) && walk(fs), "every instruction's length adds up");
    }

    // --- Live cases ------------------------------------------------------------------------------------

    struct Device
    {
        vk::device::Context      context;
        vk::resources::Allocator allocator;
        vk::frame::Timeline      timeline;
        bool                     headless = false; ///< made with a headless surface's extensions
    };

    bool Make(Device& d, bool headless)
    {
        vk::device::Options options;
        options.application   = "wxl-vk-tests";
        options.floor.present = false;
        options.swapchain     = headless;
        if (headless) options.instance_extensions = vk::present::HeadlessExtensions(options.instance_extension_count);
        char why[512] = {};
        if (!d.context.Create(options, why, sizeof why))
        {
            if (headless) std::printf("  no headless surface here: %s\n", why);
            return false;
        }
        d.headless = headless;
        return d.allocator.Create(d.context, why, sizeof why) && d.timeline.Create(d.context, why, sizeof why);
    }

    void Release(Device& d)
    {
        d.timeline.Destroy();
        d.allocator.Destroy();
        d.context.Destroy();
    }

    FrameEnd Frame(Display& display, uint32_t argb)
    {
        vk::frame::Wait wait{};
        if (!display.BeginFrame(wait)) return FrameEnd::Lost;
        // Larger than any scene: the clear is clamped to whatever size the scene has this frame.
        ClearScene(display, argb, VkRect2D{{0, 0}, {4096, 4096}});
        return display.EndFrame();
    }

    void PassCases(Device& d)
    {
        const uint32_t baseline = d.allocator.Allocations();
        char           why[256] = {};
        Display        display;
        Display::Options options;
        options.frames = 2;
        options.filter = vk::present::Filter::Nearest;
        Check(display.Create(d.context, d.allocator, d.timeline, options, why, sizeof why), "a display is made");
        if (!display.Ready())
        {
            std::printf("  %s\n", why);
            return;
        }
        Check(display.SetOffscreen({64, 32}), "an offscreen target of 64x32");
        display.SetSceneExtent({32, 16});

        // Four quadrants, stretched twice with nearest: every pixel exact.
        const uint32_t tl = Argb(255, 0, 0), tr = Argb(0, 255, 0), bl = Argb(0, 0, 255), br = Argb(255, 255, 255);
        vk::frame::Wait wait{};
        Check(display.BeginFrame(wait), "a frame begins");
        ClearScene(display, tl, {{0, 0}, {16, 8}});
        ClearScene(display, tr, {{16, 0}, {16, 8}});
        ClearScene(display, bl, {{0, 8}, {16, 8}});
        ClearScene(display, br, {{16, 8}, {100, 100}}); // clamped to the scene
        Check(display.EndFrame() == FrameEnd::Drawn, "an offscreen frame is drawn");
        std::vector<uint32_t> pixels;
        Check(display.ReadOffscreen(pixels) && pixels.size() == 64 * 32, "the offscreen image reads back");
        Save("present-nearest", pixels, 64, 32);
        bool exact = pixels.size() == 64 * 32;
        for (uint32_t y = 0; exact && y < 32; ++y)
            for (uint32_t x = 0; x < 64; ++x)
            {
                const uint32_t want = y < 16 ? (x < 32 ? tl : tr) : (x < 32 ? bl : br);
                if (pixels[y * 64 + x] != want)
                {
                    std::printf("  (%u,%u) = %08x, want %08x\n", x, y, pixels[y * 64 + x], want);
                    exact = false;
                    break;
                }
            }
        Check(exact, "the scene's quadrants, cleared one by one, stretched twice with nearest, read back exactly");

        // Bilinear: the same away from the edges between quadrants.
        display.PresentPass().SetFilter(vk::present::Filter::Linear);
        Check(display.BeginFrame(wait), "a second frame begins");
        ClearScene(display, tl, {{0, 0}, {16, 8}});
        ClearScene(display, tr, {{16, 0}, {16, 8}});
        ClearScene(display, bl, {{0, 8}, {16, 8}});
        ClearScene(display, br, {{16, 8}, {16, 8}});
        display.EndFrame();
        display.ReadOffscreen(pixels);
        Save("present-linear", pixels, 64, 32);
        Check(pixels.size() == 64 * 32 && pixels[4 * 64 + 4] == tl && pixels[4 * 64 + 60] == tr && pixels[28 * 64 + 4] == bl &&
                  pixels[28 * 64 + 60] == br && pixels[15 * 64 + 31] != tl,
              "bilinear keeps each quadrant's colour inside it and blends at the edges");

        // The gamma ramp: inverted, the colour comes back as its complement.
        vk::present::GammaRamp invert{};
        for (int i = 0; i < 256; ++i) invert.red[i] = invert.green[i] = invert.blue[i] = static_cast<uint16_t>(65535 - i * 257);
        display.PresentPass().SetGamma(&invert);
        Frame(display, Argb(64, 128, 191));
        display.ReadOffscreen(pixels);
        Check(!pixels.empty() && Near(pixels[10 * 64 + 10], Argb(191, 127, 64), 1), "an inverted gamma ramp turns a colour into its complement");
        if (!pixels.empty()) std::printf("  gamma: (64,128,191) -> %08x\n", pixels[10 * 64 + 10]);
        vk::present::GammaRamp half{};
        for (int i = 0; i < 256; ++i) half.red[i] = half.green[i] = half.blue[i] = static_cast<uint16_t>(i * 257 / 2);
        display.PresentPass().SetGamma(&half);
        Frame(display, Argb(200, 100, 50));
        display.ReadOffscreen(pixels);
        Check(!pixels.empty() && Near(pixels[0], Argb(100, 50, 25), 1), "a ramp at half halves every channel");
        display.PresentPass().SetGamma(nullptr);
        Frame(display, Argb(200, 100, 50));
        display.ReadOffscreen(pixels);
        Check(!pixels.empty() && pixels[0] == Argb(200, 100, 50), "no ramp leaves the colour exactly as it was");

        display.SetSceneExtent({64, 32});
        const uint32_t grey = Argb(90, 90, 90);
        Frame(display, grey);

        // The scene made again at a new size, between frames and inside one.
        const uint64_t scenes = display.Counters().scenes;
        display.SetSceneExtent({40, 20});
        Frame(display, grey);
        Check(display.BeginFrame(wait), "a frame begins after a resize");
        ClearScene(display, grey, {{0, 0}, {40, 20}});
        display.SetSceneExtent({48, 24}); // the frame has named the old images already
        ClearScene(display, tl, {{0, 0}, {48, 24}});
        display.EndFrame();
        display.ReadOffscreen(pixels);
        Check(display.Counters().scenes == scenes + 2 && display.SceneExtent().width == 48 && !pixels.empty() && pixels[0] == tl,
              "a scene resized between frames and inside one is made again, the frame drawn with the new one");

        display.Destroy();
        Check(d.allocator.Allocations() == baseline, "the display leaves nothing allocated");
    }

    void LoopCases(Device& d)
    {
        const uint32_t baseline = d.allocator.Allocations();
        for (uint32_t frames = 1; frames <= 3; ++frames)
        {
            char             why[256] = {};
            Display          display;
            Display::Options options;
            options.frames = frames;
            if (!display.Create(d.context, d.allocator, d.timeline, options, why, sizeof why))
            {
                Check(false, "a display with N frames in flight");
                continue;
            }
            display.SetOffscreen({32, 32});
            Counter counter;
            const bool timed = display.SetTimestamps(&counter);
            int        drawn = 0;
            for (int i = 0; i < 12; ++i)
                if (Frame(display, Argb(static_cast<uint8_t>(i * 20), 0, 0)) == FrameEnd::Drawn) ++drawn;
            std::vector<uint32_t> pixels;
            display.ReadOffscreen(pixels);
            char what[160];
            std::snprintf(what, sizeof what, "%u frame(s) in flight: 12 frames drawn, the last one read back", frames);
            Check(drawn == 12 && !pixels.empty() && pixels[0] == Argb(220, 0, 0), what);
            if (timed)
            {
                std::snprintf(what, sizeof what, "%u frame(s) in flight: a GPU time for each frame begun and ended, in order", frames);
                Check(counter.begins == 12 && counter.ends == 12 && counter.times >= 2 * (12 - static_cast<int>(frames)) &&
                          counter.ordered,
                      what);
                int64_t ticks = 0;
                Check(display.SampleGpuTime(ticks) && ticks >= counter.last, "a GPU timestamp is sampled outside the frames");
            }
            else
                std::printf("  no timestamps on this queue\n");
            display.Destroy();
        }
        Check(d.allocator.Allocations() == baseline, "the frame loops leave nothing allocated");
    }

    void LossCases()
    {
        Device d;
        if (!Make(d, false))
        {
            Check(false, "a second device for the loss");
            return;
        }
        char             why[256] = {};
        Display          display;
        Display::Options options;
        Check(display.Create(d.context, d.allocator, d.timeline, options, why, sizeof why) && display.SetOffscreen({16, 16}),
              "a display on the device to lose");
        Check(Frame(display, Argb(1, 2, 3)) == FrameEnd::Drawn, "a frame before the loss");
        vk::frame::Wait wait{};
        Check(display.BeginFrame(wait), "a frame begins");
        d.context.MarkLost("the test's simulated loss");
        Check(display.EndFrame() == FrameEnd::Lost, "a frame open when the device is lost ends as lost");
        Check(!display.BeginFrame(wait) && wait == vk::frame::Wait::Lost, "a lost device begins no frame, and says why");
        const auto start = std::chrono::steady_clock::now();
        display.Destroy();
        Release(d);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        Check(ms < 1000, "a lost device's display and device go without waiting");
        // Made again: what the runtime does at the next scene.
        Check(Make(d, false) && !d.context.Lost(), "the device is made again after a loss");
        Check(display.Create(d.context, d.allocator, d.timeline, options, why, sizeof why) && display.SetOffscreen({16, 16}) &&
                  Frame(display, Argb(4, 5, 6)) == FrameEnd::Drawn,
              "and draws again");
        display.Destroy();
        Release(d);
    }

    struct HeadlessUser
    {
        int made = 0;
    };

    VkSurfaceKHR MakeHeadless(VkInstance instance, void* user, char* why, uint32_t why_size)
    {
        ++static_cast<HeadlessUser*>(user)->made;
        return vk::present::CreateHeadlessSurface(instance, why, why_size);
    }

    void SwapchainCases()
    {
        Device d;
        if (!Make(d, true))
        {
            std::printf("SKIP the swapchain on a headless surface: the driver has no VK_EXT_headless_surface\n");
            return;
        }
        if (!d.context.Chosen().extensions.swapchain)
        {
            std::printf("SKIP the swapchain on a headless surface: the device has no VK_KHR_swapchain\n");
            Release(d);
            return;
        }
        char             why[256] = {};
        Display          display;
        Display::Options options;
        Check(display.Create(d.context, d.allocator, d.timeline, options, why, sizeof why), "a display for the swapchain");
        HeadlessUser user;
        Check(display.SetSurface(&MakeHeadless, &user, why, sizeof why), "a headless surface");
        display.SetWindowExtent({64, 48});
        int presented = 0;
        for (int i = 0; i < 8; ++i)
            if (Frame(display, Argb(10, 20, 30)) == FrameEnd::Presented) ++presented;
        const vk::present::Swapchain& chain = display.Chain();
        std::printf("  swapchain %ux%u, %u images, %s, format %d; %d of 8 presented\n", chain.Extent().width, chain.Extent().height,
                    chain.ImageCount(), vk::present::PresentModeName(chain.Mode()), static_cast<int>(chain.Format()), presented);
        Check(presented == 8 && chain.Extent().width == 64 && chain.Mode() == VK_PRESENT_MODE_FIFO_KHR,
              "frames are presented on a FIFO swapchain of the window's size");

        const uint64_t rebuilds = display.Counters().rebuilds;
        display.SetWindowExtent({80, 60});
        Check(Frame(display, Argb(10, 20, 30)) == FrameEnd::Presented && chain.Extent().width == 80 && chain.Extent().height == 60 &&
                  display.Counters().rebuilds == rebuilds + 1,
              "a resized window rebuilds the swapchain at its new size, and the frame is shown");

        display.SetSync(vk::present::Sync::Off);
        Frame(display, Argb(10, 20, 30));
        std::printf("  vsync off: %s\n", vk::present::PresentModeName(chain.Mode()));
        Check(chain.Mode() == VK_PRESENT_MODE_MAILBOX_KHR || chain.Mode() == VK_PRESENT_MODE_IMMEDIATE_KHR ||
                  chain.Mode() == VK_PRESENT_MODE_FIFO_KHR,
              "vsync off takes the fastest mode the surface offers");

        display.SetWindowExtent({0, 0});
        Check(Frame(display, Argb(10, 20, 30)) == FrameEnd::Drawn && !chain.Ready(), "a minimised window shows nothing and keeps no swapchain");
        display.SetWindowExtent({64, 48});
        Check(Frame(display, Argb(10, 20, 30)) == FrameEnd::Presented, "the window back, frames are shown again");

        // The surface made anew (a new window, or the surface lost): the old one retired, a new one made.
        Check(display.SetSurface(&MakeHeadless, &user, why, sizeof why) && Frame(display, Argb(1, 1, 1)) == FrameEnd::Presented &&
                  user.made == 2,
              "a surface made anew presents");
        display.Destroy();
        Check(display.Pending() == 0, "nothing is left to release");
        Release(d);
    }

    void ImGuiCases(Device& d)
    {
        const uint32_t baseline = d.allocator.Allocations();
        char           why[256] = {};
        Display        display;
        Display::Options options;
        if (!display.Create(d.context, d.allocator, d.timeline, options, why, sizeof why) || !display.SetOffscreen({128, 64}))
        {
            Check(false, "a display for Dear ImGui");
            return;
        }
        ImGuiContext* imgui = ImGui::CreateContext();
        ImGuiIO&      io    = ImGui::GetIO();
        io.IniFilename      = nullptr;
        io.DisplaySize      = ImVec2(128.0f, 64.0f);
        io.DeltaTime        = 1.0f / 60.0f;

        vk::imgui::Renderer renderer;
        Check(renderer.Create(d.context, d.allocator, Display::kSceneFormat, 1, 2, why, sizeof why),
              "Dear ImGui's Vulkan backend starts on the renderer's device");

        std::vector<uint32_t> pixels;
        const uint32_t        green = Argb(0, 255, 0);
        for (int pass = 0; pass < 3; ++pass)
        {
            vk::frame::Wait wait{};
            if (!display.BeginFrame(wait)) break;
            ClearScene(display, green, {{0, 0}, {128, 64}});
            display.EndRendering();
            renderer.NewFrame(display.Cmd(), [&](std::function<void()> release) { display.AfterFrame(std::move(release)); });
            ImGui::NewFrame();
            ImDrawList* draw = ImGui::GetForegroundDrawList();
            draw->AddRectFilled(ImVec2(8, 8), ImVec2(40, 30), IM_COL32(255, 0, 0, 255));
            draw->AddRectFilled(ImVec2(50, 8), ImVec2(90, 30), IM_COL32(0, 0, 255, 128));
            draw->AddText(ImVec2(8, 40), IM_COL32(255, 255, 255, 255), "WXL 123");
            ImGui::Render();

            const VkCommandBuffer cmd = display.BeginColorRendering();
            renderer.Record(cmd, ImGui::GetDrawData());
            display.EndRendering();
            display.EndFrame();
        }
        Check(display.ReadOffscreen(pixels) && pixels.size() == 128 * 64, "a frame of Dear ImGui reads back");
        Save("imgui", pixels, 128, 64);
        if (pixels.size() == 128 * 64)
        {
            Check(pixels[20 * 128 + 20] == Argb(255, 0, 0) && pixels[8 * 128 + 8] == Argb(255, 0, 0) && pixels[29 * 128 + 39] == Argb(255, 0, 0) &&
                      pixels[7 * 128 + 8] == green && pixels[20 * 128 + 40] == green,
                  "an opaque rectangle covers exactly its pixels");
            const uint32_t blended = pixels[20 * 128 + 70];
            std::printf("  half-transparent blue over green: %08x\n", blended);
            Check(Near(blended, Argb(0, 127, 128), 1), "a half-transparent rectangle blends with the scene beneath (source alpha)");
            int lit = 0;
            for (uint32_t y = 40; y < 56; ++y)
                for (uint32_t x = 8; x < 64; ++x)
                    if (pixels[y * 128 + x] != green) ++lit;
            std::printf("  text: %d pixels drawn\n", lit);
            Check(lit > 40, "the font atlas was uploaded and text is drawn with it");
        }

        display.Destroy(); // waits for the frames, so the backend's buffers are free
        renderer.Destroy();
        ImGui::DestroyContext(imgui);
        Check(d.allocator.Allocations() == baseline, "Dear ImGui's atlas and the display leave nothing allocated");
    }
}

namespace wxl::vk::test
{
    void PresentPureCases()
    {
        ChoiceCases();
        LimiterCases();
        GraveyardCases();
        SpirvCases();
    }

    bool PresentLiveCases()
    {
        Device d;
        if (!Make(d, false))
        {
            Check(false, "a device for the present cases");
            return false;
        }
        PassCases(d);
        LoopCases(d);
        ImGuiCases(d);
        Release(d);
        LossCases();
        SwapchainCases();
        return true;
    }
}
