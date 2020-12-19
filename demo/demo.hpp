#pragma once

#include <functional>
#include <liblava-extras/raytracing.hpp>
#include <liblava/lava.hpp>
#include <optional>

lava::device::ptr create_raytracing_device(lava::device_manager& manager);
std::optional<VkFormat> get_supported_format(lava::device_ptr device, const lava::VkFormats& possible_formats, VkImageUsageFlags usage);
VkDeviceAddress get_buffer_address(lava::device_ptr device, lava::buffer::ptr buffer);
bool one_time_command_buffer(lava::device_ptr device, VkCommandPool pool, lava::device::queue::ref queue, std::function<void(VkCommandBuffer)> callback);

// VmaAllocator with custom create flags
// we need VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT to take buffer device addresses

struct custom_flags_allocator : lava::allocator {
    explicit custom_flags_allocator(VkPhysicalDevice physical_device, VkDevice device, VmaAllocatorCreateFlags flags);

    using ptr = std::shared_ptr<custom_flags_allocator>;
};

inline custom_flags_allocator::ptr make_custom_flags_allocator(VkPhysicalDevice physical_device, VkDevice device, VmaAllocatorCreateFlags flags) {
    return std::make_shared<custom_flags_allocator>(physical_device, device, flags);
}
