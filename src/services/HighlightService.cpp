// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.highlight: the highlights mods set, one per mod and object, kept here; each world pass finds
// their objects' top models and hands them to the renderer, which keeps the batches drawn of
// them and draws the highlights after the world. Under Vulkan the renderer is the Vulkan one's
// (gx/vk/render/Highlight.cpp, made of wxl.render's pieces), on both clients; the stock client on
// Direct3D 9 has the same passes made on its D3D9 device (gx/d3d9/Highlight.cpp); the recompiled
// client on D3D9 has none, and says so through the contract.
//
// While a highlight is set the runtime holds up the client's flag of ModelBatchDrawn: the x64
// client calls M2.DrawBatch, where the batches are kept, only while that flag is up. What each
// world pass did is counted, and logged under `highlight` about once a second at debug level, with
// each reason something was left out logged once.

#include "services/HighlightService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "objects/Objects.hpp"
#include "runtime/Core.hpp"
#include "runtime/Listening.hpp"

#include "wxl/host/mod_host.hpp"

#include "gx/Vulkan.hpp"
#include "gx/vk/gxfront/Front.hpp"
#include "gx/vk/render/Highlight.hpp"
#if defined(_M_IX86)
    #include "gx/d3d9/Highlight.hpp"
#endif

#include <algorithm>
#include <cstddef>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using namespace wxl::core;

    constexpr const char* kHighlightTag = "highlight";

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

    services::highlight::PassCounts g_pass;          ///< the open or the last world pass with highlights
    uint32_t                        g_passes = 0;    ///< world passes with highlights since the last summary
    std::chrono::steady_clock::time_point g_summaryAt{}; ///< when the summary's second began
    bool                            g_summarized = false; ///< a second is being summarized: highlights are set
    std::vector<std::string>        g_said;          ///< the reasons logged already, each once

    wxl::vk::render::Highlight g_vulkan; ///< the Vulkan renderer's passes, attached to its device's wxl.render
    wxl::vk::render::Render*   g_attached = nullptr;
    const services::highlight::VulkanTarget* g_target = nullptr; ///< the tests' in place of the device's

    /// Whether the highlights are drawn on the stock client's Direct3D 9 device rather than on the
    /// Vulkan renderer: the stock client not drawn by Vulkan.
#if defined(_M_IX86)
    bool D3d9() { return !g_target && !gx::VulkanActive(); }
#else
    constexpr bool D3d9() { return false; }
