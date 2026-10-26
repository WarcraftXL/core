// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The one translation unit that compiles VMA's implementation, over volk's entry points: no
// function is linked statically, none fetched by VMA itself.

#include "gx/vk/Vk.hpp"

#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
