#pragma once

#include <liblava-extras/raytracing/pipeline.hpp>
#include <liblava/base/device.hpp>
#include <liblava/resource/buffer.hpp>
#include <memory>
#include <vector>

// assumes shader groups were added to the pipeline in the following order:
// raygen 1...X
// miss 1...Y
// hit 1...Z
// callable 1...W

namespace lava {
    namespace extras {
        namespace raytracing {

            struct shader_binding_table {
                using ptr = std::shared_ptr<shader_binding_table>;

                ~shader_binding_table() {
                    destroy();
                };

                bool create(raytracing_pipeline::ptr pipeline, std::vector<cdata> records = std::vector<cdata>()) {
                    device = pipeline->get_device();

                    size_t group_counts[group_type::count] = {}; // number of shader groups of each type
                    size_t record_sizes[group_type::count] = {}; // largest record size per type, to calculate stride

                    // extract shader count and record size from group info and shader stages

                    const VkRayTracingShaderGroupCreateInfosKHR& groups = pipeline->get_shader_groups();
                    records.resize(groups.size()); // fill with empty ptr/size if necessary

                    const pipeline::shader_stage::list stages = pipeline->get_shader_stages();
                    for (size_t i = 0; i < groups.size(); i++) {
                        const VkRayTracingShaderGroupCreateInfoKHR& group = groups[i];
                        switch (group.type) {
                        case VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR:
                            assert(group.generalShader != VK_SHADER_UNUSED_KHR);
                            switch (stages[group.generalShader]->get_create_info().stage) {
                            case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
                                assert(group_counts[miss] == 0 && group_counts[hit] == 0 && group_counts[callable] == 0);
                                group_counts[raygen]++;
                                record_sizes[raygen] = std::max(record_sizes[raygen], records[i].size);
                                break;
                            case VK_SHADER_STAGE_MISS_BIT_KHR:
                                assert(group_counts[hit] == 0 && group_counts[callable] == 0);
                                group_counts[miss]++;
                                record_sizes[miss] = std::max(record_sizes[miss], records[i].size);
                                break;
                            case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
                                group_counts[callable]++;
                                record_sizes[callable] = std::max(record_sizes[callable], records[i].size);
                                break;
                            default:
                                assert(false && "unknown raytracing shader stage");
                                break;
                            }
                            break;
                        case VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR:
                        case VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR:
                            assert(group_counts[callable] == 0);
                            group_counts[hit]++;
                            record_sizes[hit] = std::max(record_sizes[hit], records[i].size);
                            break;
                        default:
                            assert(false && "unknown raytracing shader group type");
                            break;
                        }
                    }

                    assert(group_counts[raygen] >= 1);
                    if (groups.size() < (group_counts[raygen] + group_counts[miss] + group_counts[hit] + group_counts[callable]))
                        return false;

                    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& rt_properties = pipeline->get_properties();
                    const size_t handle_size = rt_properties.shaderGroupHandleSize;

                    std::vector<uint8_t> handles(handle_size * groups.size());
                    if (!check(device->call().vkGetRayTracingShaderGroupHandlesKHR(
                            device->get(), pipeline->get(), 0, groups.size(), handles.size(), handles.data())))
                        return false;

                    // shaderGroupBaseAlignment must be a multiple of shaderGroupHandleAlignment (or else you couldn't use the SBT base address as the first entry)
                    // so it's enough to round up the group entry size once we have an aligned SBT base address

                    size_t strides[group_type::count] = {}; // size of a shader group entry, must be the same for each type
                    size_t sbt_sizes[group_type::count] = {}; // size of the entire SBT per type, this includes padding for alignment of the next group

                    size_t cur_group = 0;
                    std::vector<uint8_t> table_data;
                    for (size_t i = 0; i < group_type::count; i++) {
                        strides[i] = align_up<VkDeviceSize>(handle_size + record_sizes[i], rt_properties.shaderGroupHandleAlignment);
                        sbt_sizes[i] = align_up<VkDeviceSize>(group_counts[i] * strides[i], rt_properties.shaderGroupBaseAlignment);
                        size_t offset = table_data.size();
                        table_data.insert(table_data.end(), sbt_sizes[i], 0);
                        for (size_t c = 0; c < group_counts[i]; c++) {
                            memcpy(&table_data[offset], &handles[cur_group * handle_size], handle_size);
                            if (records[cur_group].ptr) {
                                memcpy(&table_data[offset + handle_size], records[cur_group].ptr, records[cur_group].size);
                            }
                            offset += strides[i];
                            cur_group++;
                        }
                    }

                    const size_t possible_padding = rt_properties.shaderGroupBaseAlignment - 1;
                    sbt_buffer = make_buffer();
                    if (!sbt_buffer->create_mapped(device, nullptr, table_data.size() + possible_padding, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT))
                        return false;
                    const VkDeviceAddress buffer_address = sbt_buffer->get_address();

                    uint8_t* buffer_data = static_cast<uint8_t*>(sbt_buffer->get_mapped_data());
                    size_t buffer_offset = align_up<VkDeviceAddress>(buffer_address, rt_properties.shaderGroupBaseAlignment) - buffer_address;

                    memcpy(&buffer_data[buffer_offset], table_data.data(), table_data.size());

                    for (size_t i = 0; i < group_type::count; i++) {
                        regions[i] = {
                            .deviceAddress = buffer_address + buffer_offset,
                            .stride = strides[i],
                            .size = group_counts[i] * strides[i]
                        };
                        buffer_offset += sbt_sizes[i];
                    }

                    // TODO copy SBT to device-local memory

                    return true;
                }

                void destroy() {
                    if (sbt_buffer)
                        sbt_buffer->destroy();
                    device = nullptr;
                }

                device_ptr get_device() {
                    return device;
                }

                bool valid() const {
                    return sbt_buffer && sbt_buffer->valid();
                }

                // miss/hit/callable shader can be chosen in traceRayEXT calls inside shaders with a parameter
                // vkCmdTraceRaysKHR has no parameter to choose a raygen shader other than the one
                // at the address provided, so adjust that address
                VkStridedDeviceAddressRegionKHR get_raygen_region(index index = 0) const {
                    VkStridedDeviceAddressRegionKHR region = regions[raygen];
                    region.deviceAddress += index * region.stride;
                    region.size = region.stride;
                    return region;
                }

                const VkStridedDeviceAddressRegionKHR& get_miss_region() const {
                    return regions[miss];
                }

                const VkStridedDeviceAddressRegionKHR& get_hit_region() const {
                    return regions[hit];
                }

                const VkStridedDeviceAddressRegionKHR& get_callable_region() const {
                    return regions[callable];
                }

            private:
                device_ptr device = nullptr;
                buffer::ptr sbt_buffer;

                enum group_type : size_t {
                    raygen = 0,
                    miss,
                    hit,
                    callable,
                    count
                };

                VkStridedDeviceAddressRegionKHR regions[group_type::count];
            };

            inline shader_binding_table::ptr make_shader_binding_table() {
                return std::make_shared<shader_binding_table>();
            }

        } // namespace raytracing
    } // namespace extras
} // namespace lava
