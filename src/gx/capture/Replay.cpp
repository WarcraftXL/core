// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A frame capture drawn again through the Vulkan renderer.

#include "gx/capture/Replay.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wxl::gx::capture
{
    namespace vk     = wxl::vk;
    namespace engine = wxl::engine;

    namespace
    {
        bool Fail(std::string& why, const char* text)
        {
            why = text;
            return false;
        }

        uint64_t Id(const void* pointer) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer)); }
    }

    void Replay::Release()
    {
        if (renderer_)
        {
            for (auto& t : textures_) renderer_->DestroyTexture(t.second.texture);
            for (auto& p : pools_) renderer_->DestroyPool(p.second.pool);
        }
        textures_.clear();
        pools_.clear();
        buffers_.clear();
        programs_[0].clear();
        programs_[1].clear();
        renderer_ = nullptr;
    }

    const engine::CGxBuf* Replay::Buffer(const void* buf)
    {
        const auto found = buffers_.find(Id(buf));
        return found == buffers_.end() ? nullptr : &found->second;
    }

    vk::draw::Pool* Replay::Pool(const void* pool)
    {
        const auto found = pools_.find(Id(pool));
        return found == pools_.end() ? nullptr : found->second.pool;
    }

    vk::draw::Texture* Replay::Texture(const void* texture, const engine::CGxTex*& fields)
    {
        const auto found = textures_.find(Id(texture));
        if (found == textures_.end())
        {
            fields = nullptr;
            return nullptr;
        }
        fields = &found->second.fields;
        return found->second.texture;
    }

    int Replay::Program(const void* shader, bool vertex)
    {
        const auto& programs = programs_[vertex ? 1 : 0];
        const auto  found    = programs.find(Id(shader));
        return found == programs.end() ? -1 : found->second;
    }

    bool Replay::Run(const uint8_t* data, size_t size, vk::draw::Renderer& renderer, std::string& why)
    {
        Release();
        stats_    = Stats{};
        renderer_ = &renderer;
        shot_.clear();
        Reader reader;
        if (!reader.Open(data, size, why)) return false;
        const wxl_gx_capture_header& h = reader.Header();
        if (h.pointer_size != sizeof(void*) || h.device_layout_hash != engine::CGxDevice::kLayoutHash ||
            h.device_size != sizeof(engine::CGxDevice) || h.tex_size != sizeof(engine::CGxTex) || h.buf_size != sizeof(engine::CGxBuf) ||
            h.state_size != sizeof(wxl_gx_app_state))
            return Fail(why, "the capture was made by a client of another layout than this build reads");
        if (!renderer.Screen().InFrame()) return Fail(why, "no frame is open on the renderer's display");

        Chunk chunk;
        while (reader.Next(chunk, why))
        {
            bool ok = true;
            switch (chunk.id)
            {
            case WXL_GX_CAPTURE_SHADER: ok = Shader(chunk, why); break;
            case WXL_GX_CAPTURE_POOL: ok = PoolChunk(chunk, why); break;
            case WXL_GX_CAPTURE_DATA: ok = Data(chunk, why); break;
            case WXL_GX_CAPTURE_BUFFER: ok = BufferChunk(chunk, why); break;
            case WXL_GX_CAPTURE_TEXTURE: ok = TextureChunk(chunk, why); break;
            case WXL_GX_CAPTURE_CONSTANTS: ok = Constants(chunk, why); break;
            case WXL_GX_CAPTURE_CLEAR: ok = Clear(reader, chunk, why); break;
            case WXL_GX_CAPTURE_DRAW: ok = Draw(reader, chunk, why); break;
            case WXL_GX_CAPTURE_COPY: ++stats_.copies; break;
            case WXL_GX_CAPTURE_SHOT: ok = ShotChunk(chunk, why); break;
            default: break;
            }
            if (!ok) return false;
            if (chunk.id == WXL_GX_CAPTURE_END) break;
        }
        return why.empty();
    }

    bool Replay::Shader(const Chunk& chunk, std::string& why)
    {
        uint64_t id = 0;
        uint32_t target = 0, length = 0;
        if (!Read64(chunk, 0, id) || !Read32(chunk, 8, target) || !Read32(chunk, 12, length) || 16ull + length > chunk.size)
            return Fail(why, "a malformed SHDR chunk");
        const std::string name(reinterpret_cast<const char*>(chunk.body + 16), length);
        ++stats_.shaders;
        const bool vertex = target == 0; // EGxShTarget: 0 vertex, 4 pixel
        uint8_t    program = 0;
        if (vk::draw::ProgramByName(vertex, name.c_str(), program))
        {
            programs_[vertex ? 1 : 0][id] = program;
            ++stats_.programs;
        }
        return true;
    }

    bool Replay::PoolChunk(const Chunk& chunk, std::string& why)
    {
        uint64_t id = 0;
        uint32_t target = 0, usage = 0, size = 0;
        if (!Read64(chunk, 0, id) || !Read32(chunk, 8, target) || !Read32(chunk, 12, usage) || !Read32(chunk, 16, size))
            return Fail(why, "a malformed POOL chunk");
        if (pools_.count(id)) return true;
        PoolEntry entry;
        entry.pool = renderer_->CreatePool(target, usage, size);
        pools_[id] = entry;
        ++stats_.pools;
        return true;
    }

    bool Replay::Data(const Chunk& chunk, std::string& why)
    {
        uint64_t id = 0;
        uint32_t offset = 0, size = 0, known = 0;
        if (!Read64(chunk, 0, id) || !Read32(chunk, 8, offset) || !Read32(chunk, 12, size) || !Read32(chunk, 16, known) ||
            20ull + size > chunk.size)
            return Fail(why, "a malformed DATA chunk");
        const auto found = pools_.find(id);
        if (found == pools_.end()) return Fail(why, "a DATA chunk for a pool the capture did not declare");
        PoolEntry&      entry = found->second;
        vk::draw::Pool* pool  = entry.pool;
        if (!known)
        {
            ++stats_.unknown;
            return true;
        }
        if (pool->usage == vk::draw::kStream)
        {
            // The engine writes a stream pool forward and starts it again at 0 when it is full:
            // a write before the last one's end is that new start.
            if (offset < entry.written) renderer_->DiscardPool(pool);
            entry.written = offset + size;
        }
        uint8_t* memory = renderer_->PoolMemory(pool);
        if (!memory || static_cast<uint64_t>(offset) + size > pool->size) return true;
        std::memcpy(memory + offset, chunk.body + 20, size);
        renderer_->PoolWritten(pool, offset, size);
        return true;
    }

    bool Replay::BufferChunk(const Chunk& chunk, std::string& why)
    {
        uint64_t id = 0;
        if (!Read64(chunk, 0, id) || 16ull + sizeof(engine::CGxBuf) > chunk.size) return Fail(why, "a malformed BUF chunk");
        std::memcpy(&buffers_[id], chunk.body + 16, sizeof(engine::CGxBuf));
        return true;
    }

    bool Replay::TextureChunk(const Chunk& chunk, std::string& why)
    {
        uint64_t id = 0;
        if (!Read64(chunk, 0, id) || 8ull + sizeof(engine::CGxTex) + 4 > chunk.size) return Fail(why, "a malformed TEX chunk");
        TextureEntry& entry = textures_[id];
        std::memcpy(&entry.fields, chunk.body + 8, sizeof(engine::CGxTex));
        uint32_t count = 0;
        uint32_t at    = 8 + static_cast<uint32_t>(sizeof(engine::CGxTex));
        Read32(chunk, at, count);
        at += 4;

        // The levels first: the largest says the texture's size, their count its levels.
        struct Level
        {
            uint32_t face, mip, width, height, pitch, format, size;
            const uint8_t* bytes;
        };
        std::vector<Level> levels;
        uint32_t           width = entry.fields.m_width, height = entry.fields.m_height, mips = 1;
        for (uint32_t i = 0; i < count; ++i)
        {
            Level l{};
            if (!Read32(chunk, at, l.face) || !Read32(chunk, at + 4, l.mip) || !Read32(chunk, at + 8, l.width) ||
                !Read32(chunk, at + 12, l.height) || !Read32(chunk, at + 16, l.pitch) || !Read32(chunk, at + 20, l.format) ||
                !Read32(chunk, at + 24, l.size) || static_cast<uint64_t>(at) + 28 + l.size > chunk.size)
                return Fail(why, "a malformed level in a TEX chunk");
            l.bytes = chunk.body + at + 28;
            at += 28 + l.size;
            if (l.mip == 0)
            {
                width  = l.width;
                height = l.height;
            }
            mips = std::max(mips, l.mip + 1);
            levels.push_back(l);
        }

        if (!entry.texture)
        {
            vk::draw::TextureDesc desc;
            desc.format        = entry.fields.m_format;
            desc.width         = std::max(width, 1u);
            desc.height        = std::max(height, 1u);
            desc.levels        = mips;
            desc.cube          = entry.fields.m_target == 1;
            desc.target        = entry.fields.m_flags.m_renderTarget != 0;
            desc.generate_mips = entry.fields.m_flags.m_generateMipMaps != 0;
            entry.texture      = renderer_->CreateTexture(desc);
            if (entry.texture) ++stats_.textures;
        }
        if (!entry.texture) return true;
        for (const Level& l : levels)
        {
            uint32_t pitch = 0;
            uint8_t* to    = renderer_->LockTexture(entry.texture, l.face, l.mip, 0, 0, l.width, l.height, pitch);
            if (!to) continue;
            const uint32_t rows = vk::draw::Formats::Rows(entry.texture->info, l.height);
            for (uint32_t r = 0; r < rows && static_cast<uint64_t>(r) * l.pitch < l.size; ++r)
                std::memcpy(to + static_cast<size_t>(r) * pitch, l.bytes + static_cast<size_t>(r) * l.pitch,
                            std::min(pitch, std::min(l.pitch, l.size - r * l.pitch)));
            renderer_->UnlockTexture(entry.texture);
        }
        return true;
    }

    bool Replay::Constants(const Chunk& chunk, std::string& why)
    {
        uint32_t target = 0, first = 0, rows = 0;
        if (!Read32(chunk, 0, target) || !Read32(chunk, 4, first) || !Read32(chunk, 8, rows) || 12ull + rows * 16ull > chunk.size)
            return Fail(why, "a malformed CONS chunk");
        if (first >= 256) return true;
        rows = std::min(rows, 256 - first);
        std::memcpy((target == 0 ? vertex_ : pixel_) + first * 4, chunk.body + 12, rows * 16);
        return true;
    }

    bool Replay::Clear(const Reader& reader, const Chunk& chunk, std::string& why)
    {
        uint32_t mask = 0, argb = 0;
        if (!Read32(chunk, 0, mask) || !Read32(chunk, 4, argb)) return Fail(why, "a malformed CLR chunk");
        const std::vector<uint8_t>& device = reader.Region(WXL_GX_CAPTURE_REGION_DEVICE);
        if (device.size() != sizeof(engine::CGxDevice)) return Fail(why, "a clear before the device's state");
        const auto& d = *reinterpret_cast<const engine::CGxDevice*>(device.data());
        ++stats_.clears;
        if (d.m_textureTarget[0].m_texture || d.m_textureTarget[1].m_texture) return true;
        int32_t rect[4];
        vk::gxfront::ClearRect(d, rect);
        const float rgba[4] = {static_cast<float>((argb >> 16) & 0xFF) / 255.0f, static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
                               static_cast<float>(argb & 0xFF) / 255.0f, static_cast<float>(argb >> 24) / 255.0f};
        if (rect[2] > 0 && rect[3] > 0)
            renderer_->Screen().Clear((mask & 1) != 0, (mask & 2) != 0, rgba, 1.0f,
                                      VkRect2D{{rect[0], rect[1]}, {static_cast<uint32_t>(rect[2]), static_cast<uint32_t>(rect[3])}});
        return true;
    }

    bool Replay::Draw(const Reader& reader, const Chunk& chunk, std::string& why)
    {
        wxl_gx_batch batch{};
        uint32_t     count = 0;
        if (chunk.size < sizeof batch + 4) return Fail(why, "a malformed DRAW chunk");
        std::memcpy(&batch, chunk.body, sizeof batch);
        Read32(chunk, sizeof batch, count);
        const std::vector<uint8_t>& device = reader.Region(WXL_GX_CAPTURE_REGION_DEVICE);
        const std::vector<uint8_t>& states = reader.Region(WXL_GX_CAPTURE_REGION_STATES);
        if (device.size() != sizeof(engine::CGxDevice) || states.size() < sizeof(wxl_gx_app_state) * vk::gxfront::kRenderStates)
            return Fail(why, "a draw before the device's state");
        ++stats_.draws;
        vk::gxfront::EngineView view;
        view.device           = reinterpret_cast<const engine::CGxDevice*>(device.data());
        view.states           = reinterpret_cast<const wxl_gx_app_state*>(states.data());
        view.vertex_constants = vertex_;
        vk::draw::Packet             packet;
        const vk::gxfront::Verdict verdict = vk::gxfront::BuildDraw(view, batch, static_cast<int32_t>(count), *this, packet);
        ++stats_.verdicts[static_cast<int>(verdict)];
        if (verdict == vk::gxfront::Verdict::Draw && renderer_->Draw(packet)) ++stats_.recorded;
        return true;
    }

    bool Replay::ShotChunk(const Chunk& chunk, std::string& why)
    {
        uint32_t width = 0, height = 0;
        if (!Read32(chunk, 0, width) || !Read32(chunk, 4, height) || 8ull + 4ull * width * height > chunk.size)
            return Fail(why, "a malformed SHOT chunk");
        shot_width_  = width;
        shot_height_ = height;
        shot_.resize(static_cast<size_t>(width) * height);
        std::memcpy(shot_.data(), chunk.body + 8, shot_.size() * 4);
        return true;
    }

    Comparison Compare(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, uint32_t tolerance, std::vector<uint32_t>* diff)
    {
        Comparison c;
        const size_t n = std::min(a.size(), b.size());
        if (diff) diff->assign(n, 0xFF000000u);
        for (size_t i = 0; i < n; ++i)
        {
            uint32_t worst = 0;
            for (int shift = 0; shift < 24; shift += 8)
            {
                const int32_t x = static_cast<int32_t>((a[i] >> shift) & 0xFF) - static_cast<int32_t>((b[i] >> shift) & 0xFF);
                worst           = std::max<uint32_t>(worst, static_cast<uint32_t>(x < 0 ? -x : x));
            }
            ++c.pixels;
            if (worst <= tolerance) ++c.matching;
            c.worst = std::max(c.worst, worst);
            if (diff) (*diff)[i] = 0xFF000000u | worst << 16 | worst << 8 | worst;
        }
        return c;
    }
}
