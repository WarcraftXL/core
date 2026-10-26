// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The SPIR-V module writer.

#include "gx/vk/shaders/Spirv.hpp"

#include <cstring>

namespace wxl::vk::spirv
{
    namespace
    {
        // Keys of the cache that are not types or constants: a capability declared, an array decorated.
        constexpr uint32_t kCapabilityKey = 0xFFFFFFFEu;
        constexpr uint32_t kDecoratedKey  = 0xFFFFFFFFu;
        constexpr uint32_t kExtensionKey  = 0xFFFFFFFDu;
    }

    Module::Module(SpvExecutionModel model) : model_(model) { Capability(SpvCapabilityShader); }

    void Module::Emit(std::vector<uint32_t>& to, SpvOp op, std::initializer_list<uint32_t> head,
                      std::initializer_list<uint32_t> tail)
    {
        const uint32_t count = 1 + static_cast<uint32_t>(head.size() + tail.size());
        to.push_back((count << SpvWordCountShift) | static_cast<uint32_t>(op));
        to.insert(to.end(), head.begin(), head.end());
        to.insert(to.end(), tail.begin(), tail.end());
    }

    void Module::Emit(std::vector<uint32_t>& to, SpvOp op, std::initializer_list<uint32_t> head, const std::vector<uint32_t>& tail)
    {
        const uint32_t count = 1 + static_cast<uint32_t>(head.size() + tail.size());
        to.push_back((count << SpvWordCountShift) | static_cast<uint32_t>(op));
        to.insert(to.end(), head.begin(), head.end());
        to.insert(to.end(), tail.begin(), tail.end());
    }

    void Module::EmitString(std::vector<uint32_t>& to, std::string_view text)
    {
        // Nul-terminated, padded with nuls to a whole word.
        const size_t words = text.size() / 4 + 1;
        const size_t at    = to.size();
        to.resize(at + words, 0);
        std::memcpy(&to[at], text.data(), text.size());
    }

    void Module::Capability(SpvCapability capability)
    {
        if (!cache_.emplace(std::vector<uint32_t>{kCapabilityKey, static_cast<uint32_t>(capability)}, 0).second) return;
        Emit(capabilities_, SpvOpCapability, {static_cast<uint32_t>(capability)});
    }

    void Module::Extension(std::string_view name)
    {
        std::vector<uint32_t> words;
        EmitString(words, name);
        std::vector<uint32_t> key{kExtensionKey};
        key.insert(key.end(), words.begin(), words.end());
        if (!cache_.emplace(std::move(key), 0).second) return;
        extensions_.push_back(((1 + static_cast<uint32_t>(words.size())) << SpvWordCountShift) | SpvOpExtension);
        extensions_.insert(extensions_.end(), words.begin(), words.end());
    }

    Id Module::Cached(std::vector<uint32_t> key, SpvOp op, std::initializer_list<uint32_t> operands, bool type)
    {
        key.insert(key.begin(), static_cast<uint32_t>(op));
        auto found = cache_.find(key);
        if (found != cache_.end()) return found->second;
        const Id id = NewId();
        if (type) Emit(globals_, op, {id}, operands);
        else
        {
            // A constant: its type comes first, then its id.
            std::vector<uint32_t> words(operands);
            const uint32_t        count = 1 + static_cast<uint32_t>(words.size()) + 1;
            globals_.push_back((count << SpvWordCountShift) | static_cast<uint32_t>(op));
            globals_.push_back(words[0]);
            globals_.push_back(id);
            globals_.insert(globals_.end(), words.begin() + 1, words.end());
        }
        cache_.emplace(std::move(key), id);
        return id;
    }

    Id Module::Void() { return Cached({}, SpvOpTypeVoid, {}, true); }
    Id Module::Bool() { return Cached({}, SpvOpTypeBool, {}, true); }
    Id Module::Int() { return Cached({32, 1}, SpvOpTypeInt, {32, 1}, true); }
    Id Module::Uint() { return Cached({32, 0}, SpvOpTypeInt, {32, 0}, true); }
    Id Module::Float() { return Cached({32}, SpvOpTypeFloat, {32}, true); }
    Id Module::Vector(Id component, uint32_t count) { return Cached({component, count}, SpvOpTypeVector, {component, count}, true); }

    Id Module::Array(Id element, uint32_t length, uint32_t stride)
    {
        const Id size = ConstUint(length);
        const Id id   = Cached({element, size, stride}, SpvOpTypeArray, {element, size}, true);
        // Decorated once: a second request answers the same, already decorated, type.
        const std::vector<uint32_t> mark = {kDecoratedKey, id};
        if (cache_.emplace(mark, id).second) Decorate(id, SpvDecorationArrayStride, {stride});
        return id;
    }

    Id Module::PlainArray(Id element, uint32_t length)
    {
        const Id size = ConstUint(length);
        // Its own key: an array of the same element and length made with a stride is another type.
        return Cached({element, size, kDecoratedKey}, SpvOpTypeArray, {element, size}, true);
    }

