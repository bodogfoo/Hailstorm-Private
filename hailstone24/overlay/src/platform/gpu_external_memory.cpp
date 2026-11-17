#include "platform/gpu_external_memory.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace hailstone::platform {

const char* gpu_external_memory_extension() noexcept {
#if defined(_WIN32)
    return VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
#elif defined(__linux__)
    return VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
#else
    return nullptr;
#endif
}

VkExternalMemoryHandleTypeFlagBits gpu_external_memory_handle_type() noexcept {
#if defined(_WIN32)
    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#elif defined(__linux__)
    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#else
    return static_cast<VkExternalMemoryHandleTypeFlagBits>(0);
#endif
}

VkResult gpu_import_memory(VkDevice device, PFN_vkAllocateMemory allocate, VkImage image, VkDeviceSize size,
                           std::uint32_t memory_type, void* handle, VkDeviceMemory* out) noexcept {
#if defined(_WIN32)
    if (!allocate || !handle || !out) return VK_ERROR_INITIALIZATION_FAILED;
    VkImportMemoryWin32HandleInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    imported.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    imported.handle = static_cast<HANDLE>(handle);
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.pNext = &imported;
    dedicated.image = image;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = &dedicated;
    // An opaque handle's import uses the exporter's size and memory type as they are.
    allocation.allocationSize = size;
    allocation.memoryTypeIndex = memory_type;
    return allocate(device, &allocation, nullptr, out);
#elif defined(__linux__)
    if (!allocate || !handle || !out) return VK_ERROR_INITIALIZATION_FAILED;
    const std::uintptr_t encoded = reinterpret_cast<std::uintptr_t>(handle);
    if (encoded == 0) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    const int caller_fd = static_cast<int>(encoded - 1);
    if (caller_fd < 0) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    // Vulkan takes ownership of the imported FD upon VK_SUCCESS, so duplicate first to preserve caller's handle.
    const int dup_fd = ::fcntl(caller_fd, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0) return VK_ERROR_INVALID_EXTERNAL_HANDLE;

    VkImportMemoryFdInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    imported.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    imported.fd = dup_fd;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.pNext = &imported;
    dedicated.image = image;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = &dedicated;
    allocation.allocationSize = size;
    allocation.memoryTypeIndex = memory_type;
    const VkResult result = allocate(device, &allocation, nullptr, out);
    if (result != VK_SUCCESS) ::close(dup_fd);
    return result;
#else
    (void)device; (void)allocate; (void)image; (void)size; (void)memory_type; (void)handle; (void)out;
    return VK_ERROR_EXTENSION_NOT_PRESENT;
#endif
}

} // namespace hailstone::platform
