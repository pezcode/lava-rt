#include <demo.hpp>

using namespace lava;

// create allocator with custom VMA flags
// we need VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT to take buffer device addresses
allocator::ptr create_custom_allocator(device_ptr device, VmaAllocatorCreateFlags flags) {
    const VmaVulkanFunctions vulkan_function = {
        .vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties,
        .vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties,
        .vkAllocateMemory = vkAllocateMemory,
        .vkFreeMemory = vkFreeMemory,
        .vkMapMemory = vkMapMemory,
        .vkUnmapMemory = vkUnmapMemory,
        .vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges,
        .vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges,
        .vkBindBufferMemory = vkBindBufferMemory,
        .vkBindImageMemory = vkBindImageMemory,
        .vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements,
        .vkGetImageMemoryRequirements = vkGetImageMemoryRequirements,
        .vkCreateBuffer = vkCreateBuffer,
        .vkDestroyBuffer = vkDestroyBuffer,
        .vkCreateImage = vkCreateImage,
        .vkDestroyImage = vkDestroyImage,
        .vkCmdCopyBuffer = vkCmdCopyBuffer,
#if VMA_DEDICATED_ALLOCATION
        .vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2KHR,
        .vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2KHR,
#endif
#if VMA_BIND_MEMORY2
        .vkBindBufferMemory2KHR = vkBindBufferMemory2KHR,
        .vkBindImageMemory2KHR = vkBindImageMemory2KHR,
#endif
#if VMA_MEMORY_BUDGET
        .vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2KHR
#endif
    };

    const VmaAllocatorCreateInfo allocator_info = {
        .flags = flags,
        .physicalDevice = device->get_vk_physical_device(),
        .device = device->get(),
        .pAllocationCallbacks = memory::alloc(),
        .pVulkanFunctions = &vulkan_function,
        .instance = instance::get(),
    };

    VmaAllocator vma_allocator = VK_NULL_HANDLE;
    if (!check(vmaCreateAllocator(&allocator_info, &vma_allocator)))
        return nullptr;

    return std::make_shared<allocator>(vma_allocator);
}

device::ptr create_raytracing_device(device_manager& manager) {
    for (physical_device::ref physical_device : instance::singleton().get_physical_devices()) {
        const VkPhysicalDeviceProperties& properties = physical_device.get_properties();
        if (properties.apiVersion < VK_API_VERSION_1_1)
            continue;

        device::create_param device_params = physical_device.create_default_device_param();

        // https://www.khronos.org/blog/vulkan-ray-tracing-final-specification-release

        device_params.extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        // next 3 required by VK_KHR_acceleration_structure
        device_params.extensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        device_params.extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        // allow indexing using non-uniform values (ie. can diverge between shader invocations)
        device_params.extensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);

        device_params.extensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        // required by VK_KHR_ray_tracing_pipeline
        device_params.extensions.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);

        // can't test this, needs an RTX GPU :<
        //device_params.extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);

        // required by VK_KHR_ray_tracing_pipeline and VK_KHR_ray_query
        device_params.extensions.push_back(VK_KHR_SPIRV_1_4_EXTENSION_NAME);
        // required by VK_KHR_spirv_1_4
        device_params.extensions.push_back(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);

        // new layout for tightly-packed buffers (always uses alignment of base type)
        device_params.extensions.push_back(VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);

#ifdef LIBLAVA_DEBUG
        // bounds-check against buffer ranges
        device_params.features.robustBufferAccess = VK_TRUE;
        // required for GPU-assisted validation
        // this needs to be enabled with vk_layer_settings.txt in the working directory
        // can't check config.debug.validation because that gets overwritten in app.setup() during debug builds
        // but we need it earlier to create the device
        device_params.features.fragmentStoresAndAtomics = VK_TRUE;
        device_params.features.vertexPipelineStoresAndAtomics = VK_TRUE;
