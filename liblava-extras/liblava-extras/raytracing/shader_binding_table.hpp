#pragma once

#include <liblava-extras/raytracing/pipeline.hpp>
#include <liblava/base/device.hpp>
#include <liblava/resource/buffer.hpp>
#include <memory>
#include <vector>

// assumes shader groups were added to a pipeline in a specific order:
// x raygen group
// y miss shaders
// y hit shaders

// no shader parameters supported yet
// this affects the binding stride

namespace lava {
    namespace extras {
        namespace raytracing {

            struct shader_binding_table {
                using ptr = std::shared_ptr<shader_binding_table>;

                ~shader_binding_table() {
                    destroy();
                };

                bool create(raytracing_pipeline::ptr pipeline, size_t raygen_shaders = 1) {
                    const VkRayTracingShaderGroupCreateInfosKHR groups = pipeline->get_shader_groups();
                    if (groups.size() < raygen_shaders)
                        return false;
                    if ((groups.size() - raygen_shaders) % 2 != 0)
                        return false;

                    size_t raygen_count = raygen_shaders;
                    size_t miss_count = (groups.size() - raygen_count) / 2;
                    size_t hit_count = miss_count;

                    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& rt_properties = pipeline->get_properties();

                    size_t handle_size = rt_properties.shaderGroupHandleSize;
                    lava::device_ptr device = pipeline->get_device();

                    std::vector<uint8_t> handles(handle_size * groups.size());
                    if (!lava::check(device->call().vkGetRayTracingShaderGroupHandlesKHR(
                            device->get(), pipeline->get(), 0, groups.size(), handles.size(), handles.data())))
                        return false;

                    // TODO support for parameters

                    entry_size = lava::align_up(handle_size, size_t(rt_properties.shaderGroupHandleAlignment));
                    std::vector<uint8_t> table_data(entry_size * groups.size());
                    for (size_t i = 0; i < groups.size(); i++) {
                        memcpy(table_data.data() + (i * entry_size), handles.data() + (i * handle_size), handle_size);
                    }

                    buffer = lava::make_buffer();
                    if (!buffer->create_mapped(device, nullptr, table_data.size() + rt_properties.shaderGroupBaseAlignment, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT))
                        return false;

                    const VkBufferDeviceAddressInfoKHR addr_info = {
                        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR,
                        .buffer = buffer->get()
                    };
                    VkDeviceAddress address = device->call().vkGetBufferDeviceAddressKHR(device->get(), &addr_info);

                    uint8_t* buffer_data = static_cast<uint8_t*>(buffer->get_mapped_data());
                    size_t buffer_offset = lava::align_up(address, VkDeviceAddress(rt_properties.shaderGroupBaseAlignment)) - address;
                    size_t data_offset = 0;

                    raygen_region = {
                        .deviceAddress = address + buffer_offset,
                        .stride = entry_size,
                        .size = raygen_count * entry_size
                    };

                    memcpy(buffer_data + buffer_offset, table_data.data() + data_offset, raygen_region.size);
                    buffer_offset += lava::align_up(raygen_region.size, VkDeviceAddress(rt_properties.shaderGroupBaseAlignment));
                    data_offset += raygen_region.size;

                    miss_region = {
                        .deviceAddress = address + buffer_offset,
                        .stride = entry_size,
                        .size = miss_count * entry_size
                    };

                    memcpy(buffer_data + buffer_offset, table_data.data() + data_offset, miss_region.size);
                    buffer_offset += lava::align_up(miss_region.size, VkDeviceAddress(rt_properties.shaderGroupBaseAlignment));
                    data_offset += miss_region.size;

                    hit_region = {
                        .deviceAddress = address + buffer_offset,
                        .stride = entry_size,
                        .size = hit_count * entry_size
                    };

                    memcpy(buffer_data + buffer_offset, table_data.data() + data_offset, hit_region.size);
                    buffer_offset += lava::align_up(hit_region.size, VkDeviceAddress(rt_properties.shaderGroupBaseAlignment));
                    data_offset += hit_region.size;

                    callable_region = {
                        .deviceAddress = 0,
                        .stride = 0,
                        .size = 0
                    };

                    // TODO copy SBT to device-local memory

                    return true;
                }

                void destroy() {
                    if (buffer)
                        buffer->destroy();
                    device = nullptr;
                }

                bool valid() const {
                    return buffer && buffer->valid();
                }

                // miss/hit shader can be chosen in traceRayEXT calls inside shaders with a parameter
                // vkCmdTraceRaysKHR has no parameter to choose a raygen shader other than the one
                // at the address provided, so adjust that address
                VkStridedDeviceAddressRegionKHR get_raygen_region(lava::index index) const {
                    VkStridedDeviceAddressRegionKHR region = raygen_region;
                    region.deviceAddress += index * entry_size;
                    region.size = region.stride;
                    return region;
                }

                const VkStridedDeviceAddressRegionKHR& get_miss_region() const {
                    return miss_region;
                }

                const VkStridedDeviceAddressRegionKHR& get_hit_region() const {
                    return hit_region;
                }

                const VkStridedDeviceAddressRegionKHR& get_callable_region() const {
                    return callable_region;
                }

            private:
                lava::device_ptr device = nullptr;
                lava::buffer::ptr buffer;
                size_t entry_size = 0;

                VkStridedDeviceAddressRegionKHR raygen_region;
                VkStridedDeviceAddressRegionKHR miss_region;
                VkStridedDeviceAddressRegionKHR hit_region;
                VkStridedDeviceAddressRegionKHR callable_region;
            };

            inline shader_binding_table::ptr make_shader_binding_table() {
                return std::make_shared<shader_binding_table>();
            }

        } // namespace raytracing
    } // namespace extras
} // namespace lava
