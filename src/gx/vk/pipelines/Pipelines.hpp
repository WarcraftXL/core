// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The pipelines the engine's draws are recorded with, and the programs they are made from: the
// renderer's own (GLSL compiled when the renderer is built: D3D9's fixed function, the client's
// interface programs) and the client's own, translated from their DX9 records (shaders/Translate.hpp)
// and added as the engine creates them. A program keeps its id for the life of the device, so a
// pipeline's key never names another program.
//
// A pipeline is found by a 64-bit key of small ids (the programs, the vertex layout, the blend
// mode, the colour mask, the topology's class, the fill, the target's formats and samples), kept in
// a hash map; whatever Vulkan 1.3 lets be dynamic is (viewport, scissor, cull, front face, topology
// within its class, depth test, write, compare and bias, blend constants), so a state change outside
// the key costs no pipeline. A missing pipeline is made in one of three ways:
//
//   - linked from parts (VK_EXT_graphics_pipeline_library, where the device has it): the vertex
//     input and the output parts are cheap and made on the spot; the parts that compile a program
//     (pre-rasterisation per vertex program, fragment per pixel program and target) are compiled on
//     a worker as soon as the program is added. Linking the four takes a fraction of a millisecond:
//     the draw is not held up. The full pipeline, compiled whole on a worker, takes its place later;
//   - compiled whole on a worker (asynchronous mode, without the parts): the draws that may be left
//     out are, meanwhile (the owner's choice: no stall, a draw missing for a few frames);
//   - compiled on the spot (synchronous mode, or a draw that may not be left out: one into a texture
//     the engine keeps, a portrait or the minimap): a stall, counted with its length.
//
// The VkPipelineCache persists on disk between launches, and so does the list of the pipelines
// drawn (each described by what it is made of, the programs by their records' hashes): at the next
// launch, each is compiled on a worker as soon as the engine has created both its programs.
//
// One layout for all: the bindless set, and the 128 bytes of push constants (shaders::Push).

#pragma once

