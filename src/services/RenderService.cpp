// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.render and wxl.vk over the runtime's Vulkan renderer: the mods' handles of the render
// module's images, buffers and pipelines (gx/vk/render/Render.hpp), owned by the mod that made them
// and given back when it goes; the points of the frame, raised on every device, inside which the
// passes and draws are taken; the batches ModelBatchDrawn hands over and the copies mods keep; the
// device's own objects for wxl.vk. On a device the renderer does not drive (Direct3D 9) every
// member answers that it is not supported, and the points are still raised.

#include "services/RenderService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "gx/Vulkan.hpp"
#include "gx/vk/gxfront/Front.hpp"
#include "runtime/Core.hpp"
#include "runtime/Frame.hpp"

#include "wxl/host/mod_host.hpp"
#include "wxl/seams/seams.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace wxl::core;
    namespace vkr = wxl::vk::render;

    constexpr char kRenderTag[] = "render";

    enum class Kind : uint8_t
    {
        Image,
        Buffer,
        Pipeline,
    };

    struct Held
    {
        Kind     kind;
        uint32_t mod;
        void*    object;
    };

    std::unordered_map<wxl_handle, Held> g_handles;
    wxl_handle                           g_next     = 16; ///< 1 and 2 name the scene's attachments
    bool                                 g_in_point = false;
    bool                                 g_point_zoned = false; ///< the point's own GPU zone is open
    uint32_t                             g_point_pass  = ~0u;   ///< the point's pass timed on the GPU (Display::PassBegin)
    uint64_t                             g_point_serial = 0; ///< one per point raised: the scene's copies are made once in each

    /// The batches mods kept this frame: stable addresses until the frame ends.
    std::deque<wxl::vk::draw::Batch> g_kept;
    uint64_t                         g_kept_frame = 0;
    constexpr size_t                 kMaxKept     = 4096;

    /// The mods' replacements of the client's shaders: which mod holds each (name, stage).
    struct Replaced
    {
        std::string name;
        bool        vertex;
        uint32_t    mod;
    };
    std::vector<Replaced> g_replaced;

    wxl::vk::gxfront::Front* Front() { return gx::VulkanFront(); }

    vkr::Render* Mods()
    {
        wxl::vk::gxfront::Front* front = Front();
        return front ? &front->Mods() : nullptr;
    }

    const char* Refusal()
    {
        if (Front()) return nullptr;
        if (gx::VulkanActive()) return "the Vulkan device is being made again";
#if defined(_M_IX86)
        return "the stock client runs on Direct3D 9: wxl.render draws on the runtime's Vulkan renderer ([gx] api = vulkan in "
               "WarcraftXL.ini)";
#else
        return "the recompiled client runs on Direct3D 9: wxl.render draws on its Vulkan renderer ([gx] api = vulkan in WarcraftXL.ini)";
#endif
    }

    uint32_t Caller()
    {
        const wxl::host::ModHost* host = Host();
        return host ? host->CurrentMod() : 0u;
    }

    bool OnGameThread(const char* call)
    {
        static std::atomic<bool> said{false};
        const wxl::host::ModHost* host = Host();
        if (!host || host->OnMainThread()) return true;
        if (!said.exchange(true)) WLOG_WARN(kRenderTag, "wxl.render.%s was called off the main thread, where the frame is drawn; it was refused", call);
        return false;
    }

    wxl_handle Hold(Kind kind, void* object)
    {
        if (!object) return WXL_HANDLE_NONE;
        const wxl_handle handle = g_next++;
        g_handles.emplace(handle, Held{kind, Caller(), object});
        return handle;
    }

    template <class T>
    T* Find(wxl_handle handle, Kind kind)
    {
        const auto found = g_handles.find(handle);
        return found != g_handles.end() && found->second.kind == kind ? static_cast<T*>(found->second.object) : nullptr;
    }

    void Release(const Held& held, vkr::Render* r)
    {
        if (!r) return;
        switch (held.kind)
        {
        case Kind::Image: r->DestroyImage(static_cast<vkr::Image*>(held.object)); break;
        case Kind::Buffer: r->DestroyBuffer(static_cast<vkr::Buffer*>(held.object)); break;
        case Kind::Pipeline: r->DestroyPipeline(static_cast<vkr::Pipeline*>(held.object)); break;
        }
    }

    void Close(wxl_handle handle, Kind kind)
    {
        const auto found = g_handles.find(handle);
        if (found == g_handles.end() || found->second.kind != kind) return;
        Release(found->second, Mods());
        g_handles.erase(found);
    }

    /// The batch a mod names: the one ModelBatchDrawn hands over now, or one it kept this frame.
    const wxl::vk::draw::Batch* BatchOf(const void* batch)
    {
        wxl::vk::gxfront::Front* front = Front();
        if (!front || !batch) return nullptr;
        if (batch == front->Renderer().LastBatch()) return front->Renderer().LastBatch();
        for (const auto& kept : g_kept)
            if (&kept == batch) return &kept;
        return nullptr;
    }

    std::vector<uint32_t> Words(const void* bytes, uint32_t size)
    {
        std::vector<uint32_t> words(size / 4);
        if (bytes && size >= 4) std::memcpy(words.data(), bytes, words.size() * 4);
        return words;
    }

    template <class H>
    uint64_t Raw(H handle)
    {
        if constexpr (std::is_pointer_v<H>) return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
        else return static_cast<uint64_t>(handle);
    }

    // --- wxl.render ---------------------------------------------------------------------------------

    wxl_bool WXL_CALL Supported() { return Refusal() ? WXL_FALSE : WXL_TRUE; }

    wxl_str WXL_CALL UnsupportedReason()
    {
        const char* why = Refusal();
        if (!why) why = "";
        return wxl_str{why, static_cast<uint32_t>(std::strlen(why))};
    }

    wxl_handle MakeImage(const vkr::ImageSpec& spec)
    {
        vkr::Render* r = Mods();
        if (!r || !OnGameThread("CreateImage")) return WXL_HANDLE_NONE;
        std::string why;
        vkr::Image* image = r->CreateImage(spec, &why);
        if (!image) WLOG_WARN(kRenderTag, "wxl.render: an image was refused: %s", why.c_str());
        return Hold(Kind::Image, image);
    }

    wxl_handle WXL_CALL CreateImage(uint32_t width, uint32_t height, uint32_t format, uint32_t samples)
    {
        return MakeImage(vkr::ImageSpec{width, height, 0, format, samples});
    }

    wxl_handle WXL_CALL CreateScreenImage(uint32_t divisor, uint32_t format, uint32_t samples)
    {
        return MakeImage(vkr::ImageSpec{0, 0, std::max(divisor, 1u), format, samples});
    }

    void WXL_CALL DestroyImage(wxl_handle image)
    {
        if (OnGameThread("DestroyImage")) Close(image, Kind::Image);
    }

    uint32_t WXL_CALL ImageSlot(wxl_handle image)
    {
        vkr::Render* r = Mods();
        vkr::Image*  i = Find<vkr::Image>(image, Kind::Image);
        return r && i && OnGameThread("ImageSlot") ? r->Slot(i) : WXL_RENDER_NO_SLOT;
    }

    uint32_t WXL_CALL ImageWidth(wxl_handle image)
    {
        vkr::Render* r = Mods();
        vkr::Image*  i = Find<vkr::Image>(image, Kind::Image);
        return r && i ? r->Extent(i).width : 0;
    }

    uint32_t WXL_CALL ImageHeight(wxl_handle image)
    {
        vkr::Render* r = Mods();
        vkr::Image*  i = Find<vkr::Image>(image, Kind::Image);
        return r && i ? r->Extent(i).height : 0;
    }

    wxl_handle WXL_CALL CreateBuffer(uint32_t size)
    {
        vkr::Render* r = Mods();
        if (!r || !OnGameThread("CreateBuffer")) return WXL_HANDLE_NONE;
        vkr::Buffer* buffer = r->CreateBuffer(size);
        if (!buffer) WLOG_WARN(kRenderTag, "wxl.render: a buffer of %u bytes was refused (1 byte to 256 MiB)", size);
        return Hold(Kind::Buffer, buffer);
    }

    void WXL_CALL DestroyBuffer(wxl_handle buffer)
    {
        if (OnGameThread("DestroyBuffer")) Close(buffer, Kind::Buffer);
    }

    wxl_result WXL_CALL WriteBuffer(wxl_handle buffer, uint32_t offset, const void* data, uint32_t size)
    {
        if (Refusal()) return WXL_E_UNSUPPORTED;
        if (!OnGameThread("WriteBuffer")) return WXL_E_INVALID;
        vkr::Buffer* b = Find<vkr::Buffer>(buffer, Kind::Buffer);
        if (!b) return WXL_E_NOT_FOUND;
        return Mods()->WriteBuffer(b, offset, data, size) ? WXL_OK : WXL_E_INVALID;
    }

    wxl_result WXL_CALL BufferAddress(wxl_handle buffer, uint64_t* address)
    {
        if (Refusal()) return WXL_E_UNSUPPORTED;
        vkr::Buffer* b = Find<vkr::Buffer>(buffer, Kind::Buffer);
        if (!b || !address) return WXL_E_NOT_FOUND;
        *address = Mods()->Address(b);
        // A buffer is made on the device with its first frame: its address is known from then on.
        return *address ? WXL_OK : WXL_E_PENDING;
    }

    uint32_t WXL_CALL Sampler(uint32_t flags)
    {
        vkr::Render* r = Mods();
        return r ? r->Sampler(flags) : 0;
    }

    wxl_handle WXL_CALL CreatePipeline(const wxl_render_pipeline_desc* desc)
    {
        vkr::Render* r = Mods();
        if (!r || !desc || !OnGameThread("CreatePipeline")) return WXL_HANDLE_NONE;
        // A description of abi 0.1.33 ends before `compute`.
        constexpr uint32_t kFirstSize = static_cast<uint32_t>(offsetof(wxl_render_pipeline_desc, compute));
        const bool         computes   = desc->struct_size >= sizeof(wxl_render_pipeline_desc);
        if (desc->struct_size < kFirstSize)
        {
            WLOG_WARN(kRenderTag, "wxl.render: a pipeline description of %u bytes is shorter than the contract's", desc->struct_size);
            return WXL_HANDLE_NONE;
        }
        vkr::PipelineSpec spec;
        spec.kind        = desc->kind;
        spec.name        = desc->name ? desc->name : "";
        spec.vertex      = Words(desc->vertex, desc->vertex ? desc->vertex_size : 0);
        spec.fragment    = Words(desc->fragment, desc->fragment ? desc->fragment_size : 0);
        if (computes) spec.compute = Words(desc->compute, desc->compute ? desc->compute_size : 0);
        spec.color_count = desc->color_count;
        for (uint32_t i = 0; i < WXL_RENDER_MAX_COLORS; ++i)
        {
            spec.color_formats[i] = desc->color_formats[i];
            // A mod names the contract's modes; the runtime's own beyond them stay its own.
            spec.blend[i] = desc->blend[i] <= vkr::kBlendMultiply ? desc->blend[i] : vkr::kBlendNone;
        }
        spec.depth_format = desc->depth_format;
        spec.samples      = desc->samples;
        spec.depth_test   = desc->depth_test;
        spec.depth_write  = desc->depth_write != WXL_FALSE;
        spec.cull         = desc->cull;
        spec.topology     = desc->topology;
        std::string    why;
        vkr::Pipeline* pipeline = r->CreatePipeline(spec, why);
        if (!pipeline)
            WLOG_WARN(kRenderTag, "wxl.render: the pipeline %s was refused: %s", spec.name.empty() ? "of a mod" : spec.name.c_str(), why.c_str());
        return Hold(Kind::Pipeline, pipeline);
    }

    void WXL_CALL DestroyPipeline(wxl_handle pipeline)
    {
        if (OnGameThread("DestroyPipeline")) Close(pipeline, Kind::Pipeline);
    }

    wxl_bool WXL_CALL PipelineReady(wxl_handle pipeline)
    {
        return Mods() && Find<vkr::Pipeline>(pipeline, Kind::Pipeline) ? WXL_TRUE : WXL_FALSE;
    }

    wxl_result WXL_CALL Begin(const wxl_render_pass* pass)
    {
        vkr::Render* r = Mods();
        if (!r) return WXL_E_UNSUPPORTED;
        if (!pass || !OnGameThread("Begin") || pass->struct_size < sizeof(wxl_render_pass) || !g_in_point) return WXL_E_INVALID;
        if (pass->color_count > WXL_RENDER_MAX_COLORS) return WXL_E_INVALID;
        vkr::PassSpec spec;
        spec.color_count = pass->color_count;
        for (uint32_t i = 0; i < pass->color_count; ++i)
        {
            if (pass->colors[i] == WXL_RENDER_SCENE_COLOR) spec.scene_color[i] = true;
            else if (!(spec.colors[i] = Find<vkr::Image>(pass->colors[i], Kind::Image))) return WXL_E_NOT_FOUND;
            std::memcpy(spec.clear_colors[i], pass->clear_colors[i], sizeof spec.clear_colors[i]);
        }
        if (pass->depth == WXL_RENDER_SCENE_DEPTH) spec.scene_depth = true;
        else if (pass->depth && !(spec.depth = Find<vkr::Image>(pass->depth, Kind::Image))) return WXL_E_NOT_FOUND;
        spec.clear       = pass->clear;
        spec.clear_depth = pass->clear_depth;
        std::string why;
        if (r->Begin(spec, &why)) return WXL_OK;
        static bool said = false;
        if (!said) WLOG_WARN(kRenderTag, "wxl.render: a pass was refused: %s", why.c_str());
        said = true;
        return WXL_E_INVALID;
    }

    void WXL_CALL End()
    {
        vkr::Render* r = Mods();
        if (r && OnGameThread("End")) r->End();
    }

    wxl_result Drawn(bool done) { return done ? WXL_OK : WXL_E_INVALID; }

    wxl_result WXL_CALL Draw(wxl_handle pipeline, uint32_t vertex_count, uint32_t instance_count, const void* push, uint32_t push_size)
    {
        vkr::Render* r = Mods();
        if (!r) return WXL_E_UNSUPPORTED;
        vkr::Pipeline* p = Find<vkr::Pipeline>(pipeline, Kind::Pipeline);
        if (!p) return WXL_E_NOT_FOUND;
        return Drawn(g_in_point && r->Draw(p, vertex_count, instance_count, push, push_size));
    }

    wxl_result WXL_CALL DrawIndexed(wxl_handle pipeline, wxl_handle indices, uint32_t first_index, uint32_t index_count,
                                    uint32_t instance_count, const void* push, uint32_t push_size)
    {
        vkr::Render* r = Mods();
        if (!r) return WXL_E_UNSUPPORTED;
        vkr::Pipeline* p = Find<vkr::Pipeline>(pipeline, Kind::Pipeline);
        vkr::Buffer*   b = Find<vkr::Buffer>(indices, Kind::Buffer);
        if (!p || !b) return WXL_E_NOT_FOUND;
        return Drawn(g_in_point && r->DrawIndexed(p, b, first_index, index_count, instance_count, push, push_size));
    }

    wxl_result WXL_CALL Fullscreen(wxl_handle pipeline, const void* push, uint32_t push_size)
    {
        vkr::Render* r = Mods();
        if (!r) return WXL_E_UNSUPPORTED;
        vkr::Pipeline* p = Find<vkr::Pipeline>(pipeline, Kind::Pipeline);
        if (!p) return WXL_E_NOT_FOUND;
        return Drawn(g_in_point && r->Fullscreen(p, push, push_size));
    }

    wxl_result WXL_CALL DrawBatch(const void* batch, wxl_handle pipeline, const void* constants, uint32_t constants_size)
    {
        vkr::Render* r = Mods();
        if (!r) return WXL_E_UNSUPPORTED;
        vkr::Pipeline*              p = Find<vkr::Pipeline>(pipeline, Kind::Pipeline);
        const wxl::vk::draw::Batch* b = BatchOf(batch);
        if (!p || !b || b->frame != Front()->Screen().FrameNumber()) return WXL_E_NOT_FOUND;
        return Drawn(g_in_point && r->DrawBatch(*b, p, constants, constants_size));
    }

    wxl_result WXL_CALL Dispatch(wxl_handle pipeline, uint32_t x, uint32_t y, uint32_t z, const void* push, uint32_t push_size)
    {
        vkr::Render* r = Mods();
        if (!r) return WXL_E_UNSUPPORTED;
        vkr::Pipeline* p = Find<vkr::Pipeline>(pipeline, Kind::Pipeline);
        if (!p) return WXL_E_NOT_FOUND;
        if (!g_in_point || !OnGameThread("Dispatch")) return WXL_E_INVALID;
        std::string why;
        if (r->Dispatch(p, x, y, z, push, push_size, &why)) return WXL_OK;
        static bool said = false;
        if (!said) WLOG_WARN(kRenderTag, "wxl.render: a dispatch was refused: %s", why.c_str());
        said = true;
        return WXL_E_INVALID;
    }

    wxl_result WXL_CALL BonePalette(const void* matrices, uint32_t size, uint64_t* address)
    {
        if (address) *address = 0;
        vkr::Render* r = Mods();
        if (!r) return WXL_E_UNSUPPORTED;
        if (!matrices || !address || !size || size % WXL_RENDER_PALETTE_MATRIX_BYTES || !OnGameThread("BonePalette")) return WXL_E_INVALID;
        VkDeviceAddress made = 0;
        if (!r->BonePalette(static_cast<const float*>(matrices), size / WXL_RENDER_PALETTE_MATRIX_BYTES, made)) return WXL_E_INVALID;
        *address = static_cast<uint64_t>(made);
        return WXL_OK;
    }

    void* WXL_CALL KeepBatch(const void* batch)
    {
        wxl::vk::gxfront::Front* front = Front();
        const wxl::vk::draw::Batch* b  = BatchOf(batch);
        if (!front || !b || !OnGameThread("KeepBatch")) return nullptr;
        const uint64_t frame = front->Screen().FrameNumber();
        if (g_kept_frame != frame)
        {
            // Last frame's copies are no longer drawn: their place is taken.
            g_kept.clear();
            g_kept_frame = frame;
        }
        if (g_kept.size() >= kMaxKept || b->frame != frame) return nullptr;
        g_kept.push_back(*b);
        return &g_kept.back();
    }

    uint32_t WXL_CALL SceneColorSlot()
    {
        vkr::Render* r = Mods();
        return r && g_in_point ? r->SceneColorSlot(g_point_serial) : WXL_RENDER_NO_SLOT;
    }

    uint32_t WXL_CALL SceneDepthSlot()
    {
        vkr::Render* r = Mods();
        return r && g_in_point ? r->SceneDepthSlot(g_point_serial) : WXL_RENDER_NO_SLOT;
    }

    wxl_result WXL_CALL ReplaceShader(wxl_str name, uint32_t stage, const void* spirv, uint32_t size)
    {
        wxl::vk::gxfront::Front* front = Front();
        if (!front) return WXL_E_UNSUPPORTED;
        if (!OnGameThread("ReplaceShader") || !name.size || stage > 1 || !spirv) return WXL_E_INVALID;
        const std::string prefix(name.data, name.size);
        std::string       why;
        if (!front->ReplaceShader(prefix, stage == 0, Words(spirv, size), why))
        {
            WLOG_WARN(kRenderTag, "wxl.render: the replacement of the client's %s shaders %s was refused: %s", stage == 0 ? "vertex" : "pixel",
                      prefix.c_str(), why.c_str());
            return WXL_E_INVALID;
        }
        g_replaced.erase(std::remove_if(g_replaced.begin(), g_replaced.end(),
                                        [&](const Replaced& r) { return r.name == prefix && r.vertex == (stage == 0); }),
                         g_replaced.end());
        g_replaced.push_back(Replaced{prefix, stage == 0, Caller()});
        return WXL_OK;
    }

    wxl_result WXL_CALL RestoreShader(wxl_str name, uint32_t stage)
    {
        wxl::vk::gxfront::Front* front = Front();
        if (!front) return WXL_E_UNSUPPORTED;
        if (!OnGameThread("RestoreShader") || stage > 1) return WXL_E_INVALID;
        const std::string prefix(name.data, name.size);
        g_replaced.erase(std::remove_if(g_replaced.begin(), g_replaced.end(),
                                        [&](const Replaced& r) { return r.name == prefix && r.vertex == (stage == 0); }),
                         g_replaced.end());
        return front->RestoreShader(prefix, stage == 0) ? WXL_OK : WXL_E_NOT_FOUND;
    }

    // --- wxl.vk ----------------------------------------------------------------------------------

    void* WXL_CALL VulkanInstance()
    {
        wxl::vk::gxfront::Front* f = Front();
        return f ? static_cast<void*>(f->Context().Instance()) : nullptr;
    }

    void* WXL_CALL VulkanPhysicalDevice()
    {
        wxl::vk::gxfront::Front* f = Front();
        return f ? static_cast<void*>(f->Context().Physical()) : nullptr;
    }

    void* WXL_CALL VulkanDevice()
    {
        wxl::vk::gxfront::Front* f = Front();
        return f ? static_cast<void*>(f->Context().Device()) : nullptr;
    }

    void* WXL_CALL VulkanQueue()
    {
        wxl::vk::gxfront::Front* f = Front();
        return f ? static_cast<void*>(f->Context().Queue()) : nullptr;
    }

    uint32_t WXL_CALL VulkanQueueFamily()
    {
        wxl::vk::gxfront::Front* f = Front();
        return f ? f->Context().QueueFamily() : 0;
    }

    void* WXL_CALL VulkanGetInstanceProcAddr()
    {
        return Front() && vkGetInstanceProcAddr ? reinterpret_cast<void*>(vkGetInstanceProcAddr) : nullptr;
    }

    uint32_t WXL_CALL VulkanApiVersion()
    {
        wxl::vk::gxfront::Front* f = Front();
        return f ? f->Context().Properties().apiVersion : 0;
    }

    void* WXL_CALL VulkanCommandBuffer()
    {
        wxl::vk::gxfront::Front* f = Front();
        if (!f || !g_in_point || !f->Screen().InFrame() || !OnGameThread("CommandBuffer")) return nullptr;
        // The runtime's rendering ends (a wxl.render pass left open with it): the mod records outside it.
        f->Mods().End();
        f->Screen().EndRendering();
        return static_cast<void*>(f->Screen().Cmd());
    }

    wxl_result WXL_CALL VulkanImage(uint64_t image, wxl_bool resolved, uint64_t* out)
    {
        if (!Front()) return WXL_E_UNSUPPORTED;
        vkr::Image* i = Find<vkr::Image>(image, Kind::Image);
        if (!i || !out) return WXL_E_NOT_FOUND;
        Mods()->Slot(i); // made at the scene's size now
        const bool one = i->samples == VK_SAMPLE_COUNT_1_BIT;
        *out           = Raw(resolved && !one ? i->resolved.image : i->drawn.image);
        return *out ? WXL_OK : WXL_E_NOT_FOUND;
    }

    wxl_result WXL_CALL VulkanImageView(uint64_t image, wxl_bool resolved, uint64_t* out)
    {
        if (!Front()) return WXL_E_UNSUPPORTED;
        vkr::Image* i = Find<vkr::Image>(image, Kind::Image);
        if (!i || !out) return WXL_E_NOT_FOUND;
        Mods()->Slot(i);
        const bool one = i->samples == VK_SAMPLE_COUNT_1_BIT;
        *out           = Raw(resolved && !one ? i->resolved.view : i->drawn.view);
        return *out ? WXL_OK : WXL_E_NOT_FOUND;
    }

    wxl_result WXL_CALL VulkanBuffer(uint64_t buffer, uint64_t* out)
    {
        if (!Front()) return WXL_E_UNSUPPORTED;
        vkr::Buffer* b = Find<vkr::Buffer>(buffer, Kind::Buffer);
        if (!b || !out) return WXL_E_NOT_FOUND;
        *out = Raw(b->buffer.buffer);
        return *out ? WXL_OK : WXL_E_PENDING;
    }

    wxl_result WXL_CALL VulkanSceneColor(uint64_t* out)
    {
        wxl::vk::gxfront::Front* f = Front();
        if (!f) return WXL_E_UNSUPPORTED;
        if (!out || !f->Screen().InFrame()) return WXL_E_INVALID;
        *out = Raw(f->Screen().SceneImage().image);
        return WXL_OK;
    }

    wxl_result WXL_CALL VulkanSceneDepth(uint64_t* out)
    {
        wxl::vk::gxfront::Front* f = Front();
        if (!f) return WXL_E_UNSUPPORTED;
        if (!out || !f->Screen().InFrame()) return WXL_E_INVALID;
        *out = Raw(f->Screen().DepthImage().image);
        return WXL_OK;
    }

    wxl_result WXL_CALL VulkanFrame(uint64_t* out)
    {
        wxl::vk::gxfront::Front* f = Front();
        if (!f) return WXL_E_UNSUPPORTED;
        if (!out) return WXL_E_INVALID;
        *out = f->Screen().FrameNumber();
        return WXL_OK;
    }

    wxl_result WXL_CALL VulkanFramesCompleted(uint64_t* out)
    {
        wxl::vk::gxfront::Front* f = Front();
        if (!f) return WXL_E_UNSUPPORTED;
        if (!out) return WXL_E_INVALID;
        *out = f->Screen().FramesCompleted();
        return WXL_OK;
    }
}

