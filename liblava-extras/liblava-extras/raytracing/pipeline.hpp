#pragma once

#include <liblava/block/pipeline.hpp>
#include <memory>

namespace lava {
    namespace extras {
        namespace raytracing {

            using VkRayTracingShaderGroupCreateInfosKHR = std::vector<VkRayTracingShaderGroupCreateInfoKHR>;

            struct raytracing_pipeline : lava::pipeline {
                using ptr = std::shared_ptr<raytracing_pipeline>;
                using map = std::map<lava::id, ptr>;
                using list = std::vector<ptr>;

                using pipeline::pipeline;

                explicit raytracing_pipeline(lava::device_ptr device, VkPipelineCache pipeline_cache = VK_NULL_HANDLE);

                void bind(VkCommandBuffer cmdBuffer) override;

                const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& get_properties() const {
                    return properties;
                }

                bool add_shader_stage(lava::data const& data, VkShaderStageFlagBits stage);
                bool add_shader(lava::data const& data, VkShaderStageFlagBits stage) {
                    return add_shader_stage(data, stage);
                }

                void add(shader_stage::ptr const& shader_stage) {
                    shader_stages.push_back(shader_stage);
                }

                shader_stage::list const& get_shader_stages() const {
                    return shader_stages;
                }
                void clear_shader_stages() {
                    shader_stages.clear();
                }

                // convenience function for triangle hit groups, don't use if you want procedural hit groups
                void add_shader_group(VkShaderStageFlagBits stage, uint32_t index);
                void add_shader_group(const VkRayTracingShaderGroupCreateInfoKHR& shader_group) {
                    shader_groups.push_back(shader_group);
                }

                const VkRayTracingShaderGroupCreateInfosKHR& get_shader_groups() const {
                    return shader_groups;
                }
                void clear_shader_groups() {
                    shader_groups.clear();
                }

                uint32_t get_max_recursion_depth() {
                    return max_recursion_depth;
                }
                void set_max_recursion_depth(uint32_t depth) {
                    max_recursion_depth = std::min(properties.maxRayRecursionDepth, depth);
                }

                void copy_to(raytracing_pipeline* target) const;
                void copy_from(ptr const& source) {
                    source->copy_to(this);
                }

            private:
                bool create_internal() override;
                void destroy_internal() override;

                VkPhysicalDeviceRayTracingPipelinePropertiesKHR properties;

                VkRayTracingShaderGroupCreateInfosKHR shader_groups;
                shader_stage::list shader_stages;
                uint32_t max_recursion_depth;
            };

            inline raytracing_pipeline::ptr make_raytracing_pipeline(lava::device_ptr device,
                                                                     VkPipelineCache pipeline_cache = VK_NULL_HANDLE) {
                return std::make_shared<raytracing_pipeline>(device, pipeline_cache);
            }

        } // namespace raytracing
    } // namespace extras
} // namespace lava
