#include <liblava-extras/raytracing/pipeline.hpp>

namespace lava {
    namespace extras {
        namespace raytracing {

            raytracing_pipeline::raytracing_pipeline(device_p device_, VkPipelineCache pipeline_cache)
            : pipeline(device_, pipeline_cache),
              properties({ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR }),
              max_recursion_depth(1) {
                VkPhysicalDeviceProperties2 properties2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                            .pNext = &properties };
                vkGetPhysicalDeviceProperties2(device->get_vk_physical_device(), &properties2);
            }

            void raytracing_pipeline::bind(VkCommandBuffer cmd_buf) {
                vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vk_pipeline);
            }

            bool raytracing_pipeline::add_shader_stage(cdata const& data, VkShaderStageFlagBits stage) {
                if (!data.ptr) {
                    log()->error("raytracing pipeline shader stage data");
                    return false;
                }

                shader_stage::ptr shader_stage = create_pipeline_shader_stage(device, data, stage);
                if (!shader_stage) {
                    log()->error("create raytracing pipeline shader stage");
                    return false;
                }

                add(shader_stage);
                return true;
            }

            void raytracing_pipeline::add_shader_general_group(uint32_t index) {
                add_shader_group({ .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                   .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
                                   .generalShader = index,
                                   .closestHitShader = VK_SHADER_UNUSED_KHR,
                                   .anyHitShader = VK_SHADER_UNUSED_KHR,
                                   .intersectionShader = VK_SHADER_UNUSED_KHR });
            }

            void raytracing_pipeline::add_shader_hit_group(uint32_t closest_hit_index, uint32_t any_hit_index, uint32_t intersection_index) {
                add_shader_group({
                    .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                    .type = intersection_index == VK_SHADER_UNUSED_KHR ? VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR : VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR,
                    .generalShader = VK_SHADER_UNUSED_KHR,
                    .closestHitShader = closest_hit_index,
                    .anyHitShader = any_hit_index,
                    .intersectionShader = intersection_index,
                });
            }

            void raytracing_pipeline::copy_to(raytracing_pipeline* target) const {
                target->shader_groups = shader_groups;
                target->shader_stages = shader_stages;
                target->max_recursion_depth = max_recursion_depth;
            }

            bool raytracing_pipeline::setup() {
                VkPipelineShaderStageCreateInfos stages;

                for (const auto& shader_stage : shader_stages)
                    stages.push_back(shader_stage->get_create_info());

                const VkRayTracingPipelineCreateInfoKHR create_info = {
                    .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
                    .stageCount = to_ui32(stages.size()),
                    .pStages = stages.data(),
                    .groupCount = to_ui32(shader_groups.size()),
                    .pGroups = shader_groups.data(),
                    .maxPipelineRayRecursionDepth = max_recursion_depth,
                    .layout = layout->get()
                };

                return check(
                    device->call().vkCreateRayTracingPipelinesKHR(device->get(), VK_NULL_HANDLE, pipeline_cache, 1,
                                                                  &create_info, memory::alloc(), &vk_pipeline));
            }

            void raytracing_pipeline::teardown() {
                shader_groups.clear();
                shader_stages.clear();
            }

        } // namespace raytracing
    } // namespace extras
} // namespace lava
