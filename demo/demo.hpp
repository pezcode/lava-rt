#pragma once

#include <functional>
#include <liblava-extras/raytracing.hpp>
#include <liblava/lava.hpp>
#include <optional>

lava::allocator::ptr create_custom_allocator(lava::device_ptr device, VmaAllocatorCreateFlags flags);
lava::device::ptr create_raytracing_device(lava::device_manager& manager);
std::optional<VkFormat> get_supported_format(lava::device_ptr device, const lava::VkFormats& possible_formats, VkImageUsageFlags usage);
VkDeviceAddress get_buffer_address(lava::device_ptr device, lava::buffer::ptr buffer);
bool one_time_command_buffer(lava::device_ptr device, VkCommandPool pool, lava::device::queue::ref queue, std::function<void(VkCommandBuffer)> callback);
glm::mat4 perspective_matrix(lava::uv2 size, float fov = 90.0f, float far_plane = 5.0f);