namespace wxl::core::services
{
    const wxl_render_api& RenderApi()
    {
        static const wxl_render_api api = [] {
            wxl_render_api t{};
            t.struct_size         = sizeof t;
            t.supported           = &Supported;
            t.unsupported_reason  = &UnsupportedReason;
            t.create_image        = &CreateImage;
            t.create_screen_image = &CreateScreenImage;
            t.destroy_image       = &DestroyImage;
            t.image_slot          = &ImageSlot;
            t.image_width         = &ImageWidth;
            t.image_height        = &ImageHeight;
            t.create_buffer       = &CreateBuffer;
            t.destroy_buffer      = &DestroyBuffer;
            t.write_buffer        = &WriteBuffer;
            t.buffer_address      = &BufferAddress;
            t.sampler             = &Sampler;
            t.create_pipeline     = &CreatePipeline;
            t.destroy_pipeline    = &DestroyPipeline;
            t.pipeline_ready      = &PipelineReady;
            t.begin               = &Begin;
            t.end                 = &End;
            t.draw                = &Draw;
            t.draw_indexed        = &DrawIndexed;
            t.fullscreen          = &Fullscreen;
            t.draw_batch          = &DrawBatch;
            t.keep_batch          = &KeepBatch;
            t.scene_color_slot    = &SceneColorSlot;
            t.scene_depth_slot    = &SceneDepthSlot;
            t.replace_shader      = &ReplaceShader;
            t.restore_shader      = &RestoreShader;
            t.dispatch            = &Dispatch;
            t.bone_palette        = &BonePalette;
            return t;
        }();
        return api;
    }

