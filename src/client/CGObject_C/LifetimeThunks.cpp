// SPDX-License-Identifier: GPL-3.0-or-later

#include "client/CGObject_C/LifetimeThunks.hpp"

#include "wxl/abi/Common.h"

// Every register and the flags are kept around the handler: the client's callers of these static
// functions may rely on more than the usual convention keeps.
namespace wxl::client::lifetime
{
    Thunks g_thunks;

    WXL_NAKED void InitObjectAfter()
    {
        __asm
        {
            push esi
            call dword ptr [g_thunks.initObject]
            pushfd
            pushad
            push dword ptr [esp + 36] // the kept object, above pushfd and pushad
            call dword ptr [g_thunks.onAdded]
            add esp, 4
            popad
            popfd
            add esp, 4
            ret
        }
    }

    WXL_NAKED void ObjDeleteBefore()
    {
        __asm
        {
            pushfd
            pushad
            push eax
            call dword ptr [g_thunks.onRemoved]
            add esp, 4
            popad
            popfd
            jmp dword ptr [g_thunks.objDelete]
        }
    }

    WXL_NAKED void DestroyRemovedObjectBefore()
    {
        __asm
        {
            pushfd
            pushad
            push esi
            call dword ptr [g_thunks.onRemoved]
            add esp, 4
            popad
            popfd
            jmp dword ptr [g_thunks.destroyRemovedObject]
        }
    }
}
