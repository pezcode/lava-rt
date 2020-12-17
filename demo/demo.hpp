#pragma once

#include <liblava/lava.hpp>
#include <liblava-extras/raytracing.hpp>
#include <optional>
#include <functional>

lava::device::ptr create_raytracing_device(lava::device_manager& manager);
std::optional<VkFormat> get_supported_format(lava::device_ptr device, const lava::VkFormats& possible_formats, VkImageUsageFlags usage);
bool one_time_command_buffer(lava::device_ptr device, VkCommandPool pool, lava::device::queue::ref queue, std::function<void(VkCommandBuffer)> callback);
