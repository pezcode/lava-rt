#include <liblava-extras/raytracing/acceleration_structure.hpp>

namespace lava {
    namespace extras {
        namespace raytracing {

            acceleration_structure::acceleration_structure()
            : device(nullptr),
              structure(VK_NULL_HANDLE),
              handle(0),
              allocation(VK_NULL_HANDLE),
              allocation_info({}),
              allow_updates(false) {
                create_info = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_NV,
                                .compactedSize = 0, // ?
                                .info = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_INFO_NV,
                                          .flags = 0,
                                          .instanceCount = 0,
                                          .geometryCount = 0,
                                          .pGeometries = nullptr } };
            }

            void acceleration_structure::destroy() {
                if (structure != VK_NULL_HANDLE) {
                    device->call().vkDestroyAccelerationStructureNV(device->get(), structure, nullptr);
                    structure = VK_NULL_HANDLE;
                }
                handle = 0;

                if (allocation != VK_NULL_HANDLE) {
                    vmaFreeMemory(device->alloc(), allocation);
                    allocation = VK_NULL_HANDLE;
                }
            }

            VkDeviceSize acceleration_structure::scratch_buffer_size() const {
                size_t build_size = get_memory_requirements(VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_BUILD_SCRATCH_NV)
                                        .memoryRequirements.size;
                size_t update_size = 0;
                if (allow_updates)
                    update_size = get_memory_requirements(VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_UPDATE_SCRATCH_NV)
                                      .memoryRequirements.size;
                return std::max(build_size, update_size);
            }

            bool acceleration_structure::create_internal(lava::device_ptr dev) {
                device = dev;

                VkBuildAccelerationStructureFlagsNV flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_NV;
                if (allow_updates)
                    flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_NV;

                create_info.info.flags = flags;

                if (lava::failed(device->call().vkCreateAccelerationStructureNV(device->get(), &create_info, nullptr, &structure)))
                    return false;

                // allocate memory

                VkMemoryRequirements2 requirements =
                    get_memory_requirements(VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_OBJECT_NV);
                VmaAllocationCreateInfo vma_info = { //.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT
                                                     .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                     .memoryTypeBits = requirements.memoryRequirements.memoryTypeBits
                };

                if (lava::failed(vmaAllocateMemory(device->alloc(), &requirements.memoryRequirements, &vma_info, &allocation,
                                                   &allocation_info)))
                    return false;

                // bind memory to acceleration structure

                VkBindAccelerationStructureMemoryInfoNV bind_info = {
                    .sType = VK_STRUCTURE_TYPE_BIND_ACCELERATION_STRUCTURE_MEMORY_INFO_NV,
                    .accelerationStructure = structure,
                    .memory = allocation_info.deviceMemory,
                    .memoryOffset = allocation_info.offset
                };

                if (lava::failed(device->call().vkBindAccelerationStructureMemoryNV(device->get(), 1, &bind_info)))
                    return false;

                // get opaque handle
                if (lava::failed(
                        device->call().vkGetAccelerationStructureHandleNV(device->get(), structure, sizeof(handle), &handle)))
                    return false;

                return true;
            }

            VkMemoryRequirements2
                acceleration_structure::get_memory_requirements(VkAccelerationStructureMemoryRequirementsTypeNV type) const {
                const VkAccelerationStructureMemoryRequirementsInfoNV info = {
                    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_INFO_NV,
                    .type = type,
                    .accelerationStructure = structure
                };

                VkMemoryRequirements2 requirements = {};
                device->call().vkGetAccelerationStructureMemoryRequirementsNV(device->get(), &info, &requirements);
                return requirements;
            }

            bool bottom_level_acceleration_structure::create(lava::device_ptr dev) {
                create_info.info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_NV;
                create_info.info.geometryCount = uint32_t(geometries.size());
                create_info.info.pGeometries = geometries.data();

                return create_internal(dev);
            }

