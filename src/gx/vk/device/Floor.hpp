// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// What a physical device must have for the renderer to run on it, and which one is taken when
// several qualify. Pure: it reads the facts gathered from the driver (Physical.cpp) and decides,
// so the rules are tested without a device, and every refusal names the fact that decided it.

#pragma once

#include "gx/vk/Vk.hpp"

#include <cstdint>

namespace wxl::vk::device
{
    /**
     * The features the renderer reads, in the structures Vulkan reports them in. One row is
     * X(member, its structure's version, required, structure): `core10` is VkPhysicalDeviceFeatures,
     * `v12` and `v13` the Vulkan 1.2 and 1.3 structures. A required feature missing refuses the device
     * by name; the others are enabled where present and decide later paths.
     */
#define WXL_VK_FEATURES(X)                                                                          \
    X(fullDrawIndexUint32, "1.0", true, core10)                                                     \
    X(timelineSemaphore, "1.2", true, v12)                                                          \
    X(bufferDeviceAddress, "1.2", true, v12)                                                        \
    X(runtimeDescriptorArray, "1.2", true, v12)                                                     \
    X(descriptorBindingPartiallyBound, "1.2", true, v12)                                            \
    X(shaderSampledImageArrayNonUniformIndexing, "1.2", true, v12)                                  \
    X(descriptorBindingSampledImageUpdateAfterBind, "1.2", true, v12)                               \
    X(dynamicRendering, "1.3", true, v13)                                                           \
    X(synchronization2, "1.3", true, v13)                                                           \
    X(samplerAnisotropy, "1.0", false, core10)                                                      \
    X(textureCompressionBC, "1.0", false, core10)                                                   \
    X(largePoints, "1.0", false, core10)                                                            \
    X(fillModeNonSolid, "1.0", false, core10)                                                       \
    X(shaderClipDistance, "1.0", false, core10)                                                     \
    X(independentBlend, "1.0", false, core10)                                                       \
    X(depthBiasClamp, "1.0", false, core10)

    /**
     * The device extensions the renderer looks for. One row is X(member, name, enabled): an
     * enabled one is turned on when present; the others are only noted until the step that uses
     * them. The swapchain is required, and enabled, when the device must present (Floor::present).
     */
#define WXL_VK_EXTENSIONS(X)                                                                        \
    X(swapchain, VK_KHR_SWAPCHAIN_EXTENSION_NAME, false)                                            \
    X(memory_budget, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, true)                                     \
    X(graphics_pipeline_library, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, false)            \
    X(pipeline_library, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, false)                              \
    X(extended_dynamic_state3, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, false)               \
    X(vertex_input_dynamic_state, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, false)          \
    X(unified_image_layouts, VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME, false)                    \
    X(calibrated_timestamps, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, false)

    struct Features
    {
#define WXL_VK_FEATURE_MEMBER(member, version, required, where) bool member = false;
        WXL_VK_FEATURES(WXL_VK_FEATURE_MEMBER)
#undef WXL_VK_FEATURE_MEMBER
    };

    struct Extensions
    {
#define WXL_VK_EXTENSION_MEMBER(member, name, enabled) bool member = false;
        WXL_VK_EXTENSIONS(WXL_VK_EXTENSION_MEMBER)
#undef WXL_VK_EXTENSION_MEMBER
    };

    /// What selection reads of one physical device.
    struct Candidate
    {
        uint32_t             index = 0; ///< in the loader's enumeration
        char                 name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
        VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
        uint32_t             api = 0;       ///< the device's apiVersion
        uint32_t             driver = 0;    ///< driverVersion, in the vendor's packing
        uint32_t             vendor = 0;
        uint32_t             device_id = 0;
        uint64_t             device_local = 0;          ///< bytes of DEVICE_LOCAL heaps
        uint32_t             family = UINT32_MAX;       ///< the graphics family taken
        bool                 presents = false;          ///< that family can present to a window
        Features             features;
        Extensions           extensions;
        char                 reject[192] = {};          ///< why it does not qualify; empty when it does
    };

    /// What a device must meet.
    struct Floor
    {
        uint32_t api = kFloorApi;
        bool     present = true; ///< a graphics family that presents, and the swapchain; off for headless tests
    };

    /// Fills @p candidate.reject when it misses the floor: the first fact that fails, named.
    void Judge(Candidate& candidate, const Floor& floor);

    /// Why @p a is preferred over @p b, or null when it is not.
    const char* WhyBetter(const Candidate& a, const Candidate& b);

    /// Whether @p preference ("[gx] gpu": an index, or a part of the name in any case) names @p candidate.
    bool Names(const Candidate& candidate, const char* preference);

    struct Choice
    {
        int  index = -1;      ///< into the list, or -1 when none qualifies
        char why[192] = {};   ///< the rule that decided
        char note[256] = {};  ///< a preference that could not be honoured, and why; empty otherwise
    };

    /**
     * Takes the best qualifying device: the one @p preference names when it qualifies, else by
     * WhyBetter. The candidates must have been judged.
     */
    Choice Choose(const Candidate* candidates, uint32_t count, const char* preference);

    /// "<name>: <reason>; <name>: <reason>" for the refusal line, cut to @p size.
    void Refusals(const Candidate* candidates, uint32_t count, char* out, uint32_t size);

    /// The driver's version as its vendor writes it: NVIDIA's 10.8.8 bits, Intel's 18.14 on Windows,
    /// Vulkan's packing for the others.
    void FormatDriver(uint32_t vendor, uint32_t driver, char* out, uint32_t size);

    /// "discrete GPU", "integrated GPU", "CPU"...
    const char* TypeName(VkPhysicalDeviceType type);
}
