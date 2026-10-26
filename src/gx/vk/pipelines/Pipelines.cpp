// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The pipelines of the engine's draws, and the programs they are made from.

#include "gx/vk/pipelines/Pipelines.hpp"

#include "gx/vk/Log.hpp"
#include "gx/vk/shaders/Interface.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace wxl::vk::pipelines
{
    namespace spirv
    {
        // The programs, compiled from shaders/glsl/ when the runtime is built (cmake/wxl_glsl.cmake).
#include "ff_vert.h"
#include "ff_clip_vert.h"
#include "ui_vert.h"
#include "ui_stereo_vert.h"
#include "ff_frag.h"
#include "ui_frag.h"
#include "desaturate_frag.h"
    }

    const uint32_t* VertexCode(VertexProgram program, size_t& words, bool clip)
    {
        switch (program)
        {
        case VertexProgram::Interface: words = sizeof spirv::ui_vert / 4; return spirv::ui_vert;
        case VertexProgram::InterfaceStereo: words = sizeof spirv::ui_stereo_vert / 4; return spirv::ui_stereo_vert;
        default:
            if (clip)
            {
                words = sizeof spirv::ff_clip_vert / 4;
                return spirv::ff_clip_vert;
            }
            words = sizeof spirv::ff_vert / 4;
            return spirv::ff_vert;
        }
    }

    const uint32_t* PixelCode(PixelProgram program, size_t& words)
    {
        switch (program)
        {
        case PixelProgram::Interface: words = sizeof spirv::ui_frag / 4; return spirv::ui_frag;
        case PixelProgram::Desaturate: words = sizeof spirv::desaturate_frag / 4; return spirv::desaturate_frag;
        default: words = sizeof spirv::ff_frag / 4; return spirv::ff_frag;
        }
    }

    Blend BlendOf(uint32_t mode)
    {
        switch (mode)
        {
        case 2: return {true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};           // Alpha
        case 3: return {true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE};                           // Add
        case 4: return {true, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO};                          // Mod
        case 5: return {true, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_COLOR};                     // Mod2x
        case 6: return {true, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE};                           // ModAdd
        case 7: return {true, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE};                 // InvSrcAlphaAdd
        case 8: return {true, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ZERO};                // InvSrcAlphaOpaque
        case 9: return {true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ZERO};                          // SrcAlphaOpaque
        case 10: return {true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE};                                // NoAlphaAdd
        case 11: return {true, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR}; // ConstantAlpha
        case 12: return {true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};                // BlendAdd
        default: return {false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO};                              // Opaque, AlphaKey
        }
    }

    namespace
    {
        VkShaderModule Module(VkDevice device, const uint32_t* code, size_t words)
        {
            VkShaderModuleCreateInfo info{};
            info.sType          = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            info.codeSize       = words * sizeof(uint32_t);
            info.pCode          = code;
            VkShaderModule made = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device, &info, nullptr, &made) != VK_SUCCESS) return VK_NULL_HANDLE;
            return made;
        }

        bool HasStencil(VkFormat format)
        {
            return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D16_UNORM_S8_UINT;
        }

        /// The interface of the renderer's own vertex programs: position, normal, colour, two coordinates.
        constexpr uint32_t kOwnInputs = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 7);

        std::vector<uint8_t> ReadFile(const std::string& path)
        {
            std::vector<uint8_t> bytes;
            if (path.empty()) return bytes;
            if (FILE* f = std::fopen(path.c_str(), "rb"))
            {
                uint8_t chunk[65536];
                size_t  got = 0;
                while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0) bytes.insert(bytes.end(), chunk, chunk + got);
                std::fclose(f);
            }
            return bytes;
        }
    }


    namespace
    {
        using Clock = std::chrono::steady_clock;

        double Since(Clock::time_point begin)
        {
            return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        }

        /// The identity of an own program (a translated one's is its record's hash).
        uint64_t OwnIdentity(bool pixel, int index)
        {
            return 0x5758'4C4F'574E'0000ull | static_cast<uint64_t>(index) | (pixel ? 0x8000000000000000ull : 0);
        }

        // The dynamic states, by the part of a pipeline they belong to.
        constexpr VkDynamicState kInputDynamic[] = {VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
        constexpr VkDynamicState kRasterDynamic[] = {
            VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,    VK_DYNAMIC_STATE_CULL_MODE,
            VK_DYNAMIC_STATE_FRONT_FACE,        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS,
            VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
        };
        constexpr VkDynamicState kFragmentDynamic[] = {
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,       VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
            VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        };
        constexpr VkDynamicState kOutputDynamic[] = {VK_DYNAMIC_STATE_BLEND_CONSTANTS};

        constexpr char     kKeysMagic[8] = {'W', 'X', 'L', 'V', 'K', 'P', 'K', '1'};
    }

    // --- Life ------------------------------------------------------------------------------------------

    bool Pipelines::Own(bool pixel, const uint32_t* code, size_t words, const shaders::ProgramInfo& info, uint64_t identity)
    {
        Program p;
        p.module   = Module(context_->Device(), code, words);
        p.info     = info;
        p.identity = identity;
        if (!p.module) return false;
        std::vector<Program>& table = pixel ? pixel_ : vertex_;
        program_ids_[pixel ? 1 : 0][identity] = static_cast<uint16_t>(table.size());
        table.push_back(p);
        return true;
    }

    bool Pipelines::Create(device::Context& context, VkDescriptorSetLayout set_layout, const std::string& cache_file, const Settings& settings,
                           char* why, uint32_t why_size)
    {
        Destroy();
        context_           = &context;
        cache_file_        = cache_file;
        settings_          = settings;
        libraries_         = settings.libraries && context.Chosen().features.graphicsPipelineLibrary;
        stats_             = Stats{};
        const VkDevice dev = context.Device();

        const VkPushConstantRange  push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushBytes};
        VkPipelineLayoutCreateInfo info{};
        info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount         = 1;
        info.pSetLayouts            = &set_layout;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges    = &push;
        VkResult result             = vkCreatePipelineLayout(dev, &info, nullptr, &layout_);
        if (result == VK_SUCCESS)
        {
            // What the previous launches compiled, when the driver takes it (it checks the header
            // itself: another device, driver or version starts empty).
            const std::vector<uint8_t> saved = ReadFile(cache_file_);
            VkPipelineCacheCreateInfo  cache{};
            cache.sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
            cache.initialDataSize = saved.size();
            cache.pInitialData    = saved.empty() ? nullptr : saved.data();
            result                = vkCreatePipelineCache(dev, &cache, nullptr, &cache_);
            if (result != VK_SUCCESS && !saved.empty())
            {
                cache.initialDataSize = 0;
                cache.pInitialData    = nullptr;
                result                = vkCreatePipelineCache(dev, &cache, nullptr, &cache_);
            }
            if (result == VK_SUCCESS && !saved.empty()) Log(LogLevel::Debug, "pipeline cache: %zu bytes from %s", saved.size(), cache_file_.c_str());
        }
        LoadKeys();
        bool       modules = result == VK_SUCCESS;
        const bool clip    = context.Chosen().features.shaderClipDistance;
        for (int i = 0; modules && i < static_cast<int>(VertexProgram::Count); ++i)
        {
            size_t               words = 0;
            const uint32_t*      code  = VertexCode(static_cast<VertexProgram>(i), words, clip);
            shaders::ProgramInfo own;
            own.inputs = kOwnInputs;
            modules    = Own(false, code, words, own, OwnIdentity(false, i));
        }
        for (int i = 0; modules && i < static_cast<int>(PixelProgram::Count); ++i)
        {
            size_t               words = 0;
            const uint32_t*      code  = PixelCode(static_cast<PixelProgram>(i), words);
            shaders::ProgramInfo own;
            own.pixel     = true;
            own.samplers  = i == static_cast<int>(PixelProgram::Fixed) ? 3u : 1u;
            own.fixed_fog = i == static_cast<int>(PixelProgram::Fixed);
            modules       = Own(true, code, words, own, OwnIdentity(true, i));
        }
        if (!modules)
        {
            std::snprintf(why, why_size, "the engine's pipeline layout, cache or programs could not be made: %s", ResultName(result));
            Destroy();
            return false;
        }

        // The workers: one on a small processor, two otherwise (the main thread and the driver's
        // own threads keep the rest).
        uint32_t workers = settings.workers;
        if (!workers) workers = std::thread::hardware_concurrency() >= 6 ? 2 : 1;
        stop_ = false;
        for (uint32_t i = 0; i < workers; ++i) workers_.emplace_back([this] { Work(); });

        // What the earlier launches drew with the own programs alone (the interface) starts now.
        for (int i = 0; i < static_cast<int>(VertexProgram::Count); ++i) Prewarm(OwnIdentity(false, i));
        return true;
    }

    void Pipelines::Save()
    {
        if (!context_ || !cache_ || cache_file_.empty()) return;
        size_t size = 0;
        if (vkGetPipelineCacheData(context_->Device(), cache_, &size, nullptr) != VK_SUCCESS || !size) return;
        std::vector<uint8_t> bytes(size);
        if (vkGetPipelineCacheData(context_->Device(), cache_, &size, bytes.data()) != VK_SUCCESS) return;
        // Written beside, then moved over: a launch that dies while writing leaves the old file.
        const std::string temporary = cache_file_ + ".new";
        FILE*             f         = std::fopen(temporary.c_str(), "wb");
        if (!f) return;
        const bool written = std::fwrite(bytes.data(), 1, size, f) == size;
        std::fclose(f);
        std::remove(cache_file_.c_str());
        if (!written || std::rename(temporary.c_str(), cache_file_.c_str()) != 0) std::remove(temporary.c_str());
        else Log(LogLevel::Debug, "pipeline cache: %zu bytes to %s", size, cache_file_.c_str());
    }

    void Pipelines::Destroy()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            jobs_.clear();
        }
        wake_.notify_all();
        for (std::thread& t : workers_)
            if (t.joinable()) t.join();
        workers_.clear();
        if (context_ && context_->Ready())
        {
            Collect();
            if (!context_->Lost()) Save();
            SaveKeys();
            const VkDevice dev = context_->Device();
            for (auto& entry : pipelines_)
                if (entry.second.pipeline) vkDestroyPipeline(dev, entry.second.pipeline, nullptr);
            for (auto& entry : parts_)
                if (entry.second) vkDestroyPipeline(dev, entry.second, nullptr);
            for (const Retired& r : retired_) vkDestroyPipeline(dev, r.pipeline, nullptr);
            for (Program& p : vertex_)
                if (p.module) vkDestroyShaderModule(dev, p.module, nullptr);
            for (Program& p : pixel_)
                if (p.module) vkDestroyShaderModule(dev, p.module, nullptr);
            if (cache_) vkDestroyPipelineCache(dev, cache_, nullptr);
            if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
        }
        pipelines_.clear();
        parts_.clear();
        queued_.clear();
        queued_parts_.clear();
        retired_.clear();
        done_.clear();
        layouts_.clear();
        layout_ids_.clear();
        targets_.clear();
        vertex_.clear();
        pixel_.clear();
        program_ids_[0].clear();
        program_ids_[1].clear();
        known_.clear();
        known_by_program_.clear();
        known_bytes_.clear();
        fresh_.clear();
        working_ = 0;
        stop_    = false;
        cache_   = VK_NULL_HANDLE;
        layout_  = VK_NULL_HANDLE;
        context_ = nullptr;
    }

    // --- Programs, layouts, targets ---------------------------------------------------------------------

    int Pipelines::AddProgram(const std::vector<uint32_t>& words, const shaders::ProgramInfo& info, uint64_t identity)
    {
        std::vector<Program>& table = info.pixel ? pixel_ : vertex_;
        if (!context_ || table.size() >= 0xFFFF) return -1;
        Program p;
        p.module   = Module(context_->Device(), words.data(), words.size());
        p.info     = info;
        p.identity = identity;
        if (!p.module) return -1;
        const uint16_t id = static_cast<uint16_t>(table.size());
        table.push_back(p);
        program_ids_[info.pixel ? 1 : 0][identity] = id;

        // Its parts start compiling now, ahead of its first draw: the vertex program's for filled
        // polygons, the pixel program's for the targets of the scene's colour format.
        if (libraries_)
        {
            Key key;
            if (info.pixel)
            {
                key.pixel = id;
                for (size_t t = 0; t < targets_.size(); ++t)
                    if (targets_[t].color == VK_FORMAT_B8G8R8A8_UNORM)
                    {
                        key.target = static_cast<uint8_t>(t);
                        const uint64_t part = PartKey(Part::Fragment, key);
                        if (!parts_.count(part) && !queued_parts_.count(part)) Queue(Part::Fragment, part, RecipeOf(key));
                    }
            }
            else
            {
                key.vertex          = id;
                const uint64_t part = PartKey(Part::PreRaster, key);
                if (!parts_.count(part) && !queued_parts_.count(part)) Queue(Part::PreRaster, part, RecipeOf(key));
            }
        }
        Prewarm(identity);
        return id;
    }

    const shaders::ProgramInfo& Pipelines::Info(bool pixel, uint16_t id) const
    {
        static const shaders::ProgramInfo kNone;
        const std::vector<Program>& table = pixel ? pixel_ : vertex_;
        return id < table.size() ? table[id].info : kNone;
    }

    uint16_t Pipelines::LayoutId(const VertexLayout& layout)
    {
        std::string bytes(reinterpret_cast<const char*>(&layout.attribute_count), sizeof layout.attribute_count);
        bytes.append(reinterpret_cast<const char*>(layout.attributes), sizeof(VkVertexInputAttributeDescription) * layout.attribute_count);
        bytes.append(reinterpret_cast<const char*>(layout.bindings), sizeof(VkVertexInputBindingDescription) * layout.binding_count);
        const auto found = layout_ids_.find(bytes);
        if (found != layout_ids_.end()) return found->second;
        const uint16_t id = static_cast<uint16_t>(layouts_.size());
        layouts_.push_back(layout);
        layout_ids_.emplace(std::move(bytes), id);
        if (layouts_.size() == 0x1000) Log(LogLevel::Warn, "4096 vertex layouts: the pipeline keys of later ones share ids");
        return id;
    }

    uint8_t Pipelines::TargetId(VkFormat color, VkFormat depth, uint32_t samples)
    {
        if (!samples) samples = 1;
        for (size_t i = 0; i < targets_.size(); ++i)
            if (targets_[i].color == color && targets_[i].depth == depth && targets_[i].samples == samples) return static_cast<uint8_t>(i);
        targets_.push_back(Target{color, depth, samples});
        return static_cast<uint8_t>(targets_.size() - 1);
    }

    bool Pipelines::CentreSamples(uint8_t target) const
    {
        if (!context_ || target >= targets_.size()) return false;
        const uint32_t samples = targets_[target].samples;
        return samples > 1 && (context_->Chosen().centre_samples & samples) != 0;
    }

    // --- Making a pipeline or a part ------------------------------------------------------------------------

    Pipelines::Recipe Pipelines::RecipeOf(const Key& key) const
    {
        Recipe r;
        if (key.vertex < vertex_.size()) r.vertex = vertex_[key.vertex].module;
        if (key.pixel < pixel_.size()) r.pixel = pixel_[key.pixel].module;
        if (key.layout < layouts_.size()) r.layout = layouts_[key.layout];
        if (key.target < targets_.size()) r.target = targets_[key.target];
        r.blend      = key.blend;
        r.color_mask = key.color_mask;
        r.topology   = key.topology;
        r.wireframe  = key.wireframe;
        r.centre     = key.centre && CentreSamples(key.target);
        return r;
    }

    uint64_t Pipelines::PartKey(Part part, const Key& key) const
    {
        uint64_t fields = 0;
        switch (part)
        {
        case Part::VertexInput: fields = key.layout | static_cast<uint64_t>(key.topology) << 16; break;
        case Part::PreRaster: fields = key.vertex | static_cast<uint64_t>(key.wireframe) << 16; break;
        case Part::Fragment: fields = key.pixel | static_cast<uint64_t>(key.target) << 16 | static_cast<uint64_t>(key.centre) << 24; break;
        case Part::Output:
            fields = key.blend | static_cast<uint64_t>(key.color_mask) << 8 | static_cast<uint64_t>(key.target) << 16 |
                     static_cast<uint64_t>(key.centre) << 24;
            break;
        default: break;
        }
        return static_cast<uint64_t>(part) << 56 | fields;
    }

    VkPipeline Pipelines::Make(Part part, const Recipe& recipe) const
    {
        const Target& target = recipe.target;
        if (!context_ || !layout_) return VK_NULL_HANDLE;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = recipe.vertex;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = recipe.pixel;
        stages[1].pName  = "main";

        VkPipelineVertexInputStateCreateInfo input{};
        input.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        input.vertexBindingDescriptionCount   = recipe.layout.binding_count;
        input.pVertexBindingDescriptions      = recipe.layout.bindings;
        input.vertexAttributeDescriptionCount = recipe.layout.attribute_count;
        input.pVertexAttributeDescriptions    = recipe.layout.attributes;

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = recipe.topology == TopologyClass::Points  ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                            : recipe.topology == TopologyClass::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                       : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = recipe.wireframe && context_->Chosen().features.fillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        raster.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(target.samples ? target.samples : 1);
        // D3D9's MULTISAMPLEANTIALIAS off: "the samples are all written with the same value, sampled
        // at the pixel's centre". Every sample placed on the centre takes the centre's coverage and
        // depth; the colour is computed once per pixel as for any draw here (no sample shading).
        VkSampleLocationEXT                     centres[16];
        VkPipelineSampleLocationsStateCreateInfoEXT locations{};
        if (recipe.centre && multisample.rasterizationSamples > VK_SAMPLE_COUNT_1_BIT && multisample.rasterizationSamples <= 16)
        {
            for (VkSampleLocationEXT& c : centres) c = {0.5f, 0.5f};
            locations.sType                                 = VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT;
            locations.sampleLocationsEnable                 = VK_TRUE;
            locations.sampleLocationsInfo.sType             = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT;
            locations.sampleLocationsInfo.sampleLocationsPerPixel = multisample.rasterizationSamples;
            locations.sampleLocationsInfo.sampleLocationGridSize  = {1, 1};
            locations.sampleLocationsInfo.sampleLocationsCount    = static_cast<uint32_t>(multisample.rasterizationSamples);
            locations.sampleLocationsInfo.pSampleLocations        = centres;
            multisample.pNext                               = &locations;
        }

        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

        // A target whose format cannot blend (a float one on some devices) draws unblended.
        Blend blend = BlendOf(recipe.blend);
        if (blend.enable && target.color != VK_FORMAT_UNDEFINED)
        {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(context_->Physical(), target.color, &props);
            if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) blend.enable = false;
        }
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.blendEnable         = blend.enable ? VK_TRUE : VK_FALSE;
        attachment.srcColorBlendFactor = blend.source;
        attachment.dstColorBlendFactor = blend.destination;
        attachment.colorBlendOp        = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = blend.source;
        attachment.dstAlphaBlendFactor = blend.destination;
        attachment.alphaBlendOp        = VK_BLEND_OP_ADD;
        attachment.colorWriteMask      = recipe.color_mask;
        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments    = &attachment;

        // The dynamic states of the parts made, in one list.
        VkDynamicState dynamicStates[24];
        uint32_t       dynamicCount = 0;
        auto           add          = [&](const VkDynamicState* states, size_t count) {
            for (size_t i = 0; i < count; ++i) dynamicStates[dynamicCount++] = states[i];
        };
        const bool whole = part == Part::Whole;
        if (whole || part == Part::VertexInput) add(kInputDynamic, std::size(kInputDynamic));
        if (whole || part == Part::PreRaster) add(kRasterDynamic, std::size(kRasterDynamic));
        if (whole || part == Part::Fragment) add(kFragmentDynamic, std::size(kFragmentDynamic));
        if (whole || part == Part::Output) add(kOutputDynamic, std::size(kOutputDynamic));
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = dynamicCount;
        dynamic.pDynamicStates    = dynamicStates;

        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount    = 1;
        rendering.pColorAttachmentFormats = &target.color;
        rendering.depthAttachmentFormat   = target.depth;
        rendering.stencilAttachmentFormat = HasStencil(target.depth) ? target.depth : VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineLibraryCreateInfoEXT library{};
        library.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT;

        VkGraphicsPipelineCreateInfo info{};
        info.sType          = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.layout         = layout_;
        info.pDynamicState  = &dynamic;
        switch (part)
        {
        case Part::Whole:
            info.pNext               = &rendering;
            info.stageCount          = 2;
            info.pStages             = stages;
            info.pVertexInputState   = &input;
            info.pInputAssemblyState = &assembly;
            info.pViewportState      = &viewport;
            info.pRasterizationState = &raster;
            info.pMultisampleState   = &multisample;
            info.pDepthStencilState  = &depth;
            info.pColorBlendState    = &colorBlend;
            break;
        case Part::VertexInput:
            library.flags            = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
            info.pNext               = &library;
            info.flags               = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
            info.pVertexInputState   = &input;
            info.pInputAssemblyState = &assembly;
            break;
        case Part::PreRaster:
            library.flags            = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
            info.pNext               = &library;
            info.flags               = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
            info.stageCount          = 1;
            info.pStages             = &stages[0];
            info.pViewportState      = &viewport;
            info.pRasterizationState = &raster;
            break;
        case Part::Fragment:
            library.flags            = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
            library.pNext            = &rendering;
            info.pNext               = &library;
            info.flags               = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
            info.stageCount          = 1;
            info.pStages             = &stages[1];
            info.pMultisampleState   = &multisample;
            info.pDepthStencilState  = &depth;
            break;
        case Part::Output:
            library.flags            = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
            library.pNext            = &rendering;
            info.pNext               = &library;
            info.flags               = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
            info.pMultisampleState   = &multisample;
            info.pColorBlendState    = &colorBlend;
            break;
        }
        VkPipeline     made   = VK_NULL_HANDLE;
        const VkResult result = vkCreateGraphicsPipelines(context_->Device(), cache_, 1, &info, nullptr, &made);
        if (result != VK_SUCCESS)
        {
            Log(LogLevel::Warn, "vkCreateGraphicsPipelines (%s): %s",
                part == Part::Whole ? "a pipeline" : part == Part::VertexInput ? "a vertex input part" : part == Part::PreRaster ? "a pre-rasterisation part"
                                                   : part == Part::Fragment    ? "a fragment part"
                                                                               : "an output part",
                ResultName(result));
            return VK_NULL_HANDLE;
        }
        return made;
    }

    // --- Getting a pipeline at a draw --------------------------------------------------------------------

    void Pipelines::Stall(double ms, bool part)
    {
        if (part)
        {
            ++stats_.parts_main;
            stats_.parts_main_ms += ms;
        }
        else
        {
            ++stats_.compiled_main;
            stats_.compiled_main_ms += ms;
        }
        if (ms >= kHitchMs) ++stats_.hitches;
        stats_.longest_ms = std::max(stats_.longest_ms, ms);
    }

    VkPipeline Pipelines::Get(const Key& key, bool wait, Got* got)
    {
        Got            how    = Got::Ready;
        const uint64_t packed = key.Packed();
        auto           found  = pipelines_.find(packed);
        if (found == pipelines_.end() || !found->second.pipeline)
        {
            // What the workers finished since: perhaps this one.
            Collect();
            found = pipelines_.find(packed);
        }
        if (found != pipelines_.end())
        {
            if (got) *got = found->second.pipeline ? Got::Ready : Got::Failed;
            return found->second.pipeline;
        }
        if (key.layout >= layouts_.size() || key.vertex >= vertex_.size() || key.pixel >= pixel_.size() || key.target >= targets_.size())
        {
            if (got) *got = Got::Failed;
            return VK_NULL_HANDLE;
        }

        const Recipe recipe = RecipeOf(key);
        const bool   now    = wait || !settings_.async;
        VkPipeline   made   = VK_NULL_HANDLE;
        if (libraries_)
        {
            made = Link(key, recipe, now, how);
            if (made || how == Got::Pending)
            {
                if (got) *got = how;
                return made;
            }
            // A part that cannot be made: the pipeline is compiled whole instead.
        }
        if (!now)
        {
            if (!queued_.count(packed))
            {
                Queue(Part::Whole, packed, recipe);
                Remember(key);
            }
            if (got) *got = Got::Pending;
            return VK_NULL_HANDLE;
        }
        const Clock::time_point begin = Clock::now();
        made                          = Make(Part::Whole, recipe);
        Stall(Since(begin), false);
        pipelines_[packed] = Entry{made, false};
        if (made)
        {
            ++stats_.made;
            Remember(key);
            how = Got::Compiled;
        }
        else
        {
            ++stats_.failed;
            how = Got::Failed;
            Log(LogLevel::Warn, "a pipeline could not be made (programs %u/%u, layout %u, blend %u, target %u): its draws are dropped",
                key.vertex, key.pixel, key.layout, key.blend, key.target);
        }
        if (got) *got = how;
        return made;
    }

    VkPipeline Pipelines::PartOf(Part part, const Key& key, const Recipe& recipe, bool wait, Got& got)
    {
        const uint64_t pk    = PartKey(part, key);
        const auto     found = parts_.find(pk);
        if (found != parts_.end())
        {
            if (!found->second) got = Got::Failed;
            return found->second;
        }
        // The vertex input and the output carry no program: made on the spot. The others compile a
        // program: on a worker, unless the draw may not wait.
        const bool cheap = part == Part::VertexInput || part == Part::Output;
        if (!cheap && !wait)
        {
            if (!queued_parts_.count(pk)) Queue(part, pk, recipe);
            got = Got::Pending;
            return VK_NULL_HANDLE;
        }
        const Clock::time_point begin = Clock::now();
        const VkPipeline        made  = Make(part, recipe);
        if (!cheap) Stall(Since(begin), true);
        parts_[pk] = made;
        if (!made) got = Got::Failed;
        return made;
    }

    VkPipeline Pipelines::Link(const Key& key, const Recipe& recipe, bool wait, Got& got)
    {
        got                 = Got::Ready;
        const VkPipeline vi = PartOf(Part::VertexInput, key, recipe, wait, got);
        const VkPipeline pr = PartOf(Part::PreRaster, key, recipe, wait, got);
        const VkPipeline fr = PartOf(Part::Fragment, key, recipe, wait, got);
        const VkPipeline ou = PartOf(Part::Output, key, recipe, wait, got);
        if (got == Got::Failed) return VK_NULL_HANDLE;
        if (!vi || !pr || !fr || !ou)
        {
            got = Got::Pending;
            return VK_NULL_HANDLE;
        }
        const VkPipeline parts[4] = {vi, pr, fr, ou};
        VkPipelineLibraryCreateInfoKHR libraries{};
        libraries.sType        = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;
        libraries.libraryCount = 4;
        libraries.pLibraries   = parts;
        VkGraphicsPipelineCreateInfo info{};
        info.sType  = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext  = &libraries;
        info.layout = layout_;
        const Clock::time_point begin  = Clock::now();
        VkPipeline              made   = VK_NULL_HANDLE;
        const VkResult          result = vkCreateGraphicsPipelines(context_->Device(), VK_NULL_HANDLE, 1, &info, nullptr, &made);
        const double            ms     = Since(begin);
        if (result != VK_SUCCESS)
        {
            Log(LogLevel::Warn, "vkCreateGraphicsPipelines (linking four parts): %s; compiled whole instead", ResultName(result));
            got = Got::Failed;
            return VK_NULL_HANDLE;
        }
        ++stats_.linked;
        ++stats_.made;
        stats_.linked_ms += ms;
        if (ms >= kHitchMs) ++stats_.hitches;
        stats_.longest_ms = std::max(stats_.longest_ms, ms);
        const uint64_t packed = key.Packed();
        pipelines_[packed]    = Entry{made, true};
        // The whole compile, optimised across the parts, takes its place when a worker has made it.
        if (!queued_.count(packed)) Queue(Part::Whole, packed, recipe);
        Remember(key);
        got = Got::Linked;
        return made;
    }

    // --- The workers ---------------------------------------------------------------------------------------

    void Pipelines::Queue(Part part, uint64_t key, const Recipe& recipe, bool prewarm)
    {
        if (workers_.empty()) return;
        (part == Part::Whole ? queued_ : queued_parts_).insert(key);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Job job;
            job.part    = part;
            job.key     = key;
            job.recipe  = recipe;
            job.prewarm = prewarm;
            // A draw waits on what it asks for; a prewarm goes after it.
            if (prewarm) jobs_.push_back(job);
            else jobs_.push_front(job);
        }
        wake_.notify_one();
    }

    void Pipelines::Work()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) return;
                job = jobs_.front();
                jobs_.pop_front();
                ++working_;
            }
            const Clock::time_point begin = Clock::now();
            const VkPipeline        made  = Make(job.part, job.recipe);
            const double            ms    = Since(begin);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                done_.push_back(Done{job.part, job.key, made, ms, job.prewarm});
                --working_;
            }
            idle_.notify_all();
        }
    }

    void Pipelines::Collect()
    {
        std::vector<Done> done;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (done_.empty()) return;
            done.swap(done_);
        }
        for (const Done& d : done) Install(d);
    }

    void Pipelines::Retire(VkPipeline pipeline)
    {
        if (pipeline) retired_.push_back(Retired{frame_, pipeline});
    }

    void Pipelines::Install(const Done& d)
    {
        const VkDevice dev = context_->Device();
        stats_.background_ms += d.ms;
        if (d.part != Part::Whole)
        {
            queued_parts_.erase(d.key);
            ++stats_.parts_background;
            const auto found = parts_.find(d.key);
            if (found != parts_.end())
            {
                // Made on the spot meanwhile (a draw that could not wait): the worker's goes.
                if (d.pipeline) vkDestroyPipeline(dev, d.pipeline, nullptr);
                return;
            }
            parts_[d.key] = d.pipeline;
            return;
        }
        queued_.erase(d.key);
        ++stats_.background;
        if (d.prewarm && d.pipeline) ++stats_.prewarmed;
        const auto found = pipelines_.find(d.key);
        if (found == pipelines_.end())
        {
            pipelines_[d.key] = Entry{d.pipeline, false};
            if (d.pipeline) ++stats_.made;
            else
            {
                ++stats_.failed;
                Log(LogLevel::Warn, "a pipeline could not be made on a worker: its draws are dropped");
            }
            return;
        }
        if (found->second.linked && d.pipeline)
        {
            // The linked one may be bound in the open frame: it goes once that frame completes.
            Retire(found->second.pipeline);
            found->second = Entry{d.pipeline, false};
            ++stats_.optimized;
            return;
        }
        if (d.pipeline) vkDestroyPipeline(dev, d.pipeline, nullptr);
    }

    void Pipelines::BeginFrame(uint64_t frame, uint64_t completed)
    {
        frame_ = frame;
        if (!context_) return;
        Collect();
        size_t kept = 0;
        for (const Retired& r : retired_)
        {
            if (r.frame <= completed) vkDestroyPipeline(context_->Device(), r.pipeline, nullptr);
            else retired_[kept++] = r;
        }
        retired_.resize(kept);
    }

    size_t Pipelines::Busy() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return jobs_.size() + working_;
    }

    void Pipelines::Drain()
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            idle_.wait(lock, [this] { return stop_ || (jobs_.empty() && working_ == 0); });
        }
        Collect();
    }

    Pipelines::Stats Pipelines::Counters() const { return stats_; }

    // --- The pipelines drawn, between launches ----------------------------------------------------------

    void Pipelines::Remember(const Key& key)
    {
        if (settings_.keys_file.empty() || key.vertex >= vertex_.size() || key.pixel >= pixel_.size() || key.layout >= layouts_.size() ||
            key.target >= targets_.size())
            return;
        Description d;
        std::memset(&d, 0, sizeof d);
        d.vertex     = vertex_[key.vertex].identity;
        d.pixel      = pixel_[key.pixel].identity;
        d.layout     = layouts_[key.layout];
        d.target     = targets_[key.target];
        d.blend      = key.blend;
        d.color_mask = key.color_mask;
        d.topology   = static_cast<uint8_t>(key.topology);
        d.wireframe  = static_cast<uint8_t>((key.wireframe ? 1 : 0) | (key.centre ? 2 : 0));
        std::string bytes(reinterpret_cast<const char*>(&d), sizeof d);
        if (!known_bytes_.insert(std::move(bytes)).second) return;
        fresh_.push_back(d);
    }

    void Pipelines::Prewarm(uint64_t identity)
    {
        const auto range = known_by_program_.equal_range(identity);
        for (auto it = range.first; it != range.second; ++it)
        {
            const Description& d      = known_[it->second];
            const auto         vertex = program_ids_[0].find(d.vertex);
            const auto         pixel  = program_ids_[1].find(d.pixel);
            if (vertex == program_ids_[0].end() || pixel == program_ids_[1].end()) continue;
            Key key;
            key.vertex     = vertex->second;
            key.pixel      = pixel->second;
            key.layout     = LayoutId(d.layout);
            key.target     = TargetId(d.target.color, d.target.depth, d.target.samples);
            key.blend      = d.blend;
            key.color_mask = d.color_mask;
            key.topology   = static_cast<TopologyClass>(std::min<uint8_t>(d.topology, 2));
            key.wireframe  = (d.wireframe & 1) != 0;
            key.centre     = (d.wireframe & 2) != 0 && CentreSamples(key.target);
            const uint64_t packed = key.Packed();
            if (pipelines_.count(packed) || queued_.count(packed)) continue;
            Queue(Part::Whole, packed, RecipeOf(key), true);
        }
    }

    void Pipelines::LoadKeys()
    {
        known_.clear();
        known_by_program_.clear();
        known_bytes_.clear();
        fresh_.clear();
        const std::vector<uint8_t> bytes = ReadFile(settings_.keys_file);
        const size_t               head  = sizeof kKeysMagic + sizeof(uint32_t);
        uint32_t                   size  = 0;
        if (bytes.size() >= head) std::memcpy(&size, bytes.data() + sizeof kKeysMagic, sizeof size);
        if (bytes.size() < head || std::memcmp(bytes.data(), kKeysMagic, sizeof kKeysMagic) != 0 || size != sizeof(Description))
        {
            // No file, or another format's: started again at the next save.
            if (!bytes.empty()) std::remove(settings_.keys_file.c_str());
            return;
        }
        for (size_t at = head; at + sizeof(Description) <= bytes.size(); at += sizeof(Description))
        {
            Description d;
            std::memcpy(&d, bytes.data() + at, sizeof d);
            if (!known_bytes_.insert(std::string(reinterpret_cast<const char*>(&d), sizeof d)).second) continue;
            known_by_program_.emplace(d.vertex, known_.size());
            known_by_program_.emplace(d.pixel, known_.size());
            known_.push_back(d);
        }
        stats_.prewarm_known = known_.size();
        if (!known_.empty()) Log(LogLevel::Debug, "pipelines: %zu drawn by earlier launches, compiled ahead as their programs come", known_.size());
    }

    void Pipelines::SaveKeys()
    {
        if (settings_.keys_file.empty() || fresh_.empty()) return;
        FILE* f = std::fopen(settings_.keys_file.c_str(), "ab");
        if (!f) return;
        std::fseek(f, 0, SEEK_END);
        if (std::ftell(f) == 0)
        {
            const uint32_t size = sizeof(Description);
            std::fwrite(kKeysMagic, 1, sizeof kKeysMagic, f);
            std::fwrite(&size, sizeof size, 1, f);
        }
        std::fwrite(fresh_.data(), sizeof(Description), fresh_.size(), f);
        std::fclose(f);
        Log(LogLevel::Debug, "pipelines: %zu new ones kept for the next launch's prewarm", fresh_.size());
        fresh_.clear();
    }
}