            void bottom_level_acceleration_structure::build(VkCommandBuffer cmd_buf, lava::buffer::ptr scratch_buffer) {
                bool update = allow_updates && built;
                VkAccelerationStructureNV prev_structure = update ? structure : VK_NULL_HANDLE;

                device->call().vkCmdBuildAccelerationStructureNV(cmd_buf, &create_info.info, VK_NULL_HANDLE, 0, update, structure,
                                                                 prev_structure, scratch_buffer->get(), 0);
                built = true;
            }

            void bottom_level_acceleration_structure::destroy() {
                geometries.clear();
                acceleration_structure::destroy();
            }

            void bottom_level_acceleration_structure::add_geometry(const VkGeometryTrianglesNV& triangles) {
                bool opaque = true;

                VkGeometryDataNV geometry_data = { .triangles = triangles,
                                                   .aabbs = { .sType = VK_STRUCTURE_TYPE_GEOMETRY_AABB_NV } };

                VkGeometryNV geometry = { .sType = VK_STRUCTURE_TYPE_GEOMETRY_NV,
                                          .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_NV,
                                          .geometry = geometry_data,
                                          .flags = opaque ? VK_GEOMETRY_OPAQUE_BIT_NV : (VkGeometryFlagsNV) 0 };

                geometries.push_back(geometry);
            }

            bool top_level_acceleration_structure::create(lava::device_ptr dev) {
                // VMA_MEMORY_USAGE_CPU_TO_GPU is not cached, so slow to read
                if (!instance_buffer.create_mapped(dev, instances.data(), sizeof(decltype(instances)::value_type) * instances.size(),
                                                   VK_BUFFER_USAGE_RAY_TRACING_BIT_NV, VMA_MEMORY_USAGE_CPU_TO_GPU))
                    return false;

                create_info.info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_NV;
                create_info.info.instanceCount = uint32_t(instances.size());

                return create_internal(dev);
            }

            void top_level_acceleration_structure::build(VkCommandBuffer cmd_buf, lava::buffer::ptr scratch_buffer) {
                bool update = allow_updates && built;
                VkAccelerationStructureNV prev_structure = update ? structure : VK_NULL_HANDLE;

                device->call().vkCmdBuildAccelerationStructureNV(cmd_buf, &create_info.info, instance_buffer.get(), 0, update,
                                                                 structure, prev_structure, scratch_buffer->get(), 0);
                built = true;
            }

            void top_level_acceleration_structure::destroy() {
                instances.clear();
                instance_buffer.destroy();
                acceleration_structure::destroy();
            }

            void top_level_acceleration_structure::add_instance(bottom_level_acceleration_structure::ptr as, lava::index index,
                                                                const glm::mat4& transform) {
                instances.push_back({ .transform = glm::mat3x4(glm::transpose(transform)),
                                      .instanceCustomIndex = index,
                                      .mask = ~0u,
                                      .instanceOffset = 0u,
                                      .flags = 0u, // TODO? VK_GEOMETRY_INSTANCE_TRIANGLE_CULL_DISABLE_BIT_NV
                                      .accelerationStructureHandle = as->get_handle() });
            }

            glm::mat4 top_level_acceleration_structure::get_transform(lava::index index) {
                glm::mat4 transform = glm::identity<glm::mat4>();

                if (instance_buffer.valid()) {
                    const VkGeometryInstanceNV* data = static_cast<VkGeometryInstanceNV*>(instance_buffer.get_mapped_data());
                    transform = glm::transpose(glm::mat4(data[index].transform));
                    transform[3][3] = 1.0f;
                }

                return transform;
            }

            void top_level_acceleration_structure::set_transform(lava::index index, const glm::mat4& transform) {
                if (instance_buffer.valid()) {
                    VkGeometryInstanceNV* data = static_cast<VkGeometryInstanceNV*>(instance_buffer.get_mapped_data());
                    data[index].transform = glm::mat3x4(glm::transpose(transform));
                }
            }

        } // namespace raytracing
    } // namespace extras
} // namespace lava
