// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.highlight: the highlights mods set, one per mod and object, kept here; each world pass finds
// their objects' top models and hands them to the renderer, which keeps the batches drawn of
// them and draws the highlights after the world. On the stock client the renderer is the D3D9
// one (gx/d3d9/Highlight.cpp); the recompiled client has none until its Vulkan renderer has its
// render points, and says so through the contract.

#include "services/HighlightService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "objects/Objects.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/mod_host.hpp"

#if defined(_M_IX86)
    #include "gx/d3d9/Highlight.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
    using namespace wxl::core;

    struct Entry
    {
        uint32_t   mod;
        wxl_handle target;
        uint32_t   color;
        float      outline, glow, strength;
        uint64_t   order; ///< the later set wins where two mods highlight one object
    };

    std::vector<Entry> g_entries;
    uint64_t           g_order     = 0;
    bool               g_resolve   = false; ///< a world pass with highlights started: find their models
    bool               g_capturing = false; ///< and some are in view: their batches are kept
    uint32_t           g_drawn     = 0;

#if !defined(_M_IX86)
    constexpr const char* kNoRenderer =
        "the recompiled client draws highlights through its Vulkan renderer's render points, which it does not have yet";
#endif

    const char* Refusal()
    {
#if defined(_M_IX86)
        return gx::d3d9::highlight::Unsupported();
#else
        return kNoRenderer;
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
        if (!said.exchange(true))
            WLOG_WARN(kTag, "wxl.highlight.%s was called off the main thread, where the world is drawn; it was refused", call);
        return false;
    }

    wxl_bool WXL_CALL Supported() { return Refusal() ? WXL_FALSE : WXL_TRUE; }

    wxl_str WXL_CALL UnsupportedReason()
    {
        const char* why = Refusal();
        if (!why) why = "";
        return wxl_str{why, static_cast<uint32_t>(std::strlen(why))};
    }

    wxl_result WXL_CALL Set(wxl_handle target, uint32_t color, float outline, float glow, float strength)
    {
        if (!OnGameThread("Set")) return WXL_E_INVALID;
        if (Refusal()) return WXL_E_UNSUPPORTED;
        if (!std::isfinite(outline) || !std::isfinite(glow) || !std::isfinite(strength) || outline < 0.0f || glow < 0.0f ||
            strength < 0.0f)
            return WXL_E_INVALID;
        if (!objects::Service().Handles().Resolve(target)) return WXL_E_NOT_FOUND;
        const uint32_t mod = Caller();
        Entry          entry{mod, target, color & 0xFFFFFFu, std::min(outline, WXL_HIGHLIGHT_MAX_OUTLINE),
                    std::min(glow, WXL_HIGHLIGHT_MAX_GLOW), std::min(strength, 1.0f), ++g_order};
        for (Entry& held : g_entries)
            if (held.mod == mod && held.target == target)
            {
                held = entry;
                return WXL_OK;
            }
        if (g_entries.size() >= WXL_HIGHLIGHT_MAX_OBJECTS) return WXL_E_INVALID;
        g_entries.push_back(entry);
        return WXL_OK;
    }

    wxl_result WXL_CALL ClearOne(wxl_handle target)
    {
        if (!OnGameThread("Clear")) return WXL_E_INVALID;
        const uint32_t mod = Caller();
        const auto     at  = std::find_if(g_entries.begin(), g_entries.end(),
                                          [&](const Entry& e) { return e.mod == mod && e.target == target; });
        if (at == g_entries.end()) return WXL_E_NOT_FOUND;
        g_entries.erase(at);
        return WXL_OK;
    }

    void WXL_CALL ClearAll()
    {
        if (!OnGameThread("ClearAll")) return;
        services::highlight::ForgetMod(Caller());
    }

    uint32_t WXL_CALL Drawn() { return g_drawn; }
}

namespace wxl::core::services
{
    const wxl_highlight_api& HighlightApi()
    {
        static const wxl_highlight_api api = [] {
            wxl_highlight_api table{};
            table.struct_size        = sizeof table;
            table.supported          = &Supported;
            table.unsupported_reason = &UnsupportedReason;
            table.set                = &Set;
            table.clear              = &ClearOne;
            table.clear_all          = &ClearAll;
            table.drawn              = &Drawn;
            return table;
        }();
        return api;
    }

    namespace highlight
    {
#if defined(_M_IX86)
        namespace renderer = gx::d3d9::highlight;

        /// The styles of this pass, one per top model in view, in the order handed to the renderer.
        std::vector<renderer::Style> g_styles;

        /// The highlighted objects' top models, found at the pass's first batch: the camera has
        /// been placed by then (a mod may have set a highlight on what is under it) and nothing
        /// of the world drawn yet.
        void Resolve()
        {
            g_resolve = false;
            std::vector<const void*> roots;
            std::vector<uint32_t>    styleOf;
            std::vector<uint64_t>    orders;
            g_styles.clear();
            for (size_t i = g_entries.size(); i-- > 0;)
            {
                const Entry& entry  = g_entries[i];
                void*        object = objects::Service().Handles().Resolve(entry.target);
                if (!object)
                {
                    g_entries.erase(g_entries.begin() + static_cast<std::ptrdiff_t>(i)); // it left view
                    continue;
                }
                const void* root = binding::m2_object_model(object);
                if (!root) continue; // no model yet, or none of the kind drawn as batches
                const auto seen = std::find(roots.begin(), roots.end(), root);
                if (seen != roots.end())
                {
                    // Two mods highlight it: the later set is drawn.
                    const size_t at = static_cast<size_t>(seen - roots.begin());
                    if (orders[at] < entry.order)
                    {
                        orders[at]            = entry.order;
                        g_styles[styleOf[at]] = {entry.color, entry.outline, entry.glow, entry.strength};
                    }
                    continue;
                }
                roots.push_back(root);
                orders.push_back(entry.order);
                styleOf.push_back(static_cast<uint32_t>(g_styles.size()));
                g_styles.push_back({entry.color, entry.outline, entry.glow, entry.strength});
            }
            if (roots.empty() || renderer::Unsupported()) return;
            renderer::Begin(roots.data(), styleOf.data(), static_cast<uint32_t>(roots.size()));
            g_capturing = true;
        }

        void WorldBegin()
        {
            g_capturing = false;
            g_drawn     = 0;
            g_resolve   = !g_entries.empty();
        }

        void Batch(const void* render)
        {
            if (g_resolve) Resolve();
            if (g_capturing) renderer::Batch(render);
        }

        void WorldEnd()
        {
            g_resolve = false;
            if (!g_capturing) return;
            g_capturing = false;
            g_drawn     = renderer::Draw(g_styles.data(), static_cast<uint32_t>(g_styles.size()));
        }

        void DeviceLost()
        {
            g_capturing = false;
            g_resolve   = false;
            renderer::DeviceLost();
        }

        void Clear()
        {
            g_entries.clear();
            g_capturing = false;
            g_drawn     = 0;
            renderer::Shutdown();
        }
#else
        void WorldBegin() {}
        void Batch(const void*) {}
        void WorldEnd() {}
        void DeviceLost() {}
        void Clear() { g_entries.clear(); }
#endif

        void ForgetMod(uint32_t mod)
        {
            g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(), [&](const Entry& e) { return e.mod == mod; }),
                            g_entries.end());
            if (g_entries.empty()) g_drawn = 0;
        }
    }
}
