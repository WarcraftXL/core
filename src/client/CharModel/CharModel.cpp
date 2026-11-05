// SPDX-License-Identifier: GPL-3.0-or-later
// The character-model seams: OnItemSlotChange after an item is put on, OnItemSlotClear before one comes off.

#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "runtime/HookPoints.hpp"
#include "wxl/offsets/CCharacterComponent.hpp"

#include <cstdint>

namespace
{
    namespace ev  = wxl::events;
    namespace off = wxl::offsets;

    off::CCharacterComponent::AddItemFn          g_origAddItem    = nullptr;
    off::CCharacterComponent::RemoveItemBySlotFn g_origRemoveItem = nullptr;

    /// Raised after the client's handler: it refuses an occupied head slot, so the slot must be filled
    /// first. `modelSlot` is the model's own slot index; `item` carries the display id.
    void __fastcall AddItemDetour(void* model, void* edx, int32_t modelSlot, const void* item, int32_t postFlag)
    {
        g_origAddItem(model, edx, modelSlot, item, postFlag);
        const ev::ItemSlotChangeArgs args{ model, static_cast<uint32_t>(modelSlot), const_cast<void*>(item) };
        ev::Emit<ev::Event::OnItemSlotChange>(args);
    }

    /// `equipSlot` is the equipment slot (0-18).
    void __fastcall RemoveItemDetour(void* model, void* edx, uint32_t equipSlot)
    {
        const ev::ItemSlotClearArgs args{ model, equipSlot };
        ev::Emit<ev::Event::OnItemSlotClear>(args);
        g_origRemoveItem(model, edx, equipSlot);
    }

    bool InstallCharModel()
    {
        wxl::runtime::hookpoints::Attach("CCharacterComponent::AddItem", &AddItemDetour, &g_origAddItem);
        wxl::runtime::hookpoints::Attach("CCharacterComponent::RemoveItemBySlot", &RemoveItemDetour, &g_origRemoveItem);
        return true;
    }
}

WXL_REGISTER_FEATURE("charmodel", true, InstallCharModel)
