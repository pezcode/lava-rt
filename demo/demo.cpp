#include <demo.hpp>

lava::device::ptr create_raytracing_device(lava::device_manager& manager) {
    for (const lava::physical_device::ref physical_device : lava::instance::singleton().get_physical_devices()) {
        const VkPhysicalDeviceProperties& properties = physical_device.get_properties();
        if (properties.apiVersion < VK_API_VERSION_1_1)
            continue;

        lava::device::create_param device_params = physical_device.create_default_device_param();

        device_params.extensions.push_back(VK_NV_RAY_TRACING_EXTENSION_NAME);
        // allow indexing using non-uniform values (ie. can diverge between shader invocations)
        // promoted to core 1.2
        device_params.extensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        // new layout for tightly-packed buffers (always uses alignment of base type)
        device_params.extensions.push_back(VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);

        // allow indexing into sampler arrays with non compile-time constants
        device_params.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;

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

        VkPhysicalDeviceDescriptorIndexingFeaturesEXT features_descriptor_indexing = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT
        };
        // allow indexing into sampler arrays with non uniform values
        //features_descriptor_indexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        // allow unbounded runtime descriptor arrays in shader (but fixed at layout creation)
        //features_descriptor_indexing.runtimeDescriptorArray = VK_TRUE;
        // allow only updating a subset of the max count in the layout
        //features_descriptor_indexing.descriptorBindingPartiallyBound = VK_TRUE;
        // allow variable descriptor array size for different sets
        //features_descriptor_indexing.descriptorBindingVariableDescriptorCount = VK_TRUE;

        VkPhysicalDeviceScalarBlockLayoutFeaturesEXT features_scalar_block_layout = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES
        };
        features_scalar_block_layout.scalarBlockLayout = VK_TRUE;

        device_params.next = &features_descriptor_indexing;
        features_descriptor_indexing.pNext = &features_scalar_block_layout;

        lava::device::ptr device = manager.create(device_params);
        if (!device)
            continue;

        // the command buffer used for vkCmdBuildAccelerationStructureNV and vkCmdTraceRaysNV must support compute
        // we use the graphics queue everywhere for convenience, make sure it supports both graphics and compute
        // the Vulkan specs guarantee that a queue family exists with both if graphics operations are supported
        // TODO
        // use semaphore to synchronize
        // deal with this properly with queue transitions (is this really needed? the image shouldn't be exclusive)
        // or send a PR so lava always selects that queue by default
        if (!(physical_device.get_queue_family_properties()[device->get_graphics_queue().family]
                  .queueFlags
              & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
            device->destroy();
            continue;
        }

        lava::log()->info("using device: {} ({})", physical_device.get_properties().deviceName,
                          physical_device.get_device_type_string());
        return device;
    }

    lava::log()->error("no compatible device found");

    return nullptr;
}

std::optional<VkFormat> get_supported_format(lava::device_ptr device, const lava::VkFormats& possible_formats, VkImageUsageFlags usage) {
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

bool one_time_command_buffer(lava::device_ptr device, VkCommandPool pool, lava::device::queue::ref queue, std::function<void(VkCommandBuffer)> callback) {
    VkCommandBuffer cmd_buf = VK_NULL_HANDLE;
    if (!device->vkAllocateCommandBuffers(
            pool, 1, &cmd_buf, VK_COMMAND_BUFFER_LEVEL_PRIMARY))
        return false;
    const VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (lava::failed(device->call().vkBeginCommandBuffer(cmd_buf, &begin_info)))
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
