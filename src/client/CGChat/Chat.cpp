// SPDX-License-Identifier: GPL-3.0-or-later
// The chat seam: OnChatMessage for each server line the client keeps, past its filters, before it is shown.

#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "runtime/HookPoints.hpp"
#include "runtime/objects/Objects.hpp"
#include "wxl/offsets/CGChat.hpp"

namespace
{
    namespace ev  = wxl::events;
    namespace off = wxl::offsets;

    off::CGChat::RecordChatFn g_origRecordChat = nullptr;

    const char* OrEmpty(const char* text) { return text ? text : ""; }

    /// AddChatMessage's only call of RecordChat comes after its spam and encoding filters.
    void __cdecl RecordChatDetour(const char* text, int32_t type, const char* senderName, uint64_t senderGuid,
                                  uint32_t senderUnk, uint32_t zoneChannelId, const char* channelName, uint32_t lineId,
                                  const void* player)
    {
        if (text && ev::Any(ev::Event::OnChatMessage))
        {
            const WXL_Handle sender = wxl::runtime::objects::HandleOf(senderGuid);
            const ev::ChatMessageArgs args{ type, text, OrEmpty(senderName), senderGuid, sender,
                                            OrEmpty(channelName), zoneChannelId, lineId };
            ev::EmitFor(ev::Event::OnChatMessage, sender, &args);
        }
        g_origRecordChat(text, type, senderName, senderGuid, senderUnk, zoneChannelId, channelName, lineId, player);
    }

    bool InstallChat() { return wxl::runtime::hookpoints::Attach("CGChat::RecordChat", &RecordChatDetour, &g_origRecordChat); }
}

WXL_REGISTER_FEATURE("chat", true, InstallChat)
