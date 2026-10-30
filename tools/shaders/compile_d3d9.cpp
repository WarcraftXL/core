// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl-core-d3d9-shaders: compiles the runtime's own Direct3D 9 programs when it is built.
//
//   wxl-core-d3d9-shaders <shaders.txt> <out.inc>
//
// Each line of shaders.txt names a program (see the file); each is compiled from its HLSL with the
// system's d3dcompiler_47.dll (Windows' own; Wine's under Wine), then held to what the line allows:
// the bytecode's version is the profile's, its instructions end where the end token is, the float
// constants it reads from the device are within the line's range (its own `def` literals aside),
// it addresses no constant relatively and uses no more samplers than the line says. Everything
// passing is written as `static const uint32_t k<Name>[]` arrays into <out.inc>, with the
// disassembly beside it (<out.inc>.asm) for a reader, and the count of its instructions as
// k<Name>Slots, which the runtime holds to the device's caps before it uses the program. Anything failing fails the build, by name.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // d3dcompiler.h's entry points, reached through the DLL so that nothing links against it.
    struct Blob
    {
        virtual HRESULT __stdcall QueryInterface(const IID&, void**) = 0;
        virtual ULONG __stdcall AddRef()                             = 0;
        virtual ULONG __stdcall Release()                            = 0;
        virtual void* __stdcall GetBufferPointer()                   = 0;
        virtual SIZE_T __stdcall GetBufferSize()                     = 0;
    };
    using Compile     = HRESULT(WINAPI*)(const void* source, SIZE_T size, const char* name, const void* defines,
                                     void* include, const char* entry, const char* target, UINT flags1,
                                     UINT flags2, Blob** code, Blob** errors);
    using Disassemble = HRESULT(WINAPI*)(const void* code, SIZE_T size, UINT flags, const char* comments,
                                         Blob** text);

    constexpr UINT kOptimizationLevel3 = 1u << 15; // D3DCOMPILE_OPTIMIZATION_LEVEL3

    struct Program
    {
        std::string name, file, profile, entry;
        int         constLow = -1, constHigh = -1; ///< -1: none
        int         samplers = 0;
        int         line     = 0;
    };

    bool Fail(const std::string& what)
    {
        std::fprintf(stderr, "wxl-core-d3d9-shaders: %s\n", what.c_str());
        return false;
    }

    std::string Folder(const std::string& path)
    {
        const size_t slash = path.find_last_of("\\/");
        return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    }

    bool ReadFile(const std::string& path, std::string& out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream text;
        text << in.rdbuf();
        out = text.str();
        return true;
    }

    bool ParseRange(const std::string& text, int& low, int& high)
    {
        if (text == "-") return true;
        int a = -1, b = -1;
        if (std::sscanf(text.c_str(), "c%d-c%d", &a, &b) != 2 || a < 0 || b < a) return false;
        low  = a;
        high = b;
        return true;
    }

    bool ReadList(const std::string& path, std::vector<Program>& out)
    {
        std::string text;
        if (!ReadFile(path, text)) return Fail("cannot read " + path);
        std::istringstream lines(text);
        std::string        line;
        int                number = 0;
        while (std::getline(lines, line))
        {
            ++number;
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            std::istringstream words(line);
            Program            p;
            std::string        constants;
            if (!(words >> p.name)) continue;
            if (!(words >> p.file >> p.profile >> p.entry >> constants >> p.samplers) ||
                !ParseRange(constants, p.constLow, p.constHigh))
                return Fail(path + ":" + std::to_string(number) + ": expected <name> <file> <profile> <entry> <cA-cB|-> <samplers>");
            p.line = number;
            out.push_back(p);
        }
        return !out.empty() || Fail(path + " lists no program");
    }

    uint32_t VersionToken(const std::string& profile)
    {
        if (profile.size() != 6 || profile[2] != '_' || profile[4] != '_') return 0;
        const uint32_t major = static_cast<uint32_t>(profile[3] - '0');
        const uint32_t minor = static_cast<uint32_t>(profile[5] - '0');
        if (profile.compare(0, 2, "ps") == 0) return 0xFFFF0000u | (major << 8) | minor;
        if (profile.compare(0, 2, "vs") == 0) return 0xFFFE0000u | (major << 8) | minor;
        return 0;
    }

    /// The register type of a parameter token (D3DSHADER_PARAM_REGISTER_TYPE).
    uint32_t RegisterType(uint32_t token) { return ((token >> 28) & 0x7u) | ((token >> 8) & 0x18u); }

    constexpr uint32_t kConst    = 2;  // D3DSPR_CONST
    constexpr uint32_t kSampler  = 10; // D3DSPR_SAMPLER
    constexpr uint32_t kRelative = 1u << 13; // D3DSHADER_ADDRMODE_RELATIVE
    constexpr uint32_t kOpDcl = 31, kOpDefB = 47, kOpDefI = 48, kOpDef = 81, kOpComment = 0xFFFE, kOpEnd = 0xFFFF;

    /// Walks the bytecode: the instructions fill it up to the end token, and what it reads from
    /// the device stays within the program's line.
    bool Validate(const Program& p, const uint32_t* code, size_t count, uint32_t& slots)
    {
        slots = 0;
        const std::string at = p.name + " (" + p.profile + " " + p.entry + ")";
        if (count < 2 || code[0] != VersionToken(p.profile)) return Fail(at + ": the bytecode's version is not the profile's");
        std::set<uint32_t> defined, read, samplers;
        size_t             i = 1;
        for (;;)
        {
            if (i >= count) return Fail(at + ": no end token");
            const uint32_t token  = code[i];
            const uint32_t opcode = token & 0xFFFFu;
            if (token == kOpEnd)
            {
                ++i;
                break;
            }
            if (opcode == kOpComment)
            {
                i += 1 + ((token >> 16) & 0x7FFFu);
                continue;
            }
            const uint32_t length = (token >> 24) & 0xFu;
            if (i + length >= count) return Fail(at + ": an instruction runs past the end");
            const uint32_t* params = code + i + 1;
            if (opcode == kOpDef || opcode == kOpDefI || opcode == kOpDefB)
            {
                if (opcode == kOpDef && RegisterType(params[0]) == kConst) defined.insert(params[0] & 0x7FFu);
            }
            else if (opcode == kOpDcl)
            {
                if (length >= 2 && RegisterType(params[1]) == kSampler) samplers.insert(params[1] & 0x7FFu);
            }
            else
            {
                ++slots;
                for (uint32_t k = 0; k < length; ++k)
                {
                    const uint32_t param = params[k];
                    if (RegisterType(param) == kConst)
                    {
                        if (param & kRelative) return Fail(at + ": addresses a constant relatively");
                        read.insert(param & 0x7FFu);
                    }
                    if (param & kRelative) ++k; // the relative address token
                }
            }
            i += 1 + length;
        }
        if (i != count) return Fail(at + ": bytes after the end token");
        for (uint32_t reg : read)
        {
            if (defined.count(reg)) continue;
            if (static_cast<int>(reg) < p.constLow || static_cast<int>(reg) > p.constHigh)
                return Fail(at + ": reads c" + std::to_string(reg) + ", outside what its line allows");
        }
        if (static_cast<int>(samplers.size()) > p.samplers)
            return Fail(at + ": uses " + std::to_string(samplers.size()) + " samplers, its line allows " +
                        std::to_string(p.samplers));
        return true;
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: wxl-core-d3d9-shaders <shaders.txt> <out.inc>\n");
        return 2;
    }
    const std::string listPath = argv[1], outPath = argv[2];
    std::vector<Program> programs;
    if (!ReadList(listPath, programs)) return 1;

    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    if (!compiler) return Fail("d3dcompiler_47.dll is not on this system"), 1;
    // Through void*: an export's address is a FARPROC, whatever the function's own type.
    const auto compile     = reinterpret_cast<Compile>(reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile")));
    const auto disassemble = reinterpret_cast<Disassemble>(reinterpret_cast<void*>(GetProcAddress(compiler, "D3DDisassemble")));
    if (!compile || !disassemble) return Fail("d3dcompiler_47.dll lacks D3DCompile or D3DDisassemble"), 1;

    std::ostringstream inc, asmText;
    inc << "// GENERATED by wxl-core-d3d9-shaders from " << listPath << ": do not edit.\n"
        << "// Each array is a compiled Direct3D 9 program, held to its line of shaders.txt.\n\n";
    bool ok = true;
    for (const Program& p : programs)
    {
        std::string source;
        const std::string file = Folder(listPath) + "\\" + p.file;
        if (!ReadFile(file, source) && !ReadFile(Folder(listPath) + "/" + p.file, source))
        {
            ok = Fail("cannot read " + file);
            continue;
        }
        Blob*         code   = nullptr;
        Blob*         errors = nullptr;
        const HRESULT hr     = compile(source.data(), source.size(), p.file.c_str(), nullptr, nullptr, p.entry.c_str(),
                                       p.profile.c_str(), kOptimizationLevel3, 0, &code, &errors);
        if (FAILED(hr) || !code)
        {
            std::string why = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                                     : std::string("no message");
            ok = Fail(p.name + " (" + p.profile + " " + p.entry + ") does not compile: " + why);
            if (errors) errors->Release();
            continue;
        }
        if (errors) errors->Release();
        const auto*  words = static_cast<const uint32_t*>(code->GetBufferPointer());
        const size_t count = code->GetBufferSize() / 4;
        uint32_t     slots = 0;
        if (code->GetBufferSize() % 4 != 0 || !Validate(p, words, count, slots))
        {
            ok = false;
            code->Release();
            continue;
        }
        Blob* text = nullptr;
        if (SUCCEEDED(disassemble(words, code->GetBufferSize(), 0, nullptr, &text)) && text)
        {
            asmText << "// " << p.name << " (" << p.profile << " " << p.entry << ")\n"
                    << std::string(static_cast<const char*>(text->GetBufferPointer()),
                                   strnlen(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize()))
                    << "\n";
            text->Release();
        }
        else
        {
            ok = Fail(p.name + ": its bytecode does not disassemble");
        }
        inc << "// " << p.profile << " " << p.entry << ", " << count << " words\n"
            << "static const uint32_t k" << p.name << "[] = {";
        for (size_t i = 0; i < count; ++i)
        {
            char word[16];
            std::snprintf(word, sizeof word, "0x%08Xu", words[i]);
            inc << (i % 8 == 0 ? "\n    " : " ") << word << (i + 1 < count ? "," : "");
        }
        inc << "};\n"
            << "/// Its instructions: what the device's caps must allow a program of its model.\n"
            << "static const uint32_t k" << p.name << "Slots = " << slots << "u;\n\n";
        code->Release();
    }
    if (!ok) return 1;

    // Written only when it changes, so that what includes it is not rebuilt for nothing.
    std::string before;
    if (!ReadFile(outPath, before) || before != inc.str())
    {
        std::ofstream out(outPath, std::ios::binary);
        out << inc.str();
        if (!out) return Fail("cannot write " + outPath), 1;
    }
    std::ofstream(outPath + ".asm", std::ios::binary) << asmText.str();
    return 0;
}
