#include <algorithm>
#include <liblava-extras/raytracing/acceleration_structure.hpp>

namespace lava {
    namespace extras {
        namespace raytracing {

            acceleration_structure::acceleration_structure()
            : properties({ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR }),
              create_info({ .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR }),
              build_info({ .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR }) {
            }

            bool acceleration_structure::create_internal(device_ptr dev, VkBuildAccelerationStructureFlagsKHR flags) {
                device = dev;

                VkPhysicalDeviceProperties2 properties2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                            .pNext = &properties };
                vkGetPhysicalDeviceProperties2(device->get_vk_physical_device(), &properties2);

                build_info.type = create_info.type;
                build_info.flags = flags;

                if (compact_size > 0) {
                    // set by compact() before calling create() on the new AS
                    create_info.size = compact_size;
                } else {
                    create_info.size = get_sizes().accelerationStructureSize;
                }

                as_buffer = make_buffer();
                if (!as_buffer->create(device, nullptr, create_info.size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT))
                    return false;
                create_info.buffer = as_buffer->get();

                if (!check(vkCreateAccelerationStructureKHR(device->get(), &create_info, memory::alloc(), &handle)))
                    return false;

                const VkAccelerationStructureDeviceAddressInfoKHR address_info = {
                    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                    .accelerationStructure = handle
                };
                address = device->call().vkGetAccelerationStructureDeviceAddressKHR(device->get(), &address_info);

                const VkQueryPoolCreateInfo pool_info = {
                    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                    .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                    .queryCount = 1
                };

                check(vkCreateQueryPool(device->get(), &pool_info, memory::alloc(), &query_pool));

                return true;
            }

            void acceleration_structure::destroy() {
                if (handle != VK_NULL_HANDLE) {
                    device->call().vkDestroyAccelerationStructureKHR(device->get(), handle, memory::alloc());
                    handle = VK_NULL_HANDLE;
                    address = 0;
                }

                if (query_pool != VK_NULL_HANDLE) {
                    device->call().vkDestroyQueryPool(device->get(), query_pool, memory::alloc());
                    query_pool = VK_NULL_HANDLE;
                }

                if (as_buffer) {
                    as_buffer->destroy();
                    as_buffer = nullptr;
                }

                geometries.clear();
                ranges.clear();

                built = false;
            }

            VkDeviceSize acceleration_structure::scratch_buffer_size() const {
                const VkAccelerationStructureBuildSizesInfoKHR sizes = get_sizes();
                return std::max(sizes.buildScratchSize, sizes.updateScratchSize);
            }

            bool acceleration_structure::build(VkCommandBuffer cmd_buf, VkDeviceAddress scratch_buffer) {
                if (handle == VK_NULL_HANDLE)
                    return false;
                if (built && !(build_info.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR))
                    return false;
                build_info.mode = built ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
                build_info.srcAccelerationStructure = built ? handle : VK_NULL_HANDLE;
                build_info.dstAccelerationStructure = handle;
                build_info.geometryCount = uint32_t(geometries.size());
                build_info.pGeometries = geometries.data();
                build_info.scratchData.deviceAddress = scratch_buffer;

                const VkAccelerationStructureBuildRangeInfoKHR* build_ranges = ranges.data();

                device->call().vkCmdBuildAccelerationStructuresKHR(cmd_buf, 1, &build_info, &build_ranges);
                built = true;

                if (build_info.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR) {
                    const VkMemoryBarrier barrier = {
                        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                        .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
                    };
                    device->call().vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0,
                                                        1, &barrier, 0, nullptr, 0, nullptr);
                    device->call().vkCmdWriteAccelerationStructuresPropertiesKHR(
                        cmd_buf, 1, &handle, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, query_pool, 0);
                }

                return true;
            }

            acceleration_structure::ptr acceleration_structure::compact(VkCommandBuffer cmd_buf) {
                if (!built)
                    return nullptr;
                if (!(build_info.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR))
                    return nullptr;

                acceleration_structure::ptr new_structure = nullptr;
                if (build_info.type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR)
                    new_structure = make_bottom_level_acceleration_structure();
                else if (build_info.type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR)
                    new_structure = make_top_level_acceleration_structure();
                else
                    return nullptr;

                new_structure->build_info = build_info;
                new_structure->geometries = geometries;
                new_structure->ranges = ranges;
                new_structure->built = built;

                check(device->call().vkGetQueryPoolResults(device->get(), query_pool, 0, 1, sizeof(VkDeviceSize), &new_structure->compact_size, sizeof(VkDeviceSize), VK_QUERY_RESULT_WAIT_BIT));

                if (!new_structure->create(device))
                    return nullptr;

                const VkCopyAccelerationStructureInfoKHR copy_info = {
                    .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
                    .src = handle,
                    .dst = new_structure->handle,
                    .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR
                };
                device->call().vkCmdCopyAccelerationStructureKHR(cmd_buf, &copy_info);

                return new_structure;
            }

