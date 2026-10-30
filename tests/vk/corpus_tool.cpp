// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The translator over a client's whole shader corpus: every record of every container under a
// folder laid out as the archives' Shaders\ (<folder>/<Vertex|Pixel>/<profile>/<name>.bls, the
// D3D9 profiles vs_1_1..vs_3_0 and ps_1_1..ps_3_0), translated, held to spirv-val's Vulkan 1.3
// rules, and with --device made into a module and a pipeline on this machine's driver. The corpus
// is the user's own (extracted from their archives); it never enters a repository.
//
//   wxl-vk-corpus <folder> [--device] [--verbose]
//
// Prints the counts, each refusal's reason with how many records it covers, and exits 1 when a
// record fails to translate or validate.

#include "gx/vk/shaders/Translate.hpp"

#include <spirv-tools/libspirv.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace wxl::vk::test
{
    /// Makes each module (and a pipeline for it) on the machine's driver; defined in corpus_device.cpp.
    bool CorpusDevice(const std::vector<std::vector<uint32_t>>& modules, const std::vector<wxl::vk::shaders::ProgramInfo>& infos,
                      uint32_t& made, uint32_t& failed);
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: wxl-vk-corpus <folder> [--device] [--verbose]\n");
        return 2;
    }
    bool device = false, verbose = false;
    for (int i = 2; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--device")) device = true;
        if (!std::strcmp(argv[i], "--verbose")) verbose = true;
    }
    static const std::set<std::string> kProfiles = {"vs_1_1", "vs_2_0", "vs_3_0", "ps_1_1", "ps_1_4", "ps_2_0", "ps_3_0"};

    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::string          message;
    tools.SetMessageConsumer([&message](spv_message_level_t, const char*, const spv_position_t& at, const char* text) {
        message += std::to_string(at.index) + ": " + text + "\n";
    });
    spvtools::ValidatorOptions options;

    uint32_t records = 0, translated = 0, valid = 0;
    std::set<std::vector<uint8_t>>            seen;
    std::map<std::string, uint32_t>           refused;
    std::map<std::string, std::string>        first;
    std::vector<std::vector<uint32_t>>        modules;
    std::vector<wxl::vk::shaders::ProgramInfo> infos;
    std::map<std::string, uint32_t>           per_profile;
    size_t words = 0;
    double spent = 0.0; ///< milliseconds in the translator

    for (const auto& entry : fs::recursive_directory_iterator(argv[1]))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".bls") continue;
        const std::string profile = entry.path().parent_path().filename().string();
        if (!kProfiles.count(profile)) continue;
        std::ifstream         in(entry.path(), std::ios::binary);
        std::vector<uint8_t>  data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.size() < 12) continue;
        uint32_t count = 0;
        std::memcpy(&count, data.data() + 8, 4);
        size_t at = 12;
        for (uint32_t r = 0; r < count && at + 16 <= data.size(); ++r)
        {
            uint32_t size = 0;
            std::memcpy(&size, data.data() + at + 12, 4);
            at += 16;
            if (at + size > data.size()) break;
            std::vector<uint8_t> code(data.begin() + static_cast<ptrdiff_t>(at), data.begin() + static_cast<ptrdiff_t>(at + size));
            at = (at + size + 3) & ~size_t(3);
            if (!seen.insert(code).second) continue;
            ++records;
            ++per_profile[profile];
            wxl::vk::shaders::Translation t;
            std::string                   why;
            const std::string name = entry.path().filename().string() + ":" + std::to_string(r) + " (" + profile + ")";
            const auto begin = std::chrono::steady_clock::now();
            const bool made  = wxl::vk::shaders::Translate(code.data(), code.size(), {}, t, why);
            spent += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            if (!made)
            {
                ++refused[why];
                first.emplace(why, name);
                continue;
            }
            ++translated;
            message.clear();
            if (!tools.Validate(t.words.data(), t.words.size(), options))
            {
                const std::string key = "spirv-val: " + message.substr(0, message.find('\n'));
                ++refused[key];
                if (first.emplace(key, name).second && verbose)
                {
                    std::string text;
                    tools.Disassemble(t.words, &text);
                    std::printf("---- %s\n%s\n%s\n", name.c_str(), message.c_str(), text.c_str());
                }
                continue;
            }
            ++valid;
            words += t.words.size();
            if (device)
            {
                modules.push_back(std::move(t.words));
                infos.push_back(t.info);
            }
        }
    }
    std::printf("%u distinct records:", records);
    for (const auto& p : per_profile) std::printf(" %s %u", p.first.c_str(), p.second);
    std::printf("\n%u translated in %.0f ms (%.3f ms each), %u pass spirv-val (%zu words, %.1f KiB on average)\n", translated, spent,
                records ? spent / records : 0.0, valid, words, valid ? static_cast<double>(words) * 4.0 / 1024.0 / valid : 0.0);
    for (const auto& r : refused) std::printf("  %u x %s (first: %s)\n", r.second, r.first.c_str(), first[r.first].c_str());
    bool ok = valid == records;
    if (device)
    {
        uint32_t made = 0, failed = 0;
        if (!wxl::vk::test::CorpusDevice(modules, infos, made, failed))
        {
            std::printf("no Vulkan device\n");
            return 77;
        }
        std::printf("%u modules and pipelines made on the driver, %u failed\n", made, failed);
        ok = ok && failed == 0;
    }
    return ok ? 0 : 1;
}