    const wxl_vk_api& VkApi()
    {
        static const wxl_vk_api api = [] {
            wxl_vk_api t{};
            t.struct_size            = sizeof t;
            t.instance               = &VulkanInstance;
            t.physical_device        = &VulkanPhysicalDevice;
            t.device                 = &VulkanDevice;
            t.queue                  = &VulkanQueue;
            t.queue_family           = &VulkanQueueFamily;
            t.get_instance_proc_addr = &VulkanGetInstanceProcAddr;
            t.api_version            = &VulkanApiVersion;
            t.command_buffer         = &VulkanCommandBuffer;
            t.image                  = &VulkanImage;
            t.image_view             = &VulkanImageView;
            t.buffer                 = &VulkanBuffer;
            t.scene_color            = &VulkanSceneColor;
            t.scene_depth            = &VulkanSceneDepth;
            t.frame                  = &VulkanFrame;
            t.frames_completed       = &VulkanFramesCompleted;
            return t;
        }();
        return api;
    }

    namespace render
    {
        uint32_t Backend()
        {
            if (gx::VulkanActive()) return WXL_GX_BACKEND_VULKAN;
            switch (binding::gx_device_api())
            {
            case 0: return WXL_GX_BACKEND_OPENGL;
            case 1: return WXL_GX_BACKEND_D3D9;
            case 2: return WXL_GX_BACKEND_D3D9EX;
            case 6: return WXL_GX_BACKEND_VULKAN;
            default: return WXL_GX_BACKEND_NONE;
            }
        }