#endif

        VkPhysicalDeviceAccelerationStructureFeaturesKHR features_acceleration_structure = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR
        };
        features_acceleration_structure.accelerationStructure = VK_TRUE;
        features_acceleration_structure.descriptorBindingAccelerationStructureUpdateAfterBind = VK_TRUE;

        VkPhysicalDeviceBufferDeviceAddressFeaturesKHR features_buffer_device_address = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR
        };
        features_buffer_device_address.bufferDeviceAddress = VK_TRUE;

        VkPhysicalDeviceDescriptorIndexingFeaturesEXT features_descriptor_indexing = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT
        };
        // VK_KHR_acceleration_structure requires the equivalent of the descriptorIndexing feature
        // https://vulkan.lunarg.com/doc/view/1.2.162.0/windows/1.2-extensions/vkspec.html#features-descriptorIndexing
        // allow indexing into sampler arrays with non compile-time constants
        device_params.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
        device_params.features.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
        features_descriptor_indexing.shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE;
        features_descriptor_indexing.shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE;
        // allow indexing into sampler arrays with non uniform values
        features_descriptor_indexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        features_descriptor_indexing.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        features_descriptor_indexing.shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
        features_descriptor_indexing.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        features_descriptor_indexing.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
        features_descriptor_indexing.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        features_descriptor_indexing.descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
        features_descriptor_indexing.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
        features_descriptor_indexing.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        // allow only updating a subset of the max count in the layout
        features_descriptor_indexing.descriptorBindingPartiallyBound = VK_TRUE;
        // allow unbounded runtime descriptor arrays in shader (but fixed at layout creation)
        features_descriptor_indexing.runtimeDescriptorArray = VK_TRUE;

        VkPhysicalDeviceRayTracingPipelineFeaturesKHR features_ray_tracing_pipeline = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR
        };
        features_ray_tracing_pipeline.rayTracingPipeline = VK_TRUE;
        features_ray_tracing_pipeline.rayTracingPipelineTraceRaysIndirect = VK_TRUE;

        //VkPhysicalDeviceRayQueryFeaturesKHR features_ray_query = {
        //    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR
        //};
        //features_ray_query.rayQuery = VK_TRUE;

        VkPhysicalDeviceScalarBlockLayoutFeaturesEXT features_scalar_block_layout = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES
        };
        features_scalar_block_layout.scalarBlockLayout = VK_TRUE;

        device_params.next = &features_acceleration_structure;
        features_acceleration_structure.pNext = &features_buffer_device_address;
        features_buffer_device_address.pNext = &features_descriptor_indexing;
        features_descriptor_indexing.pNext = &features_ray_tracing_pipeline;
        //features_ray_tracing_pipeline.pNext = &features_ray_query;
        //features_ray_query.pNext = &features_scalar_block_layout;
        features_ray_tracing_pipeline.pNext = &features_scalar_block_layout;

        device::ptr device = manager.create(device_params);
        if (!device)
            continue;

        // the command buffer used for vkCmdBuildAccelerationStructureKHR and vkCmdTraceRaysKHR must support compute
        // we use the graphics queue everywhere for convenience, make sure it supports both graphics and compute
        // the Vulkan specs guarantee that a queue family exists with both if graphics operations are supported
        // TODO
        // use semaphore to synchronize
        // deal with this properly with queue transitions (are images actually exclusive?)
        // or send a PR so lava always selects that queue by default
        if (!(physical_device.get_queue_family_properties()[device->get_graphics_queue().family]
                  .queueFlags
              & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
            device->destroy();
            continue;
        }

        log()->info("using device: {} ({})", physical_device.get_properties().deviceName,
                    physical_device.get_device_type_string());

        device->set_allocator(create_custom_allocator(device.get(), VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT));

        return device;
    }

    log()->error("no compatible device found");

    return nullptr;
}

std::optional<VkFormat> get_supported_format(device_ptr device, const VkFormats& possible_formats, VkImageUsageFlags usage) {
    VkFormatFeatureFlags features = 0;
    if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        features |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        features |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT)
        features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT)
        features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
        features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;

    for (auto& format : possible_formats) {
        VkFormatProperties format_props;
        vkGetPhysicalDeviceFormatProperties(device->get_vk_physical_device(), format, &format_props);

        if ((format_props.optimalTilingFeatures & features) == features) {
            return { format };
        }
    }

    return std::nullopt;
}

bool one_time_command_buffer(device_ptr device, VkCommandPool pool, device::queue::ref queue, std::function<void(VkCommandBuffer)> callback) {
    VkCommandBuffer cmd_buf = VK_NULL_HANDLE;
    if (!device->vkAllocateCommandBuffers(
            pool, 1, &cmd_buf, VK_COMMAND_BUFFER_LEVEL_PRIMARY))
        return false;
    const VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!check(device->call().vkBeginCommandBuffer(cmd_buf, &begin_info)))
        return false;

    callback(cmd_buf);

    device->call().vkEndCommandBuffer(cmd_buf);

    VkFence fence = VK_NULL_HANDLE;
    const VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (!device->vkCreateFence(&fence_info, &fence))
        return false;

    const VkSubmitInfo submit_info = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                       .commandBufferCount = 1,
                                       .pCommandBuffers = &cmd_buf };
    if (!device->vkQueueSubmit(queue.vk_queue, 1, &submit_info, fence)) {
        device->vkDestroyFence(fence);
        return false;
    }

    device->vkWaitForFences(1, &fence, VK_TRUE, ~0);
    device->vkDestroyFence(fence);

    device->vkFreeCommandBuffers(pool, 1, &cmd_buf);

    return true;
}

glm::mat4 perspective_matrix(uv2 size, float fov, float far_plane) {
    // Vulkan NDC is right-handed with Y pointing down
    // we flip Y which makes it left-handed
    return glm::scale(glm::identity<glm::mat4>(), { 1.0f, -1.0f, 1.0f }) *
        glm::perspectiveLH_ZO(glm::radians(fov), float(size.x) / size.y, 0.1f, far_plane);
}
