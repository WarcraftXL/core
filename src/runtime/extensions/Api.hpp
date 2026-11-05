// SPDX-License-Identifier: GPL-3.0-or-later
// The WXL_Api table every extension's WXL_Load receives.

#pragma once

#include "wxl/abi/Plugin.h"

namespace wxl::runtime::extensions
{
    /// The core's table, valid for the process lifetime. Its functions check every argument: they
    /// come from binaries this build never saw.
    const WXL_Api& Api();
}
