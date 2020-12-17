#pragma once

#include <liblava/base/device.hpp>
#include <liblava/resource/buffer.hpp>
#include <liblava-extras/raytracing/pipeline.hpp>
#include <vector>
#include <memory>

// assumes shader groups added to a pipeline to be in a specific order:
// x raygen group
// y hit shaders
// y miss shaders

// no shader parameters allowed yet
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
                    const VkRayTracingShaderGroupCreateInfosNV groups = pipeline->get_shader_groups();
                    if (groups.size() < raygen_shaders)
                        return false;
                    if ((groups.size() - raygen_shaders) % 2 != 0)
                        return false;

                    raygen_count = raygen_shaders;
                    miss_count = (groups.size() - raygen_count) / 2;
                    hit_count = miss_count;

                    lava::device_ptr device = pipeline->get_device();
                    VkPhysicalDeviceProperties2 properties2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                                .pNext = &rt_properties };
                    vkGetPhysicalDeviceProperties2(device->get_vk_physical_device(), &properties2);

                    size_t handle_size = rt_properties.shaderGroupHandleSize;

                    std::vector<uint8_t> handles(handle_size * groups.size());
                    if (lava::failed(device->call().vkGetRayTracingShaderGroupHandlesNV(
                            device->get(), pipeline->get(), 0, groups.size(), handles.size(), handles.data())))
                        return false;

                    entry_size = lava::align_up(handle_size, size_t(rt_properties.shaderGroupBaseAlignment)); // + parameters
                    std::vector<uint8_t> table_data(entry_size * groups.size());
                    for (size_t i = 0; i < groups.size(); i++) {
                        memcpy(table_data.data() + (i * entry_size), handles.data() + (i * handle_size), handle_size);
                        // parameters...
                        // table_data.data() + (i * entry_size) + handle_size
                    }

                    buffer = lava::make_buffer();
                    if (!buffer->create(device, table_data.data(), table_data.size(), VK_BUFFER_USAGE_RAY_TRACING_BIT_NV, false,
                                        VMA_MEMORY_USAGE_CPU_TO_GPU))
                        return false;

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

                lava::buffer::ptr get_buffer() const {
                    return buffer;
                }

                uint32_t get_raygen_offset(lava::index index) const {
                    return index * entry_size;
                }

                uint32_t get_miss_offset() const {
                    return 0 + (raygen_count * entry_size);
                }

                uint32_t get_hit_offset() const {
                    return get_miss_offset() + (miss_count * entry_size);
                }

                uint32_t get_binding_stride() const {
                    return entry_size;
                }

            private:
                lava::device_ptr device = nullptr;
                lava::buffer::ptr buffer;

                uint32_t raygen_count = 0;
                uint32_t miss_count = 0;
                uint32_t hit_count = 0;
                uint32_t entry_size = 0;

                VkPhysicalDeviceRayTracingPropertiesNV rt_properties = {
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PROPERTIES_NV
                };
            };

            inline shader_binding_table::ptr make_shader_binding_table() {
                return std::make_shared<shader_binding_table>();
            }

        } // namespace raytracing
    } // namespace extras
} // namespace lava
