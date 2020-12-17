#include <liblava-extras/raytracing/pipeline.hpp>

namespace lava {
    namespace extras {
        namespace raytracing {

            raytracing_pipeline::raytracing_pipeline(lava::device_ptr device_, VkPipelineCache pipeline_cache)
            : pipeline(device_, pipeline_cache) {
                max_recursion_depth = 2;
            }

            void raytracing_pipeline::bind(VkCommandBuffer cmd_buf) {
                vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_RAY_TRACING_NV, vk_pipeline);
            }

            bool raytracing_pipeline::add_shader_stage(lava::data const& data, VkShaderStageFlagBits stage) {
                if (!data.ptr) {
                    lava::log()->error("raytracing pipeline shader stage data");
                    return false;
                }

                shader_stage::ptr shader_stage = lava::create_pipeline_shader_stage(device, data, stage);
                if (!shader_stage) {
                    lava::log()->error("create raytracing pipeline shader stage");
                    return false;
                }

                add(shader_stage);
                return true;
            }

            void raytracing_pipeline::add_shader_group(VkShaderStageFlagBits stage, uint32_t index) {
                VkRayTracingShaderGroupCreateInfoNV create_info = {
                    .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_NV,
                    .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_NV,
                    .generalShader = VK_SHADER_UNUSED_NV,
                    .closestHitShader = VK_SHADER_UNUSED_NV,
                    .anyHitShader = VK_SHADER_UNUSED_NV,
                    .intersectionShader = VK_SHADER_UNUSED_NV,
                };

                switch (stage) {
                case VK_SHADER_STAGE_RAYGEN_BIT_NV:
                case VK_SHADER_STAGE_MISS_BIT_NV:
                    create_info.generalShader = index;
                    break;
                case VK_SHADER_STAGE_CLOSEST_HIT_BIT_NV:
                    create_info.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_NV;
                    create_info.closestHitShader = index;
                    break;
                case VK_SHADER_STAGE_ANY_HIT_BIT_NV:
                    create_info.anyHitShader = index;
                    break;
                case VK_SHADER_STAGE_INTERSECTION_BIT_NV:
                    create_info.intersectionShader = index;
                    break;
                default:
                    assert(0 && "Unsupported shader stage");
                    break;
                }

                add_shader_group(create_info);
            }

            void raytracing_pipeline::copy_to(raytracing_pipeline* target) const {
                target->shader_groups = shader_groups;
                target->shader_stages = shader_stages;
                target->max_recursion_depth = max_recursion_depth;
            }

            bool raytracing_pipeline::create_internal() {
                lava::VkPipelineShaderStageCreateInfos stages;

                for (auto& shader_stage : shader_stages)
                    stages.push_back(shader_stage->get_create_info());

                VkRayTracingPipelineCreateInfoNV const create_info = {
                    .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_NV,
                    .stageCount = lava::to_ui32(stages.size()),
                    .pStages = stages.data(),
                    .groupCount = lava::to_ui32(shader_groups.size()),
                    .pGroups = shader_groups.data(),
                    .maxRecursionDepth = max_recursion_depth,
                    .layout = layout->get(),
                    .basePipelineHandle = VK_NULL_HANDLE,
                    .basePipelineIndex = -1,
                };

                const std::array<VkRayTracingPipelineCreateInfoNV, 1> create_infos = { create_info };

                return lava::check(
                    device->call().vkCreateRayTracingPipelinesNV(device->get(), pipeline_cache, lava::to_ui32(create_infos.size()),
                                                                 create_infos.data(), lava::memory::alloc(), &vk_pipeline));
            }

            void raytracing_pipeline::destroy_internal() {
                shader_groups.clear();
                shader_stages.clear();
            }

        } // namespace raytracing
    } // namespace extras
} // namespace lava