        bool WantsBatches()
        {
            if (!Front() || !wxl::seam::Listening(WXL_EV_ModelBatchDrawn)) return false;
            const wxl::host::ModHost* host = Host();
            return host && host->Listening(WXL_EV_ModelBatchDrawn);
        }

        void FillBatch(wxl_model_batch_args& args)
        {
            wxl::vk::gxfront::Front* front = Front();
            const wxl::vk::draw::Batch* batch = front ? front->Renderer().LastBatch() : nullptr;
            if (!batch) return;
            args.batch           = const_cast<wxl::vk::draw::Batch*>(batch);
            args.viewport_x      = static_cast<uint32_t>(std::max(batch->viewport[0], 0.0f));
            args.viewport_y      = static_cast<uint32_t>(std::max(batch->viewport[1], 0.0f));
            args.viewport_width  = static_cast<uint32_t>(std::max(batch->viewport[2], 0.0f));
            args.viewport_height = static_cast<uint32_t>(std::max(batch->viewport[3], 0.0f));
            args.viewport_min_z  = batch->viewport[4];
            args.viewport_max_z  = batch->viewport[5];
        }

        bool PointBegin(wxl_render_point point, wxl_render_point_args& args)
        {
            if (!wxl::seam::Listening(WXL_EV_RenderPoint)) return false;
            wxl::host::ModHost* host = Host();
            if (!host || !host->Listening(WXL_EV_RenderPoint)) return false;
            // Only inside a frame the client paints: the loading screen's presents have no points.
            wxl_frame_args frame{};
            if (!frame::Current(frame)) return false;
            args       = wxl_render_point_args{};
            args.point = point;
            args.frame = frame.index;
            binding::gx_window_size(&args.width, &args.height);
            wxl::vk::gxfront::Front* front = Front();
            if (front && front->Screen().InFrame())
            {
                const VkExtent2D scene = front->Screen().WantedSceneExtent();
                args.width             = scene.width;
                args.height            = scene.height;
                args.samples           = front->Screen().Samples();
            }
            static const char* const kNames[] = {"RenderPoint after_opaque_world", "RenderPoint after_liquids", "RenderPoint after_world",
                                                 "RenderPoint after_ui"};
            g_point_zoned = front && static_cast<uint32_t>(point) < 4 && front->Mods().ZoneBegin(kNames[point]);
            // The point's GPU time for the session's last lines (wxl.render's zones go to a viewer only).
            g_point_pass = front && static_cast<uint32_t>(point) < 4 ? front->Screen().PassBegin(kNames[point])
                                                                     : wxl::vk::present::Display::kNoPass;
            g_in_point    = true;
            ++g_point_serial;
            return true;
        }