    Id Module::RuntimeArray(Id element) { return Cached({element}, SpvOpTypeRuntimeArray, {element}, true); }

    Id Module::Struct(std::initializer_list<Id> members)
    {
        // Every struct is its own type: each carries its own member offsets.
        const Id id = NewId();
        Emit(globals_, SpvOpTypeStruct, {id}, members);
        return id;
    }

    Id Module::Struct(const std::vector<Id>& members)
    {
        const Id id = NewId();
        Emit(globals_, SpvOpTypeStruct, {id}, members);
        return id;
    }

    Id Module::Pointer(SpvStorageClass storage, Id pointee)
    {
        return Cached({static_cast<uint32_t>(storage), pointee}, SpvOpTypePointer, {static_cast<uint32_t>(storage), pointee}, true);
    }

    Id Module::Function(Id result, std::initializer_list<Id> parameters)
    {
        std::vector<uint32_t> key{static_cast<uint32_t>(SpvOpTypeFunction), result};
        key.insert(key.end(), parameters.begin(), parameters.end());
        auto found = cache_.find(key);
        if (found != cache_.end()) return found->second;
        const Id id = NewId();
        Emit(globals_, SpvOpTypeFunction, {id, result}, parameters);
        cache_.emplace(std::move(key), id);
        return id;
    }

    Id Module::Image2D(Id sampled) { return Image(sampled, SpvDim2D); }

    Id Module::Image(Id sampled, SpvDim dim)
    {
        // Not depth, not arrayed, single-sampled, used with a sampler, format unknown.
        return Cached({sampled, static_cast<uint32_t>(dim), 0, 0, 0, 1, SpvImageFormatUnknown}, SpvOpTypeImage,
                      {sampled, static_cast<uint32_t>(dim), 0, 0, 0, 1, SpvImageFormatUnknown}, true);
    }

    Id Module::Sampler() { return Cached({}, SpvOpTypeSampler, {}, true); }

    Id Module::SampledImage(Id image) { return Cached({image}, SpvOpTypeSampledImage, {image}, true); }

    Id Module::ConstInt(int32_t value)
    {
        const Id type = Int();
        return Cached({type, static_cast<uint32_t>(value)}, SpvOpConstant, {type, static_cast<uint32_t>(value)}, false);
    }

    Id Module::ConstUint(uint32_t value)
    {
        const Id type = Uint();
        return Cached({type, value}, SpvOpConstant, {type, value}, false);
    }

    Id Module::ConstFloat(float value)
    {
        const Id type = Float();
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        return Cached({type, bits}, SpvOpConstant, {type, bits}, false);
    }

    Id Module::ConstBool(bool value)
    {
        const Id type = Bool();
        const SpvOp op = value ? SpvOpConstantTrue : SpvOpConstantFalse;
        std::vector<uint32_t> key{static_cast<uint32_t>(op), type};
        auto found = cache_.find(key);
        if (found != cache_.end()) return found->second;
        const Id id = NewId();
        Emit(globals_, op, {type, id});
        cache_.emplace(std::move(key), id);
        return id;
    }

    Id Module::ConstComposite(Id type, std::initializer_list<Id> parts) { return ConstComposite(type, std::vector<Id>(parts)); }

    Id Module::ConstComposite(Id type, const std::vector<Id>& parts)
    {
        std::vector<uint32_t> key{static_cast<uint32_t>(SpvOpConstantComposite), type};
        key.insert(key.end(), parts.begin(), parts.end());
        auto found = cache_.find(key);
        if (found != cache_.end()) return found->second;
        const Id id = NewId();
        Emit(globals_, SpvOpConstantComposite, {type, id}, parts);
        cache_.emplace(std::move(key), id);
        return id;
    }

    Id Module::ConstNull(Id type)
    {
        std::vector<uint32_t> key{static_cast<uint32_t>(SpvOpConstantNull), type};
        auto found = cache_.find(key);
        if (found != cache_.end()) return found->second;
        const Id id = NewId();
        Emit(globals_, SpvOpConstantNull, {type, id});
        cache_.emplace(std::move(key), id);
        return id;
    }

    Id Module::Variable(Id pointer, SpvStorageClass storage)
    {
        const Id id = NewId();
        Emit(globals_, SpvOpVariable, {pointer, id, static_cast<uint32_t>(storage)});
        return id;
    }

    Id Module::Variable(Id pointer, SpvStorageClass storage, Id initializer)
    {
        const Id id = NewId();
        Emit(globals_, SpvOpVariable, {pointer, id, static_cast<uint32_t>(storage), initializer});
        return id;
    }

    void Module::Decorate(Id target, SpvDecoration decoration, std::initializer_list<uint32_t> operands)
    {
        Emit(annotations_, SpvOpDecorate, {target, static_cast<uint32_t>(decoration)}, operands);
    }

    void Module::MemberDecorate(Id type, uint32_t member, SpvDecoration decoration, std::initializer_list<uint32_t> operands)
    {
        Emit(annotations_, SpvOpMemberDecorate, {type, member, static_cast<uint32_t>(decoration)}, operands);
    }

