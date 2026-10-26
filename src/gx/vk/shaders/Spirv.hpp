// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A SPIR-V module written word by word, against SPIRV-Headers: the renderer's own shaders are made
// this way at run time (Own.cpp), as the translator of the client's DX9 shaders will write its
// output. Nothing is compiled from text, so no shader compiler ships or runs.
//
// The module keeps the sections in the order SPIR-V lays them out and assigns ids as they are
// asked for; types and constants asked for twice answer the same id.

#pragma once

#include <spirv/unified1/spirv.h>

#include <cstdint>
#include <initializer_list>
#include <map>
#include <string_view>
#include <vector>

namespace wxl::vk::spirv
{
    using Id = uint32_t;

    class Module
    {
    public:
        /// SPIR-V 1.3: what every Vulkan 1.1 and later device takes, interfaces listing inputs and outputs only.
        static constexpr uint32_t kVersion = 0x00010300;

        explicit Module(SpvExecutionModel model);

        Id NewId() { return next_++; }

        // --- Types, each made once ---------------------------------------------------------------
        Id Void();
        Id Bool();
        Id Int();   ///< signed 32-bit
        Id Uint();  ///< unsigned 32-bit
        Id Float(); ///< 32-bit
        Id Vector(Id component, uint32_t count);
        Id Array(Id element, uint32_t length, uint32_t stride);
        Id Struct(std::initializer_list<Id> members);
        Id Pointer(SpvStorageClass storage, Id pointee);
        Id Function(Id result, std::initializer_list<Id> parameters = {});
        Id Image2D(Id sampled);       ///< a 2D image sampled through a sampler, of @p sampled components
        Id SampledImage(Id image);

        // --- Constants, each made once --------------------------------------------------------------
        Id ConstInt(int32_t value);
        Id ConstUint(uint32_t value);
        Id ConstFloat(float value);
        Id ConstComposite(Id type, std::initializer_list<Id> parts);

        // --- Globals and their decorations ----------------------------------------------------------
        Id Variable(Id pointer, SpvStorageClass storage);
        void Decorate(Id target, SpvDecoration decoration, std::initializer_list<uint32_t> operands = {});
        void MemberDecorate(Id type, uint32_t member, SpvDecoration decoration, std::initializer_list<uint32_t> operands = {});
        void Name(Id target, std::string_view name);

        /// The GLSL.std.450 extended instructions, imported on first use.
        Id Glsl();

        // --- The entry point's function -------------------------------------------------------------
        /// Opens `main`, the module's entry point over @p interface (its inputs and outputs).
        void BeginMain(std::initializer_list<Id> interface);
        void ExecutionMode(SpvExecutionMode mode, std::initializer_list<uint32_t> operands = {});
        /// An instruction with a result: its new id.
        Id Op(SpvOp op, Id type, std::initializer_list<uint32_t> operands);
        /// An instruction without a result.
        void OpVoid(SpvOp op, std::initializer_list<uint32_t> operands);
        /// A GLSL.std.450 instruction.
        Id Ext(Id type, uint32_t instruction, std::initializer_list<Id> operands);
        /// Starts the block @p label (an id from NewId) inside main.
        void Label(Id label);
        void EndMain();

        /// The finished module, header first.
        std::vector<uint32_t> Words() const;

    private:
        static void Emit(std::vector<uint32_t>& to, SpvOp op, std::initializer_list<uint32_t> head,
                         std::initializer_list<uint32_t> tail = {});
        static void EmitString(std::vector<uint32_t>& to, std::string_view text);
        Id Cached(std::vector<uint32_t> key, SpvOp op, std::initializer_list<uint32_t> operands, bool type);

        SpvExecutionModel model_;
        Id                next_ = 1;
        Id                glsl_ = 0;
        Id                main_ = 0;

        std::vector<uint32_t> capabilities_;
        std::vector<uint32_t> imports_;
        std::vector<uint32_t> entry_;
        std::vector<uint32_t> modes_;
        std::vector<uint32_t> names_;
        std::vector<uint32_t> annotations_;
        std::vector<uint32_t> globals_; ///< types, constants and variables, in the order made
        std::vector<uint32_t> code_;

        std::map<std::vector<uint32_t>, Id> cache_;
    };
}