            void acceleration_structure::add_geometry(const VkAccelerationStructureGeometryDataKHR& geometry_data, VkGeometryTypeKHR type, const VkAccelerationStructureBuildRangeInfoKHR& range, VkGeometryFlagsKHR flags) {
                if (built)
                    return;

                geometries.push_back({ .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                                       .geometryType = type,
                                       .geometry = geometry_data,
                                       .flags = flags });
                ranges.push_back(range);
            }

            VkAccelerationStructureBuildSizesInfoKHR acceleration_structure::get_sizes() const {
                build_info.pGeometries = geometries.data();
                build_info.geometryCount = uint32_t(geometries.size());

                const VkAccelerationStructureBuildTypeKHR build_type = VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR;
                std::vector<uint32_t> primitive_counts(ranges.size());
                std::transform(ranges.begin(), ranges.end(), primitive_counts.begin(),
                               [](const VkAccelerationStructureBuildRangeInfoKHR& r) { return r.primitiveCount; });

                VkAccelerationStructureBuildSizesInfoKHR info = {
                    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
                };
                device->call().vkGetAccelerationStructureBuildSizesKHR(device->get(), build_type, &build_info, primitive_counts.data(), &info);
                return info;
            }

            bool bottom_level_acceleration_structure::create(device_ptr dev, VkBuildAccelerationStructureFlagsKHR flags) {
                create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                return create_internal(dev, flags);
            }

            void bottom_level_acceleration_structure::clear_geometries() {
                geometries.clear();
                ranges.clear();
            }

            bool top_level_acceleration_structure::create(device_ptr dev, VkBuildAccelerationStructureFlagsKHR flags) {
                device = dev;

                if (!instance_buffer.create_mapped(device, instances.data(), sizeof(decltype(instances)::value_type) * instances.size(),
                                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR))
                    return false;

                const VkAccelerationStructureGeometryDataKHR geometry = {
                    .instances = {
                        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                        .arrayOfPointers = VK_FALSE,
                        .data = { .deviceAddress = instance_buffer.get_address() } }
                };
                const VkAccelerationStructureBuildRangeInfoKHR range = {
                    .primitiveCount = uint32_t(instances.size()),
                    .primitiveOffset = 0
                };
                add_geometry(geometry, VK_GEOMETRY_TYPE_INSTANCES_KHR, range);

                create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
                return create_internal(dev, flags);
            }

            void top_level_acceleration_structure::destroy() {
                instances.clear();
                instance_buffer.destroy();
                acceleration_structure::destroy();
            }

            void top_level_acceleration_structure::add_instance(const VkAccelerationStructureInstanceKHR& instance) {
                if (built)
                    return;
                instances.push_back(instance);
            }

            void top_level_acceleration_structure::add_instance(bottom_level_acceleration_structure::ptr blas) {
                if (built)
                    return;
                instances.push_back({ .transform = *reinterpret_cast<const VkTransformMatrixKHR*>(glm::value_ptr(glm::identity<glm::mat4x3>())),
                                      .mask = ~0u,
                                      .accelerationStructureReference = blas->get_address() });
            }

            void top_level_acceleration_structure::update_instance(index i, const VkAccelerationStructureInstanceKHR& instance) {
                if (i < instances.size()) {
                    instances[i] = instance;
                    if (instance_buffer.valid()) {
                        VkAccelerationStructureInstanceKHR* buffer_instances = static_cast<VkAccelerationStructureInstanceKHR*>(instance_buffer.get_mapped_data());
                        buffer_instances[i] = instance;
                    }
                }
            }

            void top_level_acceleration_structure::update_instance(index i, bottom_level_acceleration_structure::ptr blas) {
                if (i < instances.size()) {
                    instances[i].accelerationStructureReference = blas->get_address();
                    if (instance_buffer.valid()) {
                        VkAccelerationStructureInstanceKHR* buffer_instances = static_cast<VkAccelerationStructureInstanceKHR*>(instance_buffer.get_mapped_data());
                        buffer_instances[i].accelerationStructureReference = blas->get_address();
                    }
                }
            }

            void top_level_acceleration_structure::set_instance_transform(index i, const glm::mat4x3& transform) {
                static_assert(sizeof(glm::mat4x3) == sizeof(VkTransformMatrixKHR::matrix));
                if (i < instances.size()) {
                    const glm::mat3x4 transposed = glm::transpose(transform);
                    const VkTransformMatrixKHR& transform_ref = *reinterpret_cast<const VkTransformMatrixKHR*>(glm::value_ptr(transposed));
                    instances[i].transform = transform_ref;
                    if (instance_buffer.valid()) {
                        VkAccelerationStructureInstanceKHR* buffer_instances = static_cast<VkAccelerationStructureInstanceKHR*>(instance_buffer.get_mapped_data());
                        buffer_instances[i].transform = transform_ref;
                    }
                }
            }

            void top_level_acceleration_structure::clear_instances() {
                geometries.clear();
                ranges.clear();
                instances.clear();
            }

        } // namespace raytracing
    } // namespace extras
} // namespace lava
