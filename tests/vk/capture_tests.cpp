// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The frame capture's reader (src/gx/capture/) on captures made up here in the format the client
// writes (wxl/core/gx_capture.h): the header refused when it is not one, the regions whole then
// patched by their deltas, the events in order with the state they had, the padding, a file cut
// short.

#include "gx/capture/Capture.hpp"

#include "Check.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace
{
    using wxl::vk::test::Check;
    namespace capture = wxl::gx::capture;

    struct Writer
    {
        std::vector<uint8_t> bytes;

        void Put(const void* data, size_t size)
        {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            bytes.insert(bytes.end(), p, p + size);
        }
        void Put32(uint32_t v) { Put(&v, 4); }
        void Put64(uint64_t v) { Put(&v, 8); }
        size_t Begin(uint32_t id)
        {
            Put32(id);
            Put32(0);
            return bytes.size();
        }
        void End(size_t start)
        {
            const uint32_t size = static_cast<uint32_t>(bytes.size() - start);
            std::memcpy(&bytes[start - 4], &size, 4);
            while (bytes.size() % 4) bytes.push_back(0);
        }
        void Mem(uint32_t region, uint32_t total, std::initializer_list<std::pair<uint32_t, std::string>> runs)
        {
            const size_t start = Begin(WXL_GX_CAPTURE_MEM);
            Put32(region);
            Put32(total);
            Put32(static_cast<uint32_t>(runs.size()));
            for (const auto& run : runs)
            {
                Put32(run.first);
                Put32(static_cast<uint32_t>(run.second.size()));
                Put(run.second.data(), run.second.size());
            }
            End(start);
        }
    };

    wxl_gx_capture_header Header()
    {
        wxl_gx_capture_header h{};
        h.magic       = WXL_GX_CAPTURE_MAGIC;
        h.version     = WXL_GX_CAPTURE_VERSION;
        h.header_size = sizeof h;
        h.pointer_size = 8;
        h.device_size = 8;
        h.width       = 4;
        h.height      = 2;
        h.api         = 1;
        return h;
    }
}

namespace wxl::vk::test
{
    void CaptureCases()
    {
        std::string     why;
        capture::Reader reader;
        const uint8_t   junk[64] = {1, 2, 3};
        Check(!reader.Open(junk, sizeof junk, why) && why.find("XCAP") != std::string::npos, "a file that is not a capture is refused");
        wxl_gx_capture_header future = Header();
        future.version               = 99;
        Check(!reader.Open(reinterpret_cast<const uint8_t*>(&future), sizeof future, why) && why.find("version 99") != std::string::npos,
              "a capture of another version is refused, its version named");

        Writer w;
        const wxl_gx_capture_header h = Header();
        w.Put(&h, sizeof h);
        w.Mem(WXL_GX_CAPTURE_REGION_DEVICE, 8, {{0, "ABCDEFGH"}});
        size_t c = w.Begin(WXL_GX_CAPTURE_CLEAR);
        w.Put32(3);
        w.Put32(0xFF102030u);
        w.End(c);
        w.Mem(WXL_GX_CAPTURE_REGION_DEVICE, 8, {{2, "xy"}, {7, "!"}});
        c = w.Begin(WXL_GX_CAPTURE_SHADER);
        w.Put64(0x1234);
        w.Put32(0);
        w.Put32(3);
        w.Put("UI0", 3); // three bytes: the chunk is padded to four
        w.Put32(0);
        w.End(c);
        c = w.Begin(WXL_GX_CAPTURE_DRAW);
        const uint8_t batch[16] = {3, 0, 0, 0, 0, 0, 0, 0, 6, 0, 0, 0, 0, 0, 3, 0};
        w.Put(batch, 16);
        w.Put32(1);
        w.End(c);
        c = w.Begin(WXL_GX_CAPTURE_SHOT);
        w.Put32(4);
        w.Put32(2);
        for (int i = 0; i < 8; ++i) w.Put32(0xFF000000u | static_cast<uint32_t>(i));
        w.End(c);
        c = w.Begin(WXL_GX_CAPTURE_END);
        w.End(c);

        Check(reader.Open(w.bytes.data(), w.bytes.size(), why) && reader.Header().width == 4, "a capture's header is read");
        capture::Chunk chunk;
        Check(reader.Next(chunk, why) && chunk.id == WXL_GX_CAPTURE_CLEAR, "the first event is the clear");
        const std::vector<uint8_t>& device = reader.Region(WXL_GX_CAPTURE_REGION_DEVICE);
        Check(device.size() == 8 && std::memcmp(device.data(), "ABCDEFGH", 8) == 0, "at the clear the device region is whole");
        uint32_t colour = 0;
        Check(capture::Read32(chunk, 4, colour) && colour == 0xFF102030u, "the clear's colour is read");
        Check(reader.Next(chunk, why) && chunk.id == WXL_GX_CAPTURE_SHADER && chunk.size == 8 + 4 + 4 + 3 + 4,
              "a chunk of an odd size is read whole, its padding skipped");
        Check(std::memcmp(reader.Region(WXL_GX_CAPTURE_REGION_DEVICE).data(), "ABxyEFG!", 8) == 0 && reader.Deltas() == 2,
              "a delta patches only its runs");
        Check(reader.Next(chunk, why) && chunk.id == WXL_GX_CAPTURE_DRAW && chunk.size == 20, "after the padding, the draw");
        Check(reader.Next(chunk, why) && chunk.id == WXL_GX_CAPTURE_SHOT && chunk.size == 8 + 32, "the back buffer");
        Check(reader.Next(chunk, why) && chunk.id == WXL_GX_CAPTURE_END && !reader.Next(chunk, why) && why.empty(),
              "the end, then nothing more");

        std::vector<uint8_t> cut(w.bytes.begin(), w.bytes.begin() + static_cast<long>(sizeof h + 20));
        Check(reader.Open(cut.data(), cut.size(), why) && !reader.Next(chunk, why) && !why.empty(), "a file cut inside a chunk says so");
        Writer bad;
        bad.Put(&h, sizeof h);
        bad.Mem(WXL_GX_CAPTURE_REGION_DEVICE, 4, {{2, "xyz"}});
        Check(reader.Open(bad.bytes.data(), bad.bytes.size(), why) && !reader.Next(chunk, why) &&
                  why.find("outside") != std::string::npos,
              "a run past its region is refused");
    }
}