#include "gx/vk/device/Device.hpp"
#include "gx/vk/shaders/Translate.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wxl::vk::pipelines
{
    /// The renderer's own vertex programs (shaders/glsl/), the first ids of the vertex table.
    enum class VertexProgram : uint16_t
    {
        Fixed,           ///< D3D9's fixed function (ff.vert)
        Interface,       ///< the client's "UI:0" (ui.vert)
        InterfaceStereo, ///< the client's "UI:1" (ui.vert, WXL_STEREO)
        Count,
    };

    /// The renderer's own pixel programs, the first ids of the pixel table.
    enum class PixelProgram : uint16_t
    {
        Fixed,      ///< D3D9's fixed function, stages 0 and 1 (ff.frag)
        Interface,  ///< the client's "UI" (ui.frag)
        Desaturate, ///< the client's "Desaturate" (desaturate.frag)
        Count,
    };

    /// The embedded SPIR-V of an own program (the fixed-function vertex program with the user clip
    /// plane when @p clip).
    const uint32_t* VertexCode(VertexProgram program, size_t& words, bool clip = false);
    const uint32_t* PixelCode(PixelProgram program, size_t& words);

    /// A vertex layout: its attributes and the buffers they read.
    struct VertexLayout
    {
        uint32_t                          attribute_count = 0;
        VkVertexInputAttributeDescription attributes[16] = {};
        uint32_t                          binding_count = 0;
        VkVertexInputBindingDescription   bindings[16] = {};
    };

    /// The topology classes a pipeline is made for; the topology itself is dynamic inside it.
    enum class TopologyClass : uint8_t
    {
        Points,
        Lines,
        Triangles,
    };

    struct Key
    {
        uint16_t      vertex = 0;   ///< a vertex program's id
        uint16_t      pixel = 0;    ///< a pixel program's id
        uint16_t      layout = 0;   ///< LayoutId()
        uint8_t       blend = 0;    ///< EGxBlend
        uint8_t       color_mask = 0xF;
        TopologyClass topology = TopologyClass::Triangles;
        bool          wireframe = false;
        uint8_t       target = 0;   ///< TargetId(): the colour and depth formats drawn into, and their samples

        uint64_t Packed() const
        {
            return static_cast<uint64_t>(vertex) | static_cast<uint64_t>(pixel) << 16 | static_cast<uint64_t>(layout & 0xFFF) << 32 |
                   static_cast<uint64_t>(blend & 0xF) << 44 | static_cast<uint64_t>(color_mask & 0xF) << 48 |
                   static_cast<uint64_t>(topology) << 52 | static_cast<uint64_t>(wireframe) << 54 | static_cast<uint64_t>(target & 0x1F) << 55;
        }
    };

    /// D3D9's blend of an EGxBlend mode (the D3D9 device's s_srcBlend/s_dstBlend): enabled, the
    /// source and destination factors, the same for alpha (D3D9 has no separate alpha blend here).
    struct Blend
    {
        bool          enable;
        VkBlendFactor source;
        VkBlendFactor destination;
    };
    Blend BlendOf(uint32_t mode);

    /// How missing pipelines are made.
    struct Settings
    {
        bool        async = false;     ///< [gx] async_pipelines: compiled on a worker; draws that may be left out are, meanwhile
        bool        libraries = true;  ///< linked from parts where the device has graphics pipeline libraries
        uint32_t    workers = 0;       ///< compile threads; 0: chosen from the processor count (1 or 2)
        std::string keys_file;         ///< the pipelines drawn, kept between launches (empty: none)
    };

    /// How Get came by a pipeline.
    enum class Got
    {
        Ready,    ///< it was there
        Linked,   ///< linked now from its parts (fast)
        Compiled, ///< compiled now, on this thread (a stall)
        Pending,  ///< compiling on a worker: the draw may be left out
        Failed,   ///< it cannot be made
    };

    class Pipelines
    {
    public:
        static constexpr uint32_t kPushBytes = 128;
        /// A stall of the main thread at least this long is a hitch.
        static constexpr double kHitchMs = 4.0;

        struct Stats
        {
            uint64_t made = 0;             ///< pipelines in use
            uint64_t linked = 0;           ///< linked from parts at a draw
            double   linked_ms = 0.0;
            uint64_t compiled_main = 0;    ///< compiled whole on the main thread
            double   compiled_main_ms = 0.0;
            uint64_t parts_main = 0;       ///< parts compiled on the main thread (a draw that could not wait for a worker)
            double   parts_main_ms = 0.0;
            uint64_t hitches = 0;          ///< main-thread stalls of kHitchMs or more
            double   longest_ms = 0.0;     ///< the longest stall
            uint64_t background = 0;       ///< pipelines compiled whole on a worker
            uint64_t parts_background = 0; ///< parts compiled on a worker
            double   background_ms = 0.0;
            uint64_t optimized = 0;        ///< linked pipelines replaced by their whole compile
            uint64_t prewarm_known = 0;    ///< pipelines the earlier launches drew (the keys file)
            uint64_t prewarmed = 0;        ///< of those, compiled ahead in this launch
            uint64_t failed = 0;
        };

        Pipelines() = default;
        Pipelines(const Pipelines&)            = delete;
        Pipelines& operator=(const Pipelines&) = delete;
        ~Pipelines() { Destroy(); }

        /// Reading the bindless set of @p set_layout; @p cache_file (may be empty) holds the
        /// VkPipelineCache between launches.
        bool Create(device::Context& context, VkDescriptorSetLayout set_layout, const std::string& cache_file, const Settings& settings,
                    char* why, uint32_t why_size);
        /// Stops the workers (waiting for the compiles under way), writes the pipeline cache and the
        /// keys to their files, then releases everything. The GPU must be done with them.
        void Destroy();
        bool Ready() const { return layout_ != VK_NULL_HANDLE; }

        VkPipelineLayout Layout() const { return layout_; }
        /// Whether missing pipelines are linked from parts.
        bool Libraries() const { return libraries_; }
        bool Async() const { return settings_.async; }

        /// A translated program added, @p identity its record's hash (the same in every launch):
        /// its id, or -1 when its module cannot be made. Its parts start compiling on a worker.
        int AddProgram(const std::vector<uint32_t>& words, const shaders::ProgramInfo& info, uint64_t identity);
        /// What a program reads (the own programs' as the interface fixes them).
        const shaders::ProgramInfo& Info(bool pixel, uint16_t id) const;
        size_t Programs(bool pixel) const { return pixel ? pixel_.size() : vertex_.size(); }

        /// The id of @p layout, given at its first sight.
        uint16_t LayoutId(const VertexLayout& layout);
        /// The id of a colour format, a depth format (UNDEFINED: none) and their samples per pixel,
        /// given at its first sight.
        uint8_t TargetId(VkFormat color, VkFormat depth, uint32_t samples = 1);

        /**
         * @brief The pipeline of @p key: there, linked from its parts, compiled now, or not yet.
         * @param wait  the draw may not be left out (it is compiled now when it cannot be linked)
         * @return null when it is not ready (@p got Pending: a worker compiles it) or cannot be
         *         made (Failed, logged once).
         */
        VkPipeline Get(const Key& key, bool wait, Got* got = nullptr);

        /// A frame begins: what the workers finished is taken in (whole pipelines in place of the
        /// linked ones), and what was replaced in frames up to @p completed is released.
        void BeginFrame(uint64_t frame, uint64_t completed);

        size_t Count() const { return pipelines_.size(); }
        size_t Layouts() const { return layouts_.size(); }
        /// Jobs queued or under way on the workers.
        size_t Busy() const;
        /// Waits until the workers have nothing left, then takes in what they made (tests).
        void Drain();
        /// Saves the pipeline cache to its file now.
        void Save();
        Stats Counters() const;

    private:
        struct Program
        {
            VkShaderModule       module = VK_NULL_HANDLE;
            shaders::ProgramInfo info;
            uint64_t             identity = 0;
        };
        struct Target
        {
            VkFormat color;
            VkFormat depth;
            uint32_t samples;
        };
        /// Everything a pipeline or a part is made from, by value: what a worker needs.
        struct Recipe
        {
            VkShaderModule vertex = VK_NULL_HANDLE;
            VkShaderModule pixel = VK_NULL_HANDLE;
            VertexLayout   layout;
            Target         target{};
            uint8_t        blend = 0;
            uint8_t        color_mask = 0xF;
            TopologyClass  topology = TopologyClass::Triangles;
            bool           wireframe = false;
        };
        enum class Part : uint8_t
        {
            Whole,       ///< the whole pipeline, compiled at once
            VertexInput, ///< graphics pipeline library parts
            PreRaster,
            Fragment,
            Output,
        };
        struct Job
        {
            Part     part = Part::Whole;
            uint64_t key = 0; ///< the pipeline's packed key, or the part's own key
            Recipe   recipe;
            bool     prewarm = false;
        };
        struct Done
        {
            Part       part;
            uint64_t   key;
            VkPipeline pipeline;
            double     ms;
            bool       prewarm;
        };
        struct Entry
        {
            VkPipeline pipeline = VK_NULL_HANDLE;
            bool       linked = false; ///< linked from parts: its whole compile replaces it when done
        };
        /// A pipeline drawn, as the keys file keeps it.
        struct Description
        {
            uint64_t     vertex = 0; ///< the programs' identities
            uint64_t     pixel = 0;
            VertexLayout layout;
            Target       target{};
            uint8_t      blend = 0;
            uint8_t      color_mask = 0xF;
            uint8_t      topology = 2;
            uint8_t      wireframe = 0;
        };

        Recipe     RecipeOf(const Key& key) const;
        VkPipeline Make(Part part, const Recipe& recipe) const;
        VkPipeline Link(const Key& key, const Recipe& recipe, bool wait, Got& got);
        VkPipeline PartOf(Part part, const Key& key, const Recipe& recipe, bool wait, Got& got);
        void       Queue(Part part, uint64_t key, const Recipe& recipe, bool prewarm = false);
        void       Collect();
        void       Install(const Done& done);
        void       Work();
        void       Stall(double ms, bool part);
        void       Retire(VkPipeline pipeline);
        void       Remember(const Key& key);
        void       Prewarm(uint64_t identity);
        void       LoadKeys();
        void       SaveKeys();
        bool       Own(bool pixel, const uint32_t* code, size_t words, const shaders::ProgramInfo& info, uint64_t identity);
        uint64_t   PartKey(Part part, const Key& key) const;

        device::Context*                           context_ = nullptr;
        VkPipelineLayout                           layout_ = VK_NULL_HANDLE;
        VkPipelineCache                            cache_ = VK_NULL_HANDLE;
        std::string                                cache_file_;
        Settings                                   settings_;
        bool                                       libraries_ = false;
        std::vector<Program>                       vertex_;
        std::vector<Program>                       pixel_;
        std::vector<Target>                        targets_;
        std::unordered_map<uint64_t, Entry>        pipelines_;
        std::unordered_map<uint64_t, VkPipeline>   parts_;  ///< graphics pipeline library parts, by PartKey
        std::unordered_set<uint64_t>               queued_; ///< pipeline keys queued or under way
        std::unordered_set<uint64_t>               queued_parts_;
        std::vector<VertexLayout>                  layouts_;
        std::unordered_map<std::string, uint16_t>  layout_ids_; ///< by the layout's bytes
        struct Retired
        {
            uint64_t   frame;
            VkPipeline pipeline;
        };
        std::vector<Retired>                       retired_;
        uint64_t                                   frame_ = 0;
        Stats                                      stats_;

        // The keys file: what earlier launches drew, and what this one drew first.
        std::vector<Description>                   known_;
        std::unordered_multimap<uint64_t, size_t>  known_by_program_; ///< known_'s indices by each program's identity
        std::unordered_set<std::string>            known_bytes_;
        std::vector<Description>                   fresh_;
        std::unordered_map<uint64_t, uint16_t>     program_ids_[2]; ///< ids by identity: [vertex, pixel]

        // The workers.
        mutable std::mutex                         mutex_;
        std::condition_variable                    wake_;
        std::condition_variable                    idle_;
        std::deque<Job>                            jobs_;
        std::vector<Done>                          done_;
        std::vector<std::thread>                   workers_;
        uint32_t                                   working_ = 0;
        bool                                       stop_ = false;
    };
}
