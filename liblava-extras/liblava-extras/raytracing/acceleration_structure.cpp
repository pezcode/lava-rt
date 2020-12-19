#include <liblava-extras/raytracing/acceleration_structure.hpp>

#include <algorithm>

namespace lava {
    namespace extras {
        namespace raytracing {

            acceleration_structure::acceleration_structure()
            : create_info({ .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR }),
              build_info({ .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR }),
              allow_updates(false),
              allow_compaction(false) {
            }

            bool acceleration_structure::create_internal(lava::device_ptr dev) {
                device = dev;

                VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR; // TODO
                if (allow_updates)
                    flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
                if (allow_compaction)
                    flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;

                build_info.type = create_info.type;
                build_info.flags = flags;

                const VkAccelerationStructureBuildSizesInfoKHR sizes = get_sizes();

                as_buffer = make_buffer();
                as_buffer->create(device, nullptr, sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

                create_info.buffer = as_buffer->get();

                if (!lava::check(vkCreateAccelerationStructureKHR(device->get(), &create_info, nullptr, &handle)))
                    return false;

                const VkAccelerationStructureDeviceAddressInfoKHR address_info = {
                    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                    .accelerationStructure = handle
                };
                address = device->call().vkGetAccelerationStructureDeviceAddressKHR(device->get(), &address_info);

                return true;
            }

            void acceleration_structure::destroy() {
                if (handle != VK_NULL_HANDLE) {
                    device->call().vkDestroyAccelerationStructureKHR(device->get(), handle, nullptr);
                    handle = VK_NULL_HANDLE;
                    address = 0;
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
                return std::max(sizes.buildScratchSize, allow_updates ? sizes.updateScratchSize : 0);
            }

            bool acceleration_structure::build(VkCommandBuffer cmd_buf, VkDeviceAddress scratch_buffer) {
                if (handle == VK_NULL_HANDLE)
                    return false;

                bool update = allow_updates && built;

                build_info.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
                build_info.srcAccelerationStructure = update ? handle : VK_NULL_HANDLE;
                build_info.dstAccelerationStructure = handle;
                build_info.geometryCount = uint32_t(geometries.size());
                build_info.pGeometries = geometries.data();
                build_info.scratchData.deviceAddress = scratch_buffer;

                const VkAccelerationStructureBuildRangeInfoKHR* build_ranges = ranges.data();

                device->call().vkCmdBuildAccelerationStructuresKHR(cmd_buf, 1, &build_info, &build_ranges);
                built = true;

                return true;
            }

            void acceleration_structure::add_geometry(const VkAccelerationStructureGeometryDataKHR& geometry_data, VkGeometryTypeKHR type, const VkAccelerationStructureBuildRangeInfoKHR& range) {
                bool opaque = true; // TODO
                VkGeometryFlagsKHR flags = 0;
                if (opaque)
                    flags |= VK_GEOMETRY_OPAQUE_BIT_KHR;

                geometries.push_back({ .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                                       .geometryType = type,
                                       .geometry = geometry_data,
                                       .flags = flags });

                ranges.push_back(range);
            }

            VkAccelerationStructureBuildSizesInfoKHR acceleration_structure::get_sizes() const {
                build_info.pGeometries = geometries.data();
                build_info.geometryCount = uint32_t(geometries.size());

                const VkAccelerationStructureBuildTypeKHR build_type = VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR; // TODO? host build support
                std::vector<uint32_t> primitive_counts(ranges.size());
                std::transform(ranges.begin(), ranges.end(), primitive_counts.begin(),
                               [](const VkAccelerationStructureBuildRangeInfoKHR& r) { return r.primitiveCount; });

                VkAccelerationStructureBuildSizesInfoKHR info = {
                    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
                };
                device->call().vkGetAccelerationStructureBuildSizesKHR(device->get(), build_type, &build_info, primitive_counts.data(), &info);
                return info;
            }

            bool bottom_level_acceleration_structure::create(lava::device_ptr dev) {
                create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                return create_internal(dev);
            }

            void bottom_level_acceleration_structure::clear_geometries() {
                geometries.clear();
                ranges.clear();
            }

            bool top_level_acceleration_structure::create(lava::device_ptr dev) {
                device = dev;

                if (!instance_buffer.create_mapped(device, instances.data(), sizeof(decltype(instances)::value_type) * instances.size(),
                                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR))
                    return false;

                const VkBufferDeviceAddressInfoKHR addr_info = {
                    .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR,
                    .buffer = instance_buffer.get()
                };
                VkDeviceAddress instance_buffer_address = device->call().vkGetBufferDeviceAddressKHR(device->get(), &addr_info);

                const VkAccelerationStructureGeometryDataKHR geometry = {
                    .instances = {
                        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                        .arrayOfPointers = VK_FALSE,
                        .data = { .deviceAddress = instance_buffer_address } }
                };
                const VkAccelerationStructureBuildRangeInfoKHR range = {
                    .primitiveCount = uint32_t(instances.size()),
                    .primitiveOffset = 0
                };
                add_geometry(geometry, VK_GEOMETRY_TYPE_INSTANCES_KHR, range);

                create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
                return create_internal(dev);
            }

            void top_level_acceleration_structure::destroy() {
                instances.clear();
                instance_buffer.destroy();
                acceleration_structure::destroy();
            }

            void top_level_acceleration_structure::add_instance(bottom_level_acceleration_structure::ptr as, lava::index custom_index,
                                                                const glm::mat4x3& transform) {
                instances.push_back({ .transform = *reinterpret_cast<const VkTransformMatrixKHR*>(glm::value_ptr(glm::transpose(transform))),
                                      .instanceCustomIndex = custom_index,
                                      .mask = ~0u,
                                      .instanceShaderBindingTableRecordOffset = 0u,
                                      .flags = 0u, // TODO
                                      .accelerationStructureReference = as->get_address() });
            }

            void top_level_acceleration_structure::set_transform(lava::index index, const glm::mat4x3& transform) {
                static_assert(sizeof(glm::mat4x3) == sizeof(VkTransformMatrixKHR::matrix));
                if (instance_buffer.valid()) {
                    VkAccelerationStructureInstanceKHR* data = static_cast<VkAccelerationStructureInstanceKHR*>(instance_buffer.get_mapped_data());
                    data[index].transform = *reinterpret_cast<const VkTransformMatrixKHR*>(glm::value_ptr(glm::transpose(transform)));
                }
            }

        } // namespace raytracing
    } // namespace extras
} // namespace lava
