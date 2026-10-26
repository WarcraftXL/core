// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Every program the renderer makes, held to SPIRV-Tools' validator (spirv-val) for Vulkan 1.3:
// the programs of the engine's draws, compiled from GLSL when the renderer is built
// (pipelines/Pipelines.cpp), and the present pass's, written at run time by shaders/Spirv.hpp.
// SPIRV-Tools (deps/spirv-tools) is built in the native test tree only (WXL_VK_SPIRV_VAL).

#include "gx/vk/pipelines/Pipelines.hpp"
#include "gx/vk/shaders/Own.hpp"

#include "Check.hpp"

#include <cstdio>
#include <string>
#include <vector>

#if defined(WXL_VK_SPIRV_VAL)
#include <spirv-tools/libspirv.hpp>
#endif

namespace wxl::vk::test
{
    bool SpirvValidation()
    {
#if defined(WXL_VK_SPIRV_VAL)
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
        std::string          message;
        tools.SetMessageConsumer([&message](spv_message_level_t, const char*, const spv_position_t& at, const char* text) {
            message += std::to_string(at.index) + ": " + text + "\n";
        });
        auto validate = [&](const uint32_t* words, size_t count, const char* name) {
            message.clear();
            const bool ok = tools.Validate(words, count);
            if (!ok) std::printf("  %s: %s", name, message.c_str());
            char what[96];
            std::snprintf(what, sizeof what, "spirv-val passes %s", name);
            Check(ok, what);
        };
        static const char* const kVertex[] = {"ff.vert", "ui.vert", "ui.vert (stereo)"};
        for (int i = 0; i < static_cast<int>(pipelines::VertexProgram::Count); ++i)
        {
            size_t          words = 0;
            const uint32_t* code  = pipelines::VertexCode(static_cast<pipelines::VertexProgram>(i), words);
            validate(code, words, kVertex[i]);
        }
        static const char* const kPixel[] = {"ff.frag", "ui.frag", "desaturate.frag"};
        for (int i = 0; i < static_cast<int>(pipelines::PixelProgram::Count); ++i)
        {
            size_t          words = 0;
            const uint32_t* code  = pipelines::PixelCode(static_cast<pipelines::PixelProgram>(i), words);
            validate(code, words, kPixel[i]);
        }
        const std::vector<uint32_t> present = shaders::PresentVertex();
        validate(present.data(), present.size(), "the present pass's vertex shader");
        const std::vector<uint32_t> scene = shaders::PresentFragment(false);
        validate(scene.data(), scene.size(), "the present pass's fragment shader");
        const std::vector<uint32_t> cursor = shaders::PresentFragment(true);
        validate(cursor.data(), cursor.size(), "the present pass's cursor fragment shader");
        return true;
#else
        return false;
#endif
    }
}
