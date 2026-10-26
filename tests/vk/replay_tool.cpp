// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A frame capture of the recompiled client (gxCapture, under D3D9) drawn again through the Vulkan
// renderer on this machine's driver, and compared with the D3D9 back buffer the capture holds:
//
//   wxl-vk-replay <capture folder> [<output folder>] [tolerance]
//
// Writes into the output folder (the capture's when not given) vulkan.ppm (what the renderer
// drew), d3d9.ppm (the capture's SHOT) and diff.ppm (each pixel's largest channel difference as
// grey), and prints what was replayed, what was left out and why, and how many pixels match.
// Exit 0 when the replay ran, 1 when it could not, 77 without a Vulkan 1.3 device.

#include "gx/capture/Replay.hpp"
#include "gx/vk/Log.hpp"
#include "gx/vk/draw/Draw.hpp"
#include "gx/vk/present/Display.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    namespace vk = wxl::vk;

    void Print(vk::LogLevel level, const char* text, void*)
    {
        if (level != vk::LogLevel::Debug) std::printf("  [vk] %s\n", text);
    }

    void Save(const std::string& path, const std::vector<uint32_t>& pixels, uint32_t w, uint32_t h)
    {
        std::ofstream out(path, std::ios::binary);
        out << "P6\n" << w << " " << h << "\n255\n";
        for (uint32_t p : pixels)
        {
            const char rgb[3] = {static_cast<char>((p >> 16) & 0xFF), static_cast<char>((p >> 8) & 0xFF), static_cast<char>(p & 0xFF)};
            out.write(rgb, 3);
        }
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: wxl-vk-replay <capture folder> [<output folder>] [tolerance]\n");
        return 1;
    }
    vk::SetLogSink(Print, nullptr);
    const std::string folder    = argv[1];
    const std::string output    = argc > 2 ? argv[2] : folder;
    const uint32_t    tolerance = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 8;
    std::ifstream     in(folder + "/frame.wxlcap", std::ios::binary);
    if (!in)
    {
        std::printf("no %s/frame.wxlcap\n", folder.c_str());
        return 1;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    wxl::gx::capture::Reader   peek;
    std::string                why;
    if (!peek.Open(bytes.data(), bytes.size(), why))
    {
        std::printf("not a capture: %s\n", why.c_str());
        return 1;
    }
    const uint32_t w = peek.Header().width, h = peek.Header().height;

    char                     reason[256] = {};
    vk::device::Context      context;
    vk::resources::Allocator allocator;
    vk::frame::Timeline      timeline;
    vk::device::Options      options;
    options.floor.present = false;
    if (!context.Create(options, reason, sizeof reason) || !allocator.Create(context, reason, sizeof reason) ||
        !timeline.Create(context, reason, sizeof reason))
    {
        std::printf("no Vulkan device: %s\n", reason);
        return 77;
    }
    int code = 1;
    {
        vk::present::Display          display;
        vk::present::Display::Options d;
        d.filter = vk::present::Filter::Nearest;
        vk::draw::Renderer renderer;
        if (display.Create(context, allocator, timeline, d, reason, sizeof reason) && display.SetOffscreen({w, h}))
        {
            display.SetSceneExtent({w, h});
            vk::frame::Wait wait{};
            if (renderer.Create(context, allocator, display, reason, sizeof reason) && display.BeginFrame(wait))
            {
                wxl::gx::capture::Replay replay;
                const bool               ran = replay.Run(bytes.data(), bytes.size(), renderer, why);
                display.EndFrame();
                std::vector<uint32_t> pixels;
                display.ReadOffscreen(pixels);
                const auto& s = replay.Counters();
                std::printf("%ux%u: %u shaders (%u the renderer draws), %u pools, %u textures, %u clears, %u copies not replayed, "
                            "%u pool ranges unknown\n",
                            w, h, s.shaders, s.programs, s.pools, s.textures, s.clears, s.copies, s.unknown);
                std::printf("%u draws: %u drawn (%u recorded); left out: %u into targets, %u in the world, %u 3D, %u client programs, "
                            "%u not ready\n",
                            s.draws, s.verdicts[0], s.recorded, s.verdicts[1], s.verdicts[2], s.verdicts[3], s.verdicts[4], s.verdicts[5]);
                if (!ran) std::printf("the replay stopped: %s\n", why.c_str());
                Save(output + "/vulkan.ppm", pixels, w, h);
                if (!replay.Shot().empty() && replay.ShotWidth() == w && replay.ShotHeight() == h)
                {
                    std::vector<uint32_t>             diff;
                    const wxl::gx::capture::Comparison c = wxl::gx::capture::Compare(pixels, replay.Shot(), tolerance, &diff);
                    Save(output + "/d3d9.ppm", replay.Shot(), w, h);
                    Save(output + "/diff.ppm", diff, w, h);
                    std::printf("%llu of %llu pixels within %u of D3D9's (%.1f%%), the worst %u off\n",
                                static_cast<unsigned long long>(c.matching), static_cast<unsigned long long>(c.pixels), tolerance,
                                c.pixels ? 100.0 * static_cast<double>(c.matching) / static_cast<double>(c.pixels) : 0.0, c.worst);
                }
                replay.Release();
                code = ran ? 0 : 1;
            }
            else
                std::printf("no renderer: %s\n", reason);
        }
        else
            std::printf("no display: %s\n", reason);
        if (timeline.Last()) timeline.WaitFor(timeline.Last());
    }
    timeline.Destroy();
    allocator.Destroy();
    context.Destroy();
    return code;
}
