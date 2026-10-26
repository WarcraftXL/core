// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A SPIR-V module written word by word, against SPIRV-Headers: the present pass's shaders are made
// this way at run time (Own.cpp), and the translator of the client's DX9 shaders (Translate.cpp)
// writes its output with it. Nothing is compiled from text, so no shader compiler ships or runs.
//
// The module keeps the sections in the order SPIR-V lays them out and assigns ids as they are
// asked for; types and constants asked for twice answer the same id. One function, `main`, the
// entry point: its local variables are gathered at the top of its first block, as SPIR-V wants
// them, whenever they are asked for.

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
        /// SPIR-V 1.5: Vulkan 1.2 and later; physical storage buffers are core, and an entry point's
        /// interface lists every global variable it uses.
        static constexpr uint32_t kVersion15 = 0x00010500;

        explicit Module(SpvExecutionModel model);

        Id NewId() { return next_++; }

        // --- The module's header ---------------------------------------------------------------------
        void SetVersion(uint32_t version) { version_ = version; }
        uint32_t Version() const { return version_; }
        /// Declared once, however often asked for.
        void Capability(SpvCapability capability);
        void Extension(std::string_view name);
        void Addressing(SpvAddressingModel model) { addressing_ = model; }

        // --- Types, each made once ---------------------------------------------------------------
        Id Void();
        Id Bool();
        Id Int();   ///< signed 32-bit
        Id Uint();  ///< unsigned 32-bit
        Id Float(); ///< 32-bit
        Id Vector(Id component, uint32_t count);
        Id Array(Id element, uint32_t length, uint32_t stride);
        /// An array without stride decoration (of descriptors, of interface variables, of locals).
        Id PlainArray(Id element, uint32_t length);
        Id RuntimeArray(Id element);
        Id Struct(std::initializer_list<Id> members);
        Id Struct(const std::vector<Id>& members);
        Id Pointer(SpvStorageClass storage, Id pointee);
        Id Function(Id result, std::initializer_list<Id> parameters = {});
        Id Image2D(Id sampled);       ///< a 2D image sampled through a sampler, of @p sampled components
        Id Image(Id sampled, SpvDim dim);
        Id Sampler();
        Id SampledImage(Id image);

        // --- Constants, each made once --------------------------------------------------------------
        Id ConstInt(int32_t value);
        Id ConstUint(uint32_t value);
        Id ConstFloat(float value);
        Id ConstBool(bool value);
        Id ConstComposite(Id type, std::initializer_list<Id> parts);
        Id ConstComposite(Id type, const std::vector<Id>& parts);
        Id ConstNull(Id type);

        // --- Globals and their decorations ----------------------------------------------------------
        Id Variable(Id pointer, SpvStorageClass storage);
        /// A global with its initial value (a Private variable, a constant array to index).
        Id Variable(Id pointer, SpvStorageClass storage, Id initializer);
        void Decorate(Id target, SpvDecoration decoration, std::initializer_list<uint32_t> operands = {});
        void MemberDecorate(Id type, uint32_t member, SpvDecoration decoration, std::initializer_list<uint32_t> operands = {});
        void Name(Id target, std::string_view name);

        /// The GLSL.std.450 extended instructions, imported on first use.
        Id Glsl();

        // --- The entry point's function -------------------------------------------------------------
        /// Opens `main`, the module's entry point over @p interface (more may be added until Words).
        void BeginMain(std::initializer_list<Id> interface);
        void AddInterface(Id variable);
        void ExecutionMode(SpvExecutionMode mode, std::initializer_list<uint32_t> operands = {});
        /// A variable of main (Function storage), placed at the top of its first block.
        Id Local(Id pointer);
        /// An instruction with a result: its new id.
        Id Op(SpvOp op, Id type, std::initializer_list<uint32_t> operands);
        Id Op(SpvOp op, Id type, const std::vector<uint32_t>& operands);
        /// An instruction without a result.
        void OpVoid(SpvOp op, std::initializer_list<uint32_t> operands);
        void OpVoid(SpvOp op, const std::vector<uint32_t>& operands);
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
        static void Emit(std::vector<uint32_t>& to, SpvOp op, std::initializer_list<uint32_t> head, const std::vector<uint32_t>& tail);
        static void EmitString(std::vector<uint32_t>& to, std::string_view text);
        Id Cached(std::vector<uint32_t> key, SpvOp op, std::initializer_list<uint32_t> operands, bool type);

        SpvExecutionModel  model_;
        uint32_t           version_ = kVersion;
        SpvAddressingModel addressing_ = SpvAddressingModelLogical;
        Id                 next_ = 1;
        Id                 glsl_ = 0;
        Id                 main_ = 0;

        std::vector<uint32_t> capabilities_;
        std::vector<uint32_t> extensions_;
        std::vector<uint32_t> imports_;
        std::vector<uint32_t> interface_;
        std::vector<uint32_t> modes_;
        std::vector<uint32_t> names_;
        std::vector<uint32_t> annotations_;
        std::vector<uint32_t> globals_; ///< types, constants and variables, in the order made
        std::vector<uint32_t> head_;    ///< main's OpFunction and first label
        std::vector<uint32_t> locals_;  ///< main's variables
        std::vector<uint32_t> code_;

        std::map<std::vector<uint32_t>, Id> cache_;
    };
}
