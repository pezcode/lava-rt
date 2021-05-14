#include <imgui.h>
#include <algorithm>
#include <demo.hpp>
#include <glm/gtc/color_space.hpp>
#include <liblava-extras/raytracing.hpp>
#include <liblava/lava.hpp>

using namespace lava;
using namespace lava::extras::raytracing;

struct uniform_data {
    glm::mat4 inv_view;
    glm::mat4 inv_proj;
    glm::uvec4 viewport;
    glm::vec4 background_color;
    uint32_t max_depth;
} uniforms;

struct instance_data {
    uint32_t vertex_base;
    uint32_t vertex_count;
    uint32_t index_base;
    uint32_t index_count;
};

int main(int argc, char* argv[]) {
    frame_config config;
    config.info.app_name = "lava raytracing cubes";
    config.cmd_line = { argc, argv };
    config.info.req_api_version = api_version::v1_1;

    app app(config);

    app.config.surface.formats = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB };

    device::ptr device = create_raytracing_device(app.manager);
    if (!device)
        return error::not_ready;
    app.device = device.get();

    if (!app.setup())
        return error::not_ready;

    // the command buffer used for vkCmdBuildAccelerationStructureKHR and vkCmdTraceRaysKHR must support compute
    // lava's default queue has graphics, compute and transfer support and the Vulkan spec guarantees that
    // this combination exists as long as the device supports graphics queues
    queue::ref queue = app.device->graphics_queue();

    const size_t uniform_stride = align_up(sizeof(uniform_data), app.device->get_physical_device()->get_properties().limits.minUniformBufferOffsetAlignment);

    mesh::ptr cube = create_mesh(app.device, mesh_type::cube);
    if (!cube)
        return error::create_failed;
    mesh_data& mesh = cube->get_data();
    mesh.scale(0.333f);

    std::vector<instance_data> instances;
    std::vector<vertex> vertices;
    std::vector<index> indices;

    // combined vertex and index buffers for all meshes

    constexpr size_t INSTANCE_COUNT = 2;
    const glm::vec3 instance_colors[INSTANCE_COUNT] = {
        glm::vec3(0.812f, 0.063f, 0.125f),
        glm::vec3(0.063f, 0.812f, 0.749f)
    };

    for (size_t i = 0; i < INSTANCE_COUNT; i++) {
        const instance_data instance = { .vertex_base = uint32_t(vertices.size()),
                                         .vertex_count = uint32_t(mesh.vertices.size()),
                                         .index_base = uint32_t(indices.size()),
                                         .index_count = uint32_t(mesh.indices.size()) };
        instances.push_back(instance);
        vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        std::for_each(vertices.begin() + instance.vertex_base, vertices.end(), [&](vertex& v) {
            v.color = { glm::convertSRGBToLinear(instance_colors[i]), 1.0f };
        });
        indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
    }

    cube->destroy();
    cube = nullptr;

    VkCommandPool pool = VK_NULL_HANDLE;
    descriptor::pool::ptr descriptor_pool;

    pipeline_layout::ptr blit_pipeline_layout;
    graphics_pipeline::ptr blit_pipeline;

    descriptor::ptr shared_descriptor_set_layout;
    VkDescriptorSet shared_descriptor_set;

    pipeline_layout::ptr raytracing_pipeline_layout;
    raytracing_pipeline::ptr raytracing_pipeline;

    shader_binding_table::ptr shader_binding;

    descriptor::ptr raytracing_descriptor_set_layout;
    VkDescriptorSet raytracing_descriptor_set;

    top_level_acceleration_structure::ptr top_as;
    bottom_level_acceleration_structure::list bottom_as_list;

    buffer::ptr scratch_buffer;
    VkDeviceAddress scratch_buffer_address = 0;

    buffer::ptr instance_buffer;
    buffer::ptr vertex_buffer;
    buffer::ptr index_buffer;

    buffer::ptr uniform_buffer;

    image::ptr output_image;

    // catch swapchain recreation
    // recreate raytracing image and update its descriptors
    target_callback swapchain_callback;

    swapchain_callback.on_created =
        [&](VkAttachmentsRef, rect area) {
            const glm::uvec2 size = area.get_size();
            uniforms.inv_proj = glm::inverse(perspective_matrix(size, 90.0f, 5.0f));
            uniforms.viewport = { area.get_origin(), size };

            if (!output_image->create(app.device, size))
                return false;

            // update image descriptor
            const VkDescriptorImageInfo image_info = { .sampler = VK_NULL_HANDLE,
                                                       .imageView = output_image->get_view(),
                                                       .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
            const VkWriteDescriptorSet write_info = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                      .dstSet = shared_descriptor_set,
                                                      .dstBinding = 1,
                                                      .descriptorCount = 1,
                                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                      .pImageInfo = &image_info };
            app.device->vkUpdateDescriptorSets({ write_info });

            // transition image to general layout
            return one_time_command_buffer(
                app.device, pool, queue, [&](VkCommandBuffer cmd_buf) {
                    insert_image_memory_barrier(app.device, cmd_buf, output_image->get(), 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, output_image->get_subresource_range());
                });
        };

    swapchain_callback.on_destroyed = [&]() {
        app.device->wait_for_idle();
        output_image->destroy();
    };

    app.target->add_callback(&swapchain_callback);

    app.on_create = [&]() {
        // command pool for one-time command buffers
        const VkCommandPoolCreateInfo create_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                                      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                                      .queueFamilyIndex = uint32_t(queue.family) };
        if (!app.device->vkCreateCommandPool(&create_info, &pool))
            return false;

        descriptor_pool = make_descriptor_pool();
        constexpr uint32_t set_count = 2;
        const VkDescriptorPoolSizes sizes = {
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 },
            { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV, 1 }
        };
        if (!descriptor_pool->create(app.device, sizes, set_count, 0))
            return false;

        // uniform buffer for camera parameters and background color
        uniform_buffer = make_buffer();
        if (!uniform_buffer->create_mapped(app.device, nullptr, app.target->get_frame_count() * uniform_stride, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
            return false;

        // output image for the raytracing shader
        // RGBA16F is guaranteed to support these usage flags
        VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
        output_image = make_image(format);
        output_image->set_usage(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        output_image->set_layout(VK_IMAGE_LAYOUT_UNDEFINED);
        output_image->set_aspect_mask(format_aspect_mask(format));

        // descriptor set used by the raytracing shaders and the blit shader
        shared_descriptor_set_layout = make_descriptor();
        shared_descriptor_set_layout->add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR);
        shared_descriptor_set_layout->add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR);
        if (!shared_descriptor_set_layout->create(app.device))
            return false;

        shared_descriptor_set = shared_descriptor_set_layout->allocate(descriptor_pool->get());

        // blit pipeline that draws the raytraced output image to the swapchain
        blit_pipeline_layout = make_pipeline_layout();
        blit_pipeline_layout->add(shared_descriptor_set_layout);
        if (!blit_pipeline_layout->create(app.device))
            return false;

        blit_pipeline = make_graphics_pipeline(app.device);

        if (!blit_pipeline->add_shader(file_data("cubes/vert.spv"), VK_SHADER_STAGE_VERTEX_BIT))
            return false;
        if (!blit_pipeline->add_shader(file_data("cubes/frag.spv"), VK_SHADER_STAGE_FRAGMENT_BIT))
            return false;

        blit_pipeline->add_color_blend_attachment();
        blit_pipeline->set_layout(blit_pipeline_layout);

        auto render_pass = app.shading.get_pass();
        if (!blit_pipeline->create(render_pass->get()))
            return false;

        blit_pipeline->on_process = [&](VkCommandBuffer cmd_buf) {
            const uint32_t uniform_offset = app.block.get_current_frame() * uniform_stride;
            app.device->call().vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, blit_pipeline_layout->get(), 0, 1, &shared_descriptor_set, 1, &uniform_offset);
            // fullscreen triangle
            // no vertex buffer, attributes are generated in the vertex shader
            app.device->call().vkCmdDraw(cmd_buf, 3, 1, 0, 0);
        };

        // add blit before lava's gui rendering
        render_pass->add_front(blit_pipeline);

        // descriptor used by the raytracing shader
        raytracing_descriptor_set_layout = make_descriptor();
        raytracing_descriptor_set_layout->add_binding(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
        raytracing_descriptor_set_layout->add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
        raytracing_descriptor_set_layout->add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
        raytracing_descriptor_set_layout->add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
        if (!raytracing_descriptor_set_layout->create(app.device))
            return false;

        raytracing_pipeline_layout = make_pipeline_layout();
        raytracing_pipeline_layout->add(shared_descriptor_set_layout);
        raytracing_pipeline_layout->add(raytracing_descriptor_set_layout);
        if (!raytracing_pipeline_layout->create(app.device))
            return false;

        raytracing_descriptor_set = raytracing_descriptor_set_layout->allocate(descriptor_pool->get());

        // raytracing pipeline with raygen, miss and closest-hit shader
        raytracing_pipeline = make_raytracing_pipeline(app.device);

        if (!raytracing_pipeline->add_shader(file_data("cubes/rgen.spv"), VK_SHADER_STAGE_RAYGEN_BIT_KHR))
            return false;
        if (!raytracing_pipeline->add_shader(file_data("cubes/rmiss.spv"), VK_SHADER_STAGE_MISS_BIT_KHR))
            return false;
        if (!raytracing_pipeline->add_shader(file_data("cubes/rchit.spv"), VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR))
            return false;
        if (!raytracing_pipeline->add_shader(file_data("cubes/rcall.spv"), VK_SHADER_STAGE_CALLABLE_BIT_KHR))
            return false;

        enum rt_stage : uint32_t {
            // this reflects the order they're added in above
            raygen = 0,
            miss,
            closest_hit,
            callable
        };

        // shader_binding_table expects the groups to be in this order
        raytracing_pipeline->add_shader_general_group(raygen);
        raytracing_pipeline->add_shader_general_group(miss);
        raytracing_pipeline->add_shader_hit_group(closest_hit);
        raytracing_pipeline->add_shader_general_group(callable);

        raytracing_pipeline->set_max_recursion_depth(1);
        raytracing_pipeline->set_layout(raytracing_pipeline_layout);

        if (!raytracing_pipeline->create())
            return false;

        // shader binding table

        // shaderRecordEXT buffer data for the callable shader
        // directional light vector for diffuse lighting
        struct callable_record_data {
            glm::vec3 direction = { 0.0f, 0.0f, 1.0f };
        } callable_record;

        std::vector records(raytracing_pipeline->get_shader_groups().size(), cdata(nullptr, 0));
        records[callable] = cdata(&callable_record, sizeof(callable_record));

        shader_binding = make_shader_binding_table();
        if (!shader_binding->create(raytracing_pipeline, records))
            return false;

        // ideally, these buffers would all be device-local (VMA_MEMORY_USAGE_GPU_ONLY) but to keep the demo code short they're host-visible to skip a staging buffer copy
        instance_buffer = make_buffer();
        if (!instance_buffer->create(app.device, instances.data(), sizeof(instance_data) * instances.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, VMA_MEMORY_USAGE_CPU_TO_GPU))
            return false;
        vertex_buffer = make_buffer();
        if (!vertex_buffer->create(app.device, vertices.data(), sizeof(vertex) * vertices.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, false, VMA_MEMORY_USAGE_CPU_TO_GPU))
            return false;
        index_buffer = make_buffer();
        if (!index_buffer->create(app.device, indices.data(), sizeof(index) * indices.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, false, VMA_MEMORY_USAGE_CPU_TO_GPU))
            return false;

        // create acceleration structures
        // - a BLAS (bottom level) for each mesh
        // - one TLAS (top level) referencing all the BLAS

        constexpr bool COMPACT_BLAS = true;

        top_as = make_top_level_acceleration_structure();

        // buffer data, common to all BLAS
        const VkAccelerationStructureGeometryTrianglesDataKHR triangles = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                                                            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                                                                            .vertexData = vertex_buffer->get_address(),
                                                                            .vertexStride = sizeof(vertex),
                                                                            .maxVertex = uint32_t(vertices.size()),
                                                                            .indexType = VK_INDEX_TYPE_UINT32,
                                                                            .indexData = index_buffer->get_address() };

        VkDeviceSize scratch_buffer_size = 0;

        for (size_t i = 0; i < instances.size(); i++) {
            const instance_data& instance = instances[i];
            // per-mesh sub-buffer region
            const VkAccelerationStructureBuildRangeInfoKHR range = {
                .primitiveCount = instance.index_count / 3,
                .primitiveOffset = instance.index_base * sizeof(index), // this is in bytes
                .firstVertex = instance.vertex_base // but this is an index...
            };

            bottom_level_acceleration_structure::ptr bottom_as = make_bottom_level_acceleration_structure();
            bottom_as->add_geometry(triangles, range, VK_GEOMETRY_OPAQUE_BIT_KHR);

            if (!bottom_as->create(app.device, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | (COMPACT_BLAS ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR : 0)))
                return false;
            bottom_as_list.push_back(bottom_as);
            scratch_buffer_size = std::max(scratch_buffer_size, bottom_as->scratch_buffer_size());

            top_as->add_instance(bottom_as);
        }

        if (!top_as->create(app.device, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR))
            return false;

        scratch_buffer_size = std::max(scratch_buffer_size, top_as->scratch_buffer_size());
        scratch_buffer = make_buffer();
        if (!scratch_buffer->create(app.device, nullptr, scratch_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR))
            return false;
        scratch_buffer_address = scratch_buffer->get_address();

        // build BLAS and TLAS

        one_time_command_buffer(app.device, pool, queue, [&](VkCommandBuffer cmd_buf) {
            // barrier to wait for build to finish
            const VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                              .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                                              .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR };
            const VkPipelineStageFlags src = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            const VkPipelineStageFlags dst = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;

            for (size_t i = 0; i < bottom_as_list.size(); i++) {
                bottom_as_list[i]->build(cmd_buf, scratch_buffer_address);
                app.device->call().vkCmdPipelineBarrier(cmd_buf, src, dst, 0, 1, &barrier, 0, 0, 0, 0);
            }
            top_as->build(cmd_buf, scratch_buffer_address);
            app.device->call().vkCmdPipelineBarrier(cmd_buf, src, dst | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 1, &barrier, 0, 0, 0, 0);
        });

        // compact BLAS
        // building must be finished to retrieve the compacted size, or vkGetQueryPoolResults will time out

        if (COMPACT_BLAS) {
            std::vector<bottom_level_acceleration_structure::ptr> compacted_bottom_as_list;

            one_time_command_buffer(app.device, pool, queue, [&](VkCommandBuffer cmd_buf) {
                const VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                                  .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                                                  .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR };
                const VkPipelineStageFlags src = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                const VkPipelineStageFlags dst = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;

                for (size_t i = 0; i < bottom_as_list.size(); i++) {
                    acceleration_structure::ptr compacted_bottom_as = bottom_as_list[i]->compact(cmd_buf);
                    compacted_bottom_as_list.push_back(std::dynamic_pointer_cast<bottom_level_acceleration_structure>(compacted_bottom_as));
                    // update the TLAS with references to the new compacted BLAS since their handles changed
                    top_as->update_instance(i, compacted_bottom_as_list[i]);
                }
                app.device->call().vkCmdPipelineBarrier(cmd_buf, src, dst, 0, 1, &barrier, 0, 0, 0, 0);
                top_as->update(cmd_buf, scratch_buffer_address);
                app.device->call().vkCmdPipelineBarrier(cmd_buf, src, dst | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 1, &barrier, 0, 0, 0, 0);
            });

            bottom_as_list = compacted_bottom_as_list;
        }

        // write descriptors

        VkDescriptorBufferInfo buffer_info = *uniform_buffer->get_descriptor_info();
        // for dynamic uniform buffers, range must be the bound size, not the total buffer size
        buffer_info.range = uniform_stride;

        const std::array<const VkWriteDescriptorSet, 5> write_sets = {
            VkWriteDescriptorSet{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = shared_descriptor_set,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                  .pBufferInfo = &buffer_info },

            VkWriteDescriptorSet{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .pNext = top_as->get_descriptor_info(),
                                  .dstSet = raytracing_descriptor_set,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR },

            VkWriteDescriptorSet{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = raytracing_descriptor_set,
                                  .dstBinding = 1,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = instance_buffer->get_descriptor_info() },

            VkWriteDescriptorSet{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = raytracing_descriptor_set,
                                  .dstBinding = 2,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = vertex_buffer->get_descriptor_info() },

            VkWriteDescriptorSet{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = raytracing_descriptor_set,
                                  .dstBinding = 3,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = index_buffer->get_descriptor_info() }
        };

        app.device->vkUpdateDescriptorSets(write_sets.size(), write_sets.data());

        glm::uvec2 size = app.target->get_size();

        uniforms.inv_view = glm::inverse(glm::lookAtLH(glm::vec3(0.75f, 0.25f, -1.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
        uniforms.inv_proj = glm::inverse(perspective_matrix(size, 90.0f, 5.0f));
        uniforms.viewport = { 0, 0, size };
        uniforms.background_color = { glm::convertSRGBToLinear(render_pass->get_clear_color()), 1.0f };
        uniforms.max_depth = 5;

        swapchain_callback.on_created({}, { { 0, 0 }, size });

        return true;
    };

    app.on_destroy = [&]() {
        swapchain_callback.on_destroyed();
        app.target->remove_callback(&swapchain_callback);

        blit_pipeline->destroy();
        blit_pipeline_layout->destroy();

        raytracing_pipeline->destroy();
        raytracing_pipeline_layout->destroy();

        descriptor_pool->destroy();

        shared_descriptor_set_layout->destroy();
        raytracing_descriptor_set_layout->destroy();

        instance_buffer->destroy();
        vertex_buffer->destroy();
        index_buffer->destroy();

        bottom_as_list.clear();
        top_as = nullptr;

        scratch_buffer->destroy();
        scratch_buffer_address = 0;

        uniform_buffer->destroy();

        app.device->vkDestroyCommandPool(pool);
    };

    app.on_update = [&](delta dt) {
        for (size_t i = 0; i < INSTANCE_COUNT; i++) {
            glm::vec3 pos = { (2.0f * i - 1) * 0.5f, 0.0f, i * 0.5f };
            float angle = glm::radians(15.0f) * float(to_sec(now())) * i;
            glm::mat4 transform = glm::translate(glm::mat4(1.0f), pos) * glm::rotate(glm::mat4(1.0f), angle, { 0.0f, 1.0f, 0.0 });
            top_as->set_instance_transform(i, transform);
        }

        return true;
    };

    // this is called before app.forward_shading (blit + gui) is processed

    app.on_process = [&](VkCommandBuffer cmd_buf, index frame) {
        const uint32_t uniform_offset = frame * uniform_stride;
        char* address = static_cast<char*>(uniform_buffer->get_mapped_data()) + uniform_offset;
        *reinterpret_cast<uniform_data*>(address) = uniforms;

        // rebuild TLAS with new transformation matrices

        const VkPipelineStageFlags build = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        const VkPipelineStageFlags use = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;

        // wait for the last trace
        app.device->call().vkCmdPipelineBarrier(cmd_buf, use, build, 0, 0, nullptr, 0, nullptr, 0, nullptr);

        top_as->update(cmd_buf, scratch_buffer_address);

        // wait for update to finish before the next trace
        const VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                          .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                                          .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR };
        app.device->call().vkCmdPipelineBarrier(cmd_buf, build, use, 0, 1, &barrier, 0, nullptr, 0, nullptr);

        // wait for previous image reads
        app.device->call().vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 0, nullptr);

        raytracing_pipeline->bind(cmd_buf);

        app.device->call().vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, raytracing_pipeline_layout->get(), 0, 1, &shared_descriptor_set, 1, &uniform_offset);
        app.device->call().vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, raytracing_pipeline_layout->get(), 1, 1, &raytracing_descriptor_set, 0, nullptr);

        // trace rays!

        const glm::uvec3 size = { uniforms.viewport.z, uniforms.viewport.w, 1 };

        const VkStridedDeviceAddressRegionKHR raygen = shader_binding->get_raygen_region();
        app.device->call().vkCmdTraceRaysKHR(
            cmd_buf,
            &raygen, &shader_binding->get_miss_region(), &shader_binding->get_hit_region(), &shader_binding->get_callable_region(),
            size.x, size.y, size.z);

        // wait for trace to finish before reading the image
        insert_image_memory_barrier(app.device, cmd_buf, output_image->get(), VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, output_image->get_subresource_range());
    };

    app.imgui.on_draw = [&]() {
        ImGui::SetNextWindowPos(ImVec2(30, 30), ImGuiCond_FirstUseEver);

        ImGui::Begin(app.get_name());

        ImGui::SetNextItemWidth(ImGui::GetWindowSize().x * 0.5f);
        ImGui::SliderInt("Max ray depth", (int*) &uniforms.max_depth, 1, 5);

        app.draw_about(true);

        ImGui::End();
    };

    return app.run();
}
