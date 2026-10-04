#pragma once

// Central include point for OpenXR with the Vulkan graphics binding.
// Vulkan must be included before openxr_platform.h so the Vulkan-specific
// OpenXR structures are declared. XR_USE_GRAPHICS_API_VULKAN is defined by the
// build system (see CMakeLists.txt).

#include <vulkan/vulkan.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