#endif

    /// The client calls M2.DrawBatch while a highlight is set, whether or not a mod listens to
    /// ModelBatchDrawn.
    void Demand() { listening::RuntimeWants(WXL_EV_ModelBatchDrawn, !g_entries.empty()); }

    /// A reason something was left out: the world pass's first (when @p pass), and logged the
    /// first time it is given.
    void Refused(const std::string& reason, bool pass = true)
    {
        if (reason.empty()) return;
        if (pass && g_pass.refusal.empty()) g_pass.refusal = reason;
        if (std::find(g_said.begin(), g_said.end(), reason) != g_said.end() || g_said.size() >= 32) return;
        g_said.push_back(reason);
        WLOG_INFO(kHighlightTag, "wxl.highlight: nothing drawn for a highlight: %s", reason.c_str());
    }

    /// What the Vulkan passes draw with and whose draws they keep: the device's, or the tests'.
    bool Target(wxl::vk::render::Render*& render, wxl::vk::draw::Renderer*& engine)
    {
        if (g_target)
        {
            render = g_target->render;
            engine = g_target->engine;
            return render != nullptr;
        }
        wxl::vk::gxfront::Front* front = gx::VulkanFront();
        if (!front) return false;
        render = &front->Mods();
        engine = &front->Renderer();
        return true;
    }

    /// The renderer keeps each draw while a world pass has highlighted models in it.
    void KeepDraws()
    {
        if (!g_target) gx::UpdateBatchKeeping();
        else if (g_target->engine) g_target->engine->KeepBatches(services::highlight::Capturing());
    }

    const char* Refusal()
    {
#if defined(_M_IX86)
        if (D3d9()) return gx::d3d9::highlight::Unsupported();
#endif
        if (g_target) return g_target->render ? nullptr : "no Vulkan renderer";
        if (gx::VulkanFront()) return nullptr;
        if (gx::VulkanActive()) return "the Vulkan device is being made again";
        return "the recompiled client draws highlights on its Vulkan renderer, and it runs on Direct3D 9 (set [gx] api = vulkan in "
               "WarcraftXL.ini)";
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
        if (const char* why = Refusal())
        {
            Refused(why, false);
            return WXL_E_UNSUPPORTED;
        }
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
        Demand();
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
        if (g_entries.empty()) g_drawn = 0;
        Demand();
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
        /// The styles of this pass, one per top model in view, in the order handed to the renderer.
        using Style = wxl::vk::render::Highlight::Style;
#if defined(_M_IX86)
        namespace renderer = gx::d3d9::highlight;
        static_assert(sizeof(renderer::Style) == sizeof(Style) && offsetof(renderer::Style, strength) == offsetof(Style, strength),
                      "one style for both renderers");
        const renderer::Style* D3d9Styles(const std::vector<Style>& styles) { return reinterpret_cast<const renderer::Style*>(styles.data()); }
#endif
        std::vector<Style>       g_styles;
        std::vector<const void*> g_roots; ///< the highlighted top models of this pass, counted against each batch

        /// The highlighted objects' top models, found at the pass's first batch: the camera has
        /// been placed by then (a mod may have set a highlight on what is under it) and nothing
        /// of the world drawn yet.
        void Resolve()
        {
            g_resolve       = false;
            g_pass.resolved = true;
            std::vector<const void*> roots;
            std::vector<uint32_t>    styleOf;
            std::vector<uint64_t>    orders;
            g_styles.clear();
            bool modelless = false;
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
                if (!root)
                {
                    modelless = true; // no model yet, or none of the kind drawn as batches
                    continue;
                }
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
            Demand();
            g_pass.roots = static_cast<uint32_t>(roots.size());
            if (modelless) Refused("a highlighted object has no model drawn in batches (not loaded yet, or not a model)");
            g_roots = roots;
            if (roots.empty()) return;
            if (const char* why = Refusal())
            {
                Refused(why);
                return;
            }
#if defined(_M_IX86)
            if (D3d9())
            {
                renderer::Begin(roots.data(), styleOf.data(), static_cast<uint32_t>(roots.size()));
                g_capturing = true;
                return;
            }
#endif
            wxl::vk::render::Render*  render = nullptr;
            wxl::vk::draw::Renderer*  engine = nullptr;
            if (!Target(render, engine)) return;
            if (g_attached != render)
            {
                g_vulkan.Forget();
                g_vulkan.Attach(*render);
                g_attached = render;
            }
            std::vector<wxl::vk::render::Highlight::Root> pass(roots.size());
            for (size_t i = 0; i < roots.size(); ++i) pass[i] = {roots[i], styleOf[i]};
            g_vulkan.World(pass.data(), static_cast<uint32_t>(pass.size()));
            g_capturing = true;
            KeepDraws(); // the renderer keeps each draw from here
        }

        void WorldBegin()
        {
            g_capturing = false;
            g_resolve   = !g_entries.empty();
            if (!g_resolve) return;
            g_pass         = PassCounts{};
            g_pass.entries = static_cast<uint32_t>(g_entries.size());
            g_pass.open    = true;
            ++g_passes;
            // The renderer keeps the world's draws from its first one, a highlighted model's first batch included.
            if (!D3d9()) KeepDraws();
        }

        bool WantsBatch() { return g_resolve || g_capturing; }

        bool Capturing() { return g_capturing || g_resolve; }

        void Batch(const void* render, const wxl_model_batch_args& args)
        {
            if (g_resolve) Resolve();
            if (!g_capturing) return;
            ++g_pass.models;
            const bool highlighted = std::find(g_roots.begin(), g_roots.end(), args.root_model) != g_roots.end();
            if (highlighted) ++g_pass.seen;
#if defined(_M_IX86)
            if (D3d9())
            {
                if (renderer::Batch(render, args)) ++g_pass.kept;
                return;
            }
#endif
            (void)render;
            if (!highlighted) return;
            wxl::vk::render::Render* target = nullptr;
            wxl::vk::draw::Renderer* engine = nullptr;
            const wxl::vk::draw::Batch* last = Target(target, engine) && engine ? engine->LastBatch() : nullptr;
            if (g_vulkan.Batch(args.root_model, args.blend_mode, last)) ++g_pass.kept;
        }

        void WorldEnd()
        {
            const bool open = g_pass.open;
            g_pass.open     = false;
            g_resolve       = false;
            if (!open) return;
            if (!g_pass.resolved)
                Refused("no model batch reached wxl.highlight in a world pass (the M2.DrawBatch seam was not called)");
            if (!g_capturing)
            {
                if (!D3d9()) KeepDraws();
                g_drawn = 0;
                return;
            }
            g_capturing = false;
#if defined(_M_IX86)
            if (D3d9())
            {
                g_drawn           = renderer::Draw(D3d9Styles(g_styles), static_cast<uint32_t>(g_styles.size()));
                g_pass.drawn      = g_drawn;
                g_pass.composited = g_drawn > 0;
                if (g_pass.kept && !g_drawn) Refused("the Direct3D 9 device drew none of the kept batches again");
                return;
            }
#endif
            KeepDraws();
            if (const char* why = Refusal())
            {
                g_vulkan.Begin();
                g_drawn = 0;
                Refused(why);
                return;
            }
            g_drawn = g_vulkan.Draw(g_styles.data(), static_cast<uint32_t>(g_styles.size()));
            const wxl::vk::render::Highlight::Counts& counts = g_vulkan.Pass();
            g_pass.drawn      = counts.drawn;
            g_pass.composited = counts.composited;
            if (!counts.refusal.empty()) Refused(counts.refusal);
        }

        void Present()
        {
            if (g_entries.empty())
            {
                g_passes     = 0;
                g_summarized = false;
                return;
            }
            // The first second starts with the first frame a highlight is set in.
            const auto now = std::chrono::steady_clock::now();
            if (!g_summarized)
            {
                g_summarized = true;
                g_summaryAt  = now;
                return;
            }
            if (now - g_summaryAt < std::chrono::seconds(1)) return;
            const double   seconds = std::chrono::duration<double>(now - g_summaryAt).count();
            const uint32_t passes  = g_passes;
            g_summaryAt            = now;
            g_passes               = 0;
            if (!passes)
            {
                WLOG_DEBUG(kHighlightTag, "%zu highlight(s) set, no world pass drawn in %.1f s", g_entries.size(), seconds);
                return;
            }
            WLOG_DEBUG(kHighlightTag,
                       "%zu highlight(s) set, %u world pass(es) in %.1f s; the last: %u top model(s) found, %u model batch(es) in the "
                       "world, %u of the highlighted models, %u kept, %u drawn again into the masks, %s%s%s",
                       g_entries.size(), passes, seconds, g_pass.roots, g_pass.models, g_pass.seen, g_pass.kept, g_pass.drawn,
                       g_pass.composited ? "composited onto the scene" : "nothing composited onto the scene",
                       g_pass.refusal.empty() ? "" : "; first refusal: ", g_pass.refusal.c_str());
        }

        const PassCounts& LastPass() { return g_pass; }

        void DeviceLost()
        {
            g_capturing = false;
            g_resolve   = false;
            g_pass.open = false;
#if defined(_M_IX86)
            if (D3d9())
            {
                renderer::DeviceLost();
                return;
            }
#endif
            // The Vulkan passes' images and pipelines are wxl.render's: made again with the device.
            g_vulkan.Begin();
            KeepDraws();
        }

        void DeviceGone()
        {
            g_vulkan.Forget();
            g_attached  = nullptr;
            g_capturing = false;
            g_pass.open = false;
        }

        void Clear()
        {
            g_entries.clear();
            Demand();
            g_capturing = false;
            g_resolve   = false;
            g_drawn     = 0;
            g_pass      = PassCounts{};
            g_passes    = 0;
            g_said.clear();
#if defined(_M_IX86)
            renderer::Shutdown();
#endif
            g_vulkan.Detach();
            g_attached = nullptr;
        }

        void ForgetMod(uint32_t mod)
        {
            g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(), [&](const Entry& e) { return e.mod == mod; }),
                            g_entries.end());
            if (g_entries.empty()) g_drawn = 0;
            Demand();
        }

        void UseVulkanTarget(const VulkanTarget* target)
        {
            g_target = target;
            g_vulkan.Forget();
            g_attached = nullptr;
        }
    }
}
