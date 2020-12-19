#pragma once

#include <glm/mat4x3.hpp>
#include <liblava/base/device.hpp>
#include <liblava/resource/buffer.hpp>
#include <memory>
#include <vector>

namespace lava {
    namespace extras {
        namespace raytracing {

            struct acceleration_structure {
                using ptr = std::shared_ptr<acceleration_structure>;

                acceleration_structure();

                virtual ~acceleration_structure() {
                    destroy();
                }

                void set_allow_updates(bool allow = true) {
                    allow_updates = allow;
                }

                void set_allow_compaction(bool allow = true) {
                    allow_compaction = allow;
                }

                virtual bool create(lava::device_ptr device) = 0;
                virtual void destroy();

                // TODO host command versions of build and compact

                bool build(VkCommandBuffer cmd_buf, VkDeviceAddress scratch_buffer);
                acceleration_structure::ptr compact(VkCommandBuffer cmd_buf);

                VkAccelerationStructureKHR get() const {
                    return handle;
                }

                VkDeviceAddress get_address() const {
                    return address;
                }

                VkDeviceSize scratch_buffer_size() const;

            protected:
                lava::device_ptr device = nullptr;
                VkAccelerationStructureCreateInfoKHR create_info;
                mutable VkAccelerationStructureBuildGeometryInfoKHR build_info;

                VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
                VkDeviceAddress address = 0;

                VkQueryPool query_pool = VK_NULL_HANDLE;

                buffer::ptr as_buffer;

                std::vector<VkAccelerationStructureGeometryKHR> geometries;
                std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;

                bool allow_updates;
                bool allow_compaction;

                // this is set on the newly created acceleration structure by compact()
                VkDeviceSize compact_size = 0;

                bool built = false;

                bool create_internal(lava::device_ptr dev);
                void add_geometry(const VkAccelerationStructureGeometryDataKHR& geometry_data, VkGeometryTypeKHR type, const VkAccelerationStructureBuildRangeInfoKHR& range);
                VkAccelerationStructureBuildSizesInfoKHR get_sizes() const;
            };

            struct bottom_level_acceleration_structure : acceleration_structure {
                using ptr = std::shared_ptr<bottom_level_acceleration_structure>;
                using map = std::map<lava::id, ptr>;
                using list = std::vector<ptr>;

                virtual bool create(lava::device_ptr device) override;

                // TODO AABB support

                void add_geometry(const VkAccelerationStructureGeometryTrianglesDataKHR& triangles, const VkAccelerationStructureBuildRangeInfoKHR& range) {
                    acceleration_structure::add_geometry({ .triangles = triangles }, VK_GEOMETRY_TYPE_TRIANGLES_KHR, range);
                }
                /*
                void add_geometry(const VkAccelerationStructureGeometryAabbsDataKHR& aabbs, const VkAccelerationStructureBuildRangeInfoKHR& range) {
                    acceleration_structure::add_geometry({ .aabbs = aabbs }, VK_GEOMETRY_TYPE_AABBS_KHR);
                }
                */

                void clear_geometries();
            };

            struct top_level_acceleration_structure : acceleration_structure {
                using ptr = std::shared_ptr<top_level_acceleration_structure>;
                using map = std::map<lava::id, ptr>;
                using list = std::vector<ptr>;

                virtual bool create(lava::device_ptr device) override;
                virtual void destroy() override;

                // custom_index is optional and corresponds to gl_InstanceCustomIndex in hit shaders
                // without custom index, gl_InstanceID is just the increasing index of the instances being added
                void add_instance(bottom_level_acceleration_structure::ptr as, lava::index custom_index = 0, const glm::mat4x3& transform = glm::identity<glm::mat4x3>());
                void update_instance(lava::index i, bottom_level_acceleration_structure::ptr blas, lava::index custom_index = 0, const glm::mat4x3& transform = glm::identity<glm::mat4x3>());

                // index here is the actual index, not the custom index!
                void set_instance_transform(lava::index i, const glm::mat4x3& transform);

                void clear_instances();

            private:
                std::vector<VkAccelerationStructureInstanceKHR> instances;
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
