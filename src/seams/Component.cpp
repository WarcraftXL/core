// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The Component seam: what a character appearance puts on and takes off, handed to wxl.m2's
// watchers.

#include "wxl/seams/seams.h"

#include "binding/Binding.hpp"
#include "services/M2Service.hpp"

#include <vector>

namespace
{
    void Hand(const void* component, int32_t item_slot, uint32_t change)
    {
        // Nothing at all is asked of the client while no mod watches.
        if (!component || !wxl::core::services::m2::Watching()) return;

        wxl_m2_equipment equipment{};
        const uint32_t   slot = item_slot < 0 ? UINT32_MAX : static_cast<uint32_t>(item_slot);
        if (!wxl::core::binding::component_wearer(component, slot, &equipment)) return;
        equipment.struct_size = sizeof equipment;
        equipment.change      = change;
        if (change == WXL_M2_FREED)
        {
            equipment.item_slot  = 0;
            equipment.display_id = 0;
        }
        else if (equipment.display_id == 0)
        {
            return; // an empty slot: nothing was put on, nothing comes off
        }
        wxl::core::services::m2::DispatchEquipment(equipment);
    }
}

namespace wxl::seam::Component::AddItem
{
    void WXL_CALL After(void* self, int32_t item_slot, const void*, int32_t)
    {
        Hand(self, item_slot, WXL_M2_EQUIPPED);
    }
}

namespace wxl::seam::Component::RemoveItem
{
    void WXL_CALL Before(void* self, int32_t item_slot)
    {
        Hand(self, item_slot, WXL_M2_UNEQUIPPED);
    }
}

namespace wxl::seam::Component::FreeComponent
{
    void WXL_CALL Before(void* component)
    {
        Hand(component, 0, WXL_M2_FREED);
    }
}

namespace
{
    // What RemoveVisuals is about to strip that wxl.m2 put there: a model a mod made (it keeps its
    // own reference, so it survives the detach), or an interface copy of one (it does not: the
    // parent held its only reference, so it is copied again from the model it was made from).
    struct Kept
    {
        void*    model;
        bool     copy;
        uint32_t attachment;
        wxl_bool to_root;
    };

    std::vector<Kept> g_kept;

    /// The points CCharacterComponent::RemoveVisuals detaches: only what hangs there is stripped.
    bool Stripped(uint32_t attachment)
    {
        switch (attachment)
        {
            case 12: case 15: case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23:
            case 24: case 25: case 29: case 34: case 35: case 38:
                return true;
            default:
                return false;
        }
    }
    void*             g_keptFrom = nullptr;
}

namespace wxl::seam::Component::RemoveVisuals
{
    void WXL_CALL Before(void* model)
    {
        namespace binding = wxl::core::binding;
        namespace m2      = wxl::core::services::m2;
        g_kept.clear();
        g_keptFrom = nullptr;
        if (!model || !m2::HasModels()) return;

        void*          children[64];
        const uint32_t count = binding::m2_children(model, children, 64);
        for (uint32_t i = 0; i < count && i < 64; ++i)
        {
            void* source = m2::SourceOf(children[i]);
            if (!source) continue;
            Kept kept{source, source != children[i], 0, WXL_FALSE};
            if (binding::m2_attach_info(children[i], &kept.attachment, &kept.to_root) && Stripped(kept.attachment))
                g_kept.push_back(kept);
        }
        if (!g_kept.empty()) g_keptFrom = model;
    }

    void WXL_CALL After(void* model)
    {
        namespace binding = wxl::core::binding;
        if (!model || model != g_keptFrom) return;
        for (const Kept& kept : g_kept)
        {
            if (kept.copy)
                binding::m2_attach_copy(kept.model, model, kept.attachment, kept.to_root);
            else if (!binding::m2_model_parent(kept.model)) // stripped: hang it back
                binding::m2_attach_model(kept.model, model, kept.attachment, kept.to_root);
        }
        g_kept.clear();
        g_keptFrom = nullptr;
    }
}
