// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A frame capture drawn again through the Vulkan renderer: its shaders, pools, buffers and textures
// made on the device, its device state read at each clear and draw as the live client's is
// (gxfront/Engine.hpp), its draws recorded into the open frame of the renderer's display. What
// the D3D9 frame looked like (its SHOT) is kept, to be compared with what the renderer drew.
// Portable: the tests and the replay tool run it natively on lavapipe.

#pragma once

#include "gx/capture/Capture.hpp"
#include "gx/vk/draw/Draw.hpp"
#include "gx/vk/gxfront/Engine.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::gx::capture
{
    class Replay final : public wxl::vk::gxfront::Resolver
    {
    public:
        struct Stats
        {
            uint32_t draws = 0;
            uint32_t verdicts[6] = {}; ///< by gxfront::Verdict
            uint32_t recorded = 0;     ///< the drawn ones the renderer recorded
            uint32_t clears = 0;
            uint32_t copies = 0;       ///< TexCopy/TexStretch, not replayed at this step
            uint32_t textures = 0;
            uint32_t pools = 0;
            uint32_t shaders = 0;
            uint32_t programs = 0;     ///< shaders the renderer has a program for
            uint32_t unknown = 0;      ///< pool ranges the capture could not read back
        };

        Replay() = default;
        Replay(const Replay&)            = delete;
        Replay& operator=(const Replay&) = delete;
        ~Replay() { Release(); }

        /**
         * @brief Draws the capture @p data into @p renderer's display, whose frame must be open and
         *        whose scene the capture's size.
         * @return false with @p why when the capture cannot be replayed on this build (another
         *         layout, a malformed chunk); its draws that cannot be drawn yet are counted, not refused.
         */
        bool Run(const uint8_t* data, size_t size, wxl::vk::draw::Renderer& renderer, std::string& why);

        /// What the renderer made for the capture goes (its textures and pools).
        void Release();

        const Stats& Counters() const { return stats_; }
        /// The D3D9 back buffer of the capture (0xAARRGGBB), and its size; empty without a SHOT.
        const std::vector<uint32_t>& Shot() const { return shot_; }
        uint32_t ShotWidth() const { return shot_width_; }
        uint32_t ShotHeight() const { return shot_height_; }

        // gxfront::Resolver
        const wxl::engine::CGxBuf* Buffer(const void* buf) override;
        wxl::vk::draw::Pool*       Pool(const void* pool) override;
        wxl::vk::draw::Texture*    Texture(const void* texture, const wxl::engine::CGxTex*& fields) override;
        int                        Program(const void* shader, bool vertex) override;

    private:
        bool Shader(const Chunk& chunk, std::string& why);
        bool PoolChunk(const Chunk& chunk, std::string& why);
        bool Data(const Chunk& chunk, std::string& why);
        bool BufferChunk(const Chunk& chunk, std::string& why);
        bool TextureChunk(const Chunk& chunk, std::string& why);
        bool Constants(const Chunk& chunk, std::string& why);
        bool Clear(const Reader& reader, const Chunk& chunk, std::string& why);
        bool Draw(const Reader& reader, const Chunk& chunk, std::string& why);
        bool ShotChunk(const Chunk& chunk, std::string& why);

        struct TextureEntry
        {
            wxl::engine::CGxTex     fields{};
            wxl::vk::draw::Texture* texture = nullptr;
        };
        struct PoolEntry
        {
            wxl::vk::draw::Pool* pool = nullptr;
            uint32_t             written = 0; ///< a stream pool's end of writing: an earlier write wraps it
        };

        wxl::vk::draw::Renderer*                          renderer_ = nullptr;
        std::unordered_map<uint64_t, wxl::engine::CGxBuf> buffers_;
        std::unordered_map<uint64_t, PoolEntry>           pools_;
        std::unordered_map<uint64_t, TextureEntry>        textures_;
        std::unordered_map<uint64_t, int>                 programs_[2]; ///< [pixel, vertex]
        float                                             vertex_[256 * 4] = {};
        float                                             pixel_[256 * 4] = {};
        std::vector<uint32_t>                             shot_;
        uint32_t                                          shot_width_ = 0;
        uint32_t                                          shot_height_ = 0;
        Stats                                             stats_;
    };

    struct Comparison
    {
        uint64_t pixels = 0;
        uint64_t matching = 0;  ///< every channel within the tolerance
        uint32_t worst = 0;     ///< the largest channel difference seen
    };

    /// @p a against @p b (0xAARRGGBB, the same size), rgb only (D3D9's back buffer alpha is not
    /// shown); @p diff (when given) gets each pixel's largest difference as grey.
    Comparison Compare(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, uint32_t tolerance,
                       std::vector<uint32_t>* diff = nullptr);
}
