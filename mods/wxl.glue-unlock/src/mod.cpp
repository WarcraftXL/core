// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.glue-unlock: the client loads a changed interface -- its own GlueXML and FrameXML, the
// Blizzard add-ons -- as it loads the one it shipped with, and leaves loose interface folders on
// disk where they are.

#include <string_view>

#include "wxl/sdk.hpp"

namespace
{
    constexpr const char* kTag = "glue-unlock";

    /// An add-on's .toc, as the client names it: "Interface\AddOns\<name>\<name>.toc".
    bool IsAddOn(std::string_view file)
    {
        constexpr std::string_view kAddOns = "interface\\addons\\";
        if (file.size() < kAddOns.size()) return false;
        for (std::size_t i = 0; i < kAddOns.size(); ++i)
        {
            char c = file[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c == '/') c = '\\';
            if (c != kAddOns[i]) return false;
        }
        return true;
    }

    /// Answers the client's signature checks and its clearing of loose folders.
    class Unlock final : public wxl::LuaScript
    {
    public:
        // The login screens' GlueXML and the game's FrameXML are taken as signed whatever the
        // verdict: anything short of it leaves the login screens empty or closes the client. An
        // add-on is taken as signed only when it ships a .sig (corrupt or modified: one of the
        // client's own, which would load tainted or not at all); one without a .sig loads as
        // every user add-on does, as insecure code.
        void OnInterfaceSignature(std::string_view file, std::string_view, wxl_interface_signature verdict,
                                  wxl::Answer accepted) override
        {
            if (verdict == WXL_INTERFACE_SIGNATURE_SIGNED) return;
            if (IsAddOn(file) && verdict == WXL_INTERFACE_SIGNATURE_MISSING) return;
            if (!accepted.Set()) return;
            ++accepted_;
            wxl::Log(WXL_LOG_DEBUG, kTag, "{}: {}, taken as signed", file, Name(verdict));
        }

        // Loose Interface\GlueXML, Interface\FrameXML and Interface\AddOns\Blizzard* folders stay
        // and load, instead of being renamed to .old.
        void OnInterfaceCleanupRequest(wxl::Answer refused) override
        {
            if (!refused.Set()) return;
            ++kept_;
            wxl::Log(WXL_LOG_DEBUG, kTag, "loose interface folders kept on disk");
        }

        unsigned Accepted() const { return accepted_; }
        unsigned Kept() const { return kept_; }

    private:
        static const char* Name(wxl_interface_signature verdict)
        {
            switch (verdict)
            {
            case WXL_INTERFACE_SIGNATURE_MISSING:  return "no signature";
            case WXL_INTERFACE_SIGNATURE_CORRUPT:  return "corrupt signature";
            case WXL_INTERFACE_SIGNATURE_MODIFIED: return "modified";
            default:                               return "unknown verdict";
            }
        }

        unsigned accepted_ = 0;
        unsigned kept_     = 0;
    };

    class GlueUnlockMod final : public wxl::Mod
    {
    public:
        wxl_result OnLoad() override
        {
            if (!wxl::Raises(WXL_EV_InterfaceSignature))
            {
                wxl::Log(WXL_LOG_ERROR, kTag, "this runtime raises no InterfaceSignature: the client checks signatures as it ships");
                return WXL_E_UNSUPPORTED;
            }
            wxl::ScriptMgr::Add(&unlock_);
            wxl::Log(WXL_LOG_INFO, kTag, "interface files load whatever their signature");
            return WXL_OK;
        }

        void OnShutdown() override
        {
            wxl::Log(WXL_LOG_INFO, kTag, "this session: {} interface file(s) taken as signed, loose folders kept {} time(s)",
                     unlock_.Accepted(), unlock_.Kept());
        }

    private:
        Unlock unlock_;
    };
} // namespace

WXL_MOD(GlueUnlockMod, "wxl.glue-unlock", "Glue unlock")
