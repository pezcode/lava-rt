#pragma once

#include <liblava/base/device.hpp>
#include <liblava/resource/buffer.hpp>
#include <glm/mat4x3.hpp>
#include <glm/mat4x4.hpp>
#include <vector>
#include <memory>

namespace lava {
    namespace extras {
        namespace raytracing {

            struct acceleration_structure {
                using ptr = std::shared_ptr<acceleration_structure>;

                typedef uint64_t handle_t;

                acceleration_structure();

                virtual ~acceleration_structure() {
                    destroy();
                }

                void set_allow_updates(bool allow = true) {
                    allow_updates = allow;
                }

                virtual bool create(lava::device_ptr device) = 0;
                virtual void build(VkCommandBuffer cmd_buf, lava::buffer::ptr scratch_buffer) = 0;
                virtual void destroy();

                VkAccelerationStructureNV get() const {
                    return structure;
                }

                handle_t get_handle() const {
                    return handle;
                }

                VkDeviceSize scratch_buffer_size() const;

            protected:
                lava::device_ptr device = nullptr;
                VkAccelerationStructureCreateInfoNV create_info;

                VkAccelerationStructureNV structure = VK_NULL_HANDLE;
                handle_t handle = 0;

                VmaAllocation allocation = VK_NULL_HANDLE;
                VmaAllocationInfo allocation_info = {};

                bool allow_updates = false;
                bool built = false;

                bool create_internal(lava::device_ptr dev);
                VkMemoryRequirements2 get_memory_requirements(VkAccelerationStructureMemoryRequirementsTypeNV type) const;
            };

            struct bottom_level_acceleration_structure : acceleration_structure {
                using ptr = std::shared_ptr<bottom_level_acceleration_structure>;
                using map = std::map<lava::id, ptr>;
                using list = std::vector<ptr>;

                virtual bool create(lava::device_ptr device) override;
                virtual void build(VkCommandBuffer cmd_buf, lava::buffer::ptr scratch_buffer) override;
                virtual void destroy() override;

                void add_geometry(const VkGeometryTrianglesNV& triangles);

            private:
                std::vector<VkGeometryNV> geometries;
            };

            struct top_level_acceleration_structure : acceleration_structure {
                using ptr = std::shared_ptr<top_level_acceleration_structure>;
                using map = std::map<lava::id, ptr>;
                using list = std::vector<ptr>;

                virtual bool create(lava::device_ptr device) override;
                virtual void build(VkCommandBuffer cmd_buf, lava::buffer::ptr scratch_buffer) override;
                virtual void destroy() override;

                // column-major transformation matrix
                void add_instance(bottom_level_acceleration_structure::ptr as, lava::index index, const glm::mat4& transform);

                glm::mat4 get_transform(lava::index index);
                void set_transform(lava::index index, const glm::mat4& transform);

            private:
                struct VkGeometryInstanceNV {
                    // row-major 4x3 transformation matrix
                    glm::mat3x4 transform;
                    // appears as gl_InstanceCustomIndexNV
                    uint32_t instanceCustomIndex : 24;
                    // visibility mask (&'ed with rayMask)
                    uint32_t mask : 8;
                    // hit group index
                    uint32_t instanceOffset : 24;
                    // flags for culling etc.
                    uint32_t flags : 8;
                    // opaque handle to bottom level acceleration structure
                    bottom_level_acceleration_structure::handle_t accelerationStructureHandle;
                };
                std::vector<VkGeometryInstanceNV> instances;
                lava::buffer instance_buffer;
            };

            inline bottom_level_acceleration_structure::ptr make_bottom_level_acceleration_structure() {
                return std::make_shared<bottom_level_acceleration_structure>();
            }

            inline top_level_acceleration_structure::ptr make_top_level_acceleration_structure() {
                return std::make_shared<top_level_acceleration_structure>();
            }

        } // namespace raytracing
    } // namespace extras
} // namespace lava
