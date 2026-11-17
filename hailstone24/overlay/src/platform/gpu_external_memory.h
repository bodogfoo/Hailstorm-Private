#pragma once

// The OS-specific part of sharing a GPU texture's memory between processes (render/gpu_shared_cache.h): which
// Vulkan handle type and device extension carry it here, and the import and export calls whose structs name OS
// handle types. Supports Win32 (opaque NT handles) and Linux (VK_KHR_external_memory_fd opaque file descriptors).

#include <cstdint>

#include <volk.h>

namespace hailstone::platform {

// The device extension the handle type needs, or nullptr where external textures are unsupported.
const char* gpu_external_memory_extension() noexcept;
// The handle type, or 0 where unsupported.
VkExternalMemoryHandleTypeFlagBits gpu_external_memory_handle_type() noexcept;

// Allocates `size` bytes of memory type `memory_type`, dedicated to `image`, importing `handle`'s payload. The
// handle stays the caller's (on Linux the fd is dup'd with F_DUPFD_CLOEXEC before Vulkan takes ownership on success);
// the new memory keeps its own reference to the payload, so it stays valid when the exporting process lets go or exits.
VkResult gpu_import_memory(VkDevice device, PFN_vkAllocateMemory allocate, VkImage image, VkDeviceSize size,
                           std::uint32_t memory_type, void* handle, VkDeviceMemory* out) noexcept;

// Exports `memory` (allocated with this handle type) as a new handle the caller owns. Only the broker tool links
// this (gpu_external_memory_export.cpp).
VkResult gpu_export_memory(VkDevice device, VkDeviceMemory memory, void** handle) noexcept;

} // namespace hailstone::platform
