// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// WarcraftXL.dll on x64: the entry point the client calls to bring the runtime up.

#ifndef WXL_CORE_INIT_H
#define WXL_CORE_INIT_H

// The recompiled client links WarcraftXL.dll and calls WarcraftXLInit once its process is up.
// The stock client never sees this header: there the DLL is started by the entry trampoline the
// patcher adds (WarcraftXLEntry), with nothing to hand over.
//
// C, not C++, like the SDK contract: the client includes it from whatever language level it is
// built at, and the struct is the only thing both sides have to agree on.

#include <stdint.h>

#include "wxl/abi.h"
#include "wxl/services/gx.h"
#include "wxl/services/m2.h"
#include "wxl/services/m2file.h"

#if defined(WXL_CORE_BUILD)
    #define WXL_CORE_API __declspec(dllexport)
#else
    #define WXL_CORE_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// A function the client's script engine calls directly, as its lua_CFunction: the native side
/// of a global set through the script rows of the binding. Answers how many results it pushed.
typedef int32_t(WXL_CALL* wxl_script_native_fn)(void* state);

/// One string per row of wxl/core/binding.def, "name: ret (params)", in table order. The
/// client hands its copy over in wxl_core_init and the runtime compares it with its own, so a
/// client built from a different table is refused by name instead of having its accessors
/// called through the wrong signatures.
static const char* const wxl_core_init_rows[] = {
#define WXL_BINDING(name, ret, params, args, absent) #name ": " #ret " " #params,
#include "wxl/core/binding.def"
#undef WXL_BINDING
};

#define WXL_CORE_INIT_ROW_COUNT ((uint32_t)(sizeof wxl_core_init_rows / sizeof wxl_core_init_rows[0]))

/// What the client hands the runtime: one accessor per row of wxl/core/binding.def, in table
/// order, map_id first. Zero it, set struct_size to sizeof(wxl_core_init), and rows and
/// row_count to wxl_core_init_rows and WXL_CORE_INIT_ROW_COUNT. The runtime reads only what
/// struct_size covers.
///
/// The function pointers are accessors: the runtime is a DLL and reads the client's state only
/// through what the client hands it here. Each may be left NULL, in which case what is built on
/// it answers the row's documented absent value.
typedef struct wxl_core_init
{
    uint32_t           struct_size;
    uint32_t           row_count; ///< how many rows the client was built with
    const char* const* rows;      ///< their strings, as wxl_core_init_rows spells them

#define WXL_BINDING(name, ret, params, args, absent) ret(WXL_CALL* name) params;
#include "wxl/core/binding.def"
#undef WXL_BINDING
} wxl_core_init;

/**
 * @brief Brings the runtime up: the mod host, every mod under Mods\ next to the client
 *        executable, then WXL_STAGE_EARLY.
 *
 * Call once, on the main thread, before the client reads anything a mod may want to change, and
 * never under the loader lock: loading a mod is a LoadLibrary. The host lives for the rest of the
 * process. The log is Logs\WarcraftXL.log next to the client executable, open since DllMain.
 *
 * @return WXL_OK when the host is up and its mods are loaded; WXL_E_INVALID when init is null,
 *         its struct_size too small, or its rows disagree with the runtime's (the log names the
 *         first that differs); WXL_E_FAILED when the host could not be stood up, in which case
 *         the reason is in the log and the client goes on without mods. A second call returns
 *         the outcome of the first.
 */
WXL_CORE_API wxl_result WXL_CALL WarcraftXLInit(const wxl_core_init* init);

#ifdef __cplusplus
}
#endif

#endif // WXL_CORE_INIT_H
