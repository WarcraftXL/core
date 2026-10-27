// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.gx's shader providers (see ShaderCode.hpp).

#include "services/ShaderCode.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"

#include <cstring>

namespace
{
    using namespace wxl::core;

    constexpr const char* kTag = "gx";
    /// The largest program a provider may answer: far above any the client ships (a few KiB).
    constexpr uint32_t kMostBytes = 1u << 20;

    struct Provider
    {
        uint32_t         mod;
        wxl_gx_shader_fn fn;
        void*            user;
    };
    std::vector<Provider> g_providers;

    /// The record whose code BeforeCreate swapped, with the client's own to put back.
    struct Swap
    {
        void*                shader = nullptr;
        const void*          code   = nullptr;
        uint32_t             size   = 0;
        std::vector<uint8_t> answer;
    } g_swap;

    services::shader_code::Counters g_counters;
    bool                            g_saidSize = false;
}

namespace wxl::core::services::shader_code
{
    wxl_result SetProvider(uint32_t mod, const wxl_gx_shader_provider* provider)
    {
        if (provider && (provider->struct_size < sizeof(wxl_gx_shader_provider) || !provider->fn)) return WXL_E_INVALID;
        for (Provider& p : g_providers)
            if (p.mod == mod)
            {
                if (provider)
                {
                    p.fn   = provider->fn;
                    p.user = provider->user;
                }
                else
                    ForgetMod(mod);
                return WXL_OK;
            }
        if (provider) g_providers.push_back(Provider{mod, provider->fn, provider->user});
        return WXL_OK;
    }

    void ForgetMod(uint32_t mod)
    {
        for (size_t i = g_providers.size(); i-- > 0;)
            if (g_providers[i].mod == mod) g_providers.erase(g_providers.begin() + static_cast<std::ptrdiff_t>(i));
    }

    void Clear()
    {
        g_providers.clear();
        g_swap     = Swap{};
        g_counters = Counters{};
    }

    bool Answer(const char* name, uint32_t stage, const void* code, uint32_t size, std::vector<uint8_t>& out)
    {
        if (g_providers.empty() || !code || !size) return false;
        ++g_counters.asked;
        std::vector<uint8_t> current(static_cast<const uint8_t*>(code), static_cast<const uint8_t*>(code) + size);
        bool                 answered = false;
        for (const Provider& p : std::vector<Provider>(g_providers))
        {
            wxl_gx_shader_code in{};
            in.struct_size = sizeof in;
            in.name        = name ? name : "";
            in.stage       = stage;
            in.code        = current.data();
            in.size        = static_cast<uint32_t>(current.size());
            wxl_gx_shader_answer answer{};
            answer.struct_size = sizeof answer;
            if (p.fn(p.user, &in, &answer) != WXL_TRUE || !answer.code || !answer.size) continue;
            if (answer.size > kMostBytes || answer.size % 4)
            {
                if (!g_saidSize)
                    WLOG_WARN(kTag, "a shader provider answered %u bytes for %s: DX9 bytecode is whole dwords, at most 1 MiB; the program is made as before",
                              answer.size, in.name);
                g_saidSize = true;
                continue;
            }
            current.assign(static_cast<const uint8_t*>(answer.code), static_cast<const uint8_t*>(answer.code) + answer.size);
            answered = true;
        }
        if (!answered) return false;
        ++g_counters.answered;
        out = std::move(current);
        return true;
    }

    void BeforeCreate(void* shader)
    {
        g_swap = Swap{};
        if (g_providers.empty() || !shader) return;
        wxl_core_shader_record record{};
        if (binding::gx_shader_record(shader, &record) != WXL_TRUE) return;
        const uint32_t stage = record.target == 0 ? 0u : 1u;
        std::vector<uint8_t> answer;
        if (!Answer(record.name, stage, record.code, record.size, answer)) return;
        g_swap.shader = shader;
        g_swap.code   = record.code;
        g_swap.size   = record.size;
        g_swap.answer = std::move(answer);
        binding::gx_shader_code_set(shader, g_swap.answer.data(), static_cast<uint32_t>(g_swap.answer.size()));
    }

    void AfterCreate(void* shader)
    {
        if (!g_swap.shader || g_swap.shader != shader) return;
        binding::gx_shader_code_set(shader, g_swap.code, g_swap.size);
        g_swap = Swap{};
    }

    Counters Count() { return g_counters; }
}