    void Module::Name(Id target, std::string_view name)
    {
        std::vector<uint32_t> words;
        EmitString(words, name);
        names_.push_back(((2 + static_cast<uint32_t>(words.size())) << SpvWordCountShift) | SpvOpName);
        names_.push_back(target);
        names_.insert(names_.end(), words.begin(), words.end());
    }

    Id Module::Glsl()
    {
        if (glsl_) return glsl_;
        glsl_ = NewId();
        std::vector<uint32_t> words;
        EmitString(words, "GLSL.std.450");
        imports_.push_back(((2 + static_cast<uint32_t>(words.size())) << SpvWordCountShift) | SpvOpExtInstImport);
        imports_.push_back(glsl_);
        imports_.insert(imports_.end(), words.begin(), words.end());
        return glsl_;
    }

    void Module::BeginMain(std::initializer_list<Id> interface)
    {
        const Id voidType = Void();
        const Id fnType   = Function(voidType);
        main_             = NewId();
        interface_.assign(interface.begin(), interface.end());
        Name(main_, "main");

        Emit(head_, SpvOpFunction, {voidType, main_, SpvFunctionControlMaskNone, fnType});
        Emit(head_, SpvOpLabel, {NewId()});
    }

    void Module::AddInterface(Id variable)
    {
        for (uint32_t id : interface_)
            if (id == variable) return;
        interface_.push_back(variable);
    }

    void Module::ExecutionMode(SpvExecutionMode mode, std::initializer_list<uint32_t> operands)
    {
        Emit(modes_, SpvOpExecutionMode, {main_, static_cast<uint32_t>(mode)}, operands);
    }

    Id Module::Local(Id pointer)
    {
        const Id id = NewId();
        Emit(locals_, SpvOpVariable, {pointer, id, static_cast<uint32_t>(SpvStorageClassFunction)});
        return id;
    }

    Id Module::Op(SpvOp op, Id type, std::initializer_list<uint32_t> operands)
    {
        const Id id = NewId();
        Emit(code_, op, {type, id}, operands);
        return id;
    }

    Id Module::Op(SpvOp op, Id type, const std::vector<uint32_t>& operands)
    {
        const Id id = NewId();
        Emit(code_, op, {type, id}, operands);
        return id;
    }

    void Module::OpVoid(SpvOp op, std::initializer_list<uint32_t> operands) { Emit(code_, op, operands); }

    void Module::OpVoid(SpvOp op, const std::vector<uint32_t>& operands) { Emit(code_, op, {}, operands); }

    Id Module::Ext(Id type, uint32_t instruction, std::initializer_list<Id> operands)
    {
        const Id set = Glsl();
        const Id id  = NewId();
        Emit(code_, SpvOpExtInst, {type, id, set, instruction}, operands);
        return id;
    }

    void Module::Label(Id label) { Emit(code_, SpvOpLabel, {label}); }

    void Module::EndMain()
    {
        Emit(code_, SpvOpReturn, {});
        Emit(code_, SpvOpFunctionEnd, {});
    }

    std::vector<uint32_t> Module::Words() const
    {
        std::vector<uint32_t> name;
        EmitString(name, "main");

        std::vector<uint32_t> out;
        out.reserve(5 + capabilities_.size() + extensions_.size() + imports_.size() + 3 + 3 + name.size() + interface_.size() +
                    modes_.size() + names_.size() + annotations_.size() + globals_.size() + head_.size() + locals_.size() + code_.size());
        out.push_back(SpvMagicNumber);
        out.push_back(version_);
        out.push_back(0);     // generator: none registered
        out.push_back(next_); // the bound: every id is below it
        out.push_back(0);
        out.insert(out.end(), capabilities_.begin(), capabilities_.end());
        out.insert(out.end(), extensions_.begin(), extensions_.end());
        out.insert(out.end(), imports_.begin(), imports_.end());
        out.push_back((3u << SpvWordCountShift) | SpvOpMemoryModel);
        out.push_back(static_cast<uint32_t>(addressing_));
        out.push_back(SpvMemoryModelGLSL450);
        if (main_)
        {
            out.push_back(((3 + static_cast<uint32_t>(name.size() + interface_.size())) << SpvWordCountShift) | SpvOpEntryPoint);
            out.push_back(static_cast<uint32_t>(model_));
            out.push_back(main_);
            out.insert(out.end(), name.begin(), name.end());
            out.insert(out.end(), interface_.begin(), interface_.end());
        }
        out.insert(out.end(), modes_.begin(), modes_.end());
        out.insert(out.end(), names_.begin(), names_.end());
        out.insert(out.end(), annotations_.begin(), annotations_.end());
        out.insert(out.end(), globals_.begin(), globals_.end());
        out.insert(out.end(), head_.begin(), head_.end());
        out.insert(out.end(), locals_.begin(), locals_.end());
        out.insert(out.end(), code_.begin(), code_.end());
        return out;
    }
}