        void PointEnd()
        {
            g_in_point = false;
            wxl::vk::gxfront::Front* front = Front();
            if (!front) return;
            // What a mod left open closes with the point; the engine's next draw opens its own rendering.
            front->Mods().End();
            front->Mods().CloseZones();
            if (g_point_zoned) front->Mods().ZoneEnd();
            g_point_zoned = false;
            front->Screen().PassEnd(g_point_pass);
            g_point_pass = wxl::vk::present::Display::kNoPass;
        }

        wxl_result GpuZoneBegin(std::string_view name)
        {
            wxl::vk::gxfront::Front* front = Front();
            if (!front) return WXL_E_UNSUPPORTED;
            if (!g_in_point) return WXL_E_INVALID;
            const std::string text(name);
            front->Mods().ZoneBegin(text.c_str()); // nothing recorded while no viewer takes the times
            return WXL_OK;
        }

        void GpuZoneEnd()
        {
            wxl::vk::gxfront::Front* front = Front();
            if (front && g_in_point) front->Mods().ZoneEnd();
        }

        void ForgetMod(uint32_t mod)
        {
            vkr::Render* r = Mods();
            for (auto it = g_handles.begin(); it != g_handles.end();)
            {
                if (it->second.mod != mod)
                {
                    ++it;
                    continue;
                }
                Release(it->second, r);
                it = g_handles.erase(it);
            }
            wxl::vk::gxfront::Front* front = Front();
            for (auto it = g_replaced.begin(); it != g_replaced.end();)
            {
                if (it->mod != mod)
                {
                    ++it;
                    continue;
                }
                if (front) front->RestoreShader(it->name, it->vertex);
                it = g_replaced.erase(it);
            }
        }

        void DeviceGone()
        {
            g_handles.clear();
            g_replaced.clear();
            g_kept.clear();
            g_kept_frame = 0;
            g_in_point   = false;
        }
    }
}
