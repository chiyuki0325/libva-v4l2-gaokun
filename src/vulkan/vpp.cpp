// SPDX-License-Identifier: MIT
#include "../internal.hpp"
#include "vpp.hpp"
#include <vulkan/vulkan.h>
#include "tonemap_spirv.h"
#include <array>
#include <cmath>
#include <cerrno>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace irisva {
namespace {
void vk_check(VkResult result, const char *operation) {
    if (result != VK_SUCCESS) {
        trace("Vulkan %s failed: %d", operation, result);
        throw Error(VA_STATUS_ERROR_OPERATION_FAILED, operation);
    }
}
struct Parameters {
    uint32_t width, height, src_stride, src_y, src_uv, dst_stride, dst_y, dst_uv;
    float source_pq, destination, knee;
};
double encode_pq(double nits) {
    double power = std::pow(nits / 10000.0, 0.1593017578125);
    return std::pow((0.8359375 + 18.8515625 * power) / (1.0 + 18.6875 * power), 78.84375);
}
void validate_layout(const Memory &memory, const Surface &surface) {
    unsigned row_bytes = surface.width * (surface.fourcc == VA_FOURCC_P010 ? 2 : 1);
    check(memory.fd >= 0 && memory.fourcc == surface.fourcc && memory.width >= surface.width &&
              memory.height >= surface.height && memory.stride >= row_bytes &&
              memory.storage_height >= surface.height && !(memory.stride & 1) &&
              !(memory.data_offset & 1) && memory.size <= UINT32_MAX &&
              uint64_t(memory.data_offset) +
                      uint64_t(memory.stride) *
                          (uint64_t(memory.storage_height) + surface.height / 2 - 1) +
                      row_bytes <=
                  memory.size,
          "unsupported Vulkan DMA-BUF layout", VA_STATUS_ERROR_INVALID_SURFACE);
}
void publish_dma_buf(const Memory &memory) {
    // Extra exporter synchronization fixes GPU-to-CDSP visibility on the
    // tested SC8280XP stack. These CPU-access hooks neither map nor copy
    // pixels; this is a workaround, not a generic device-cache flush.
    for (uint64_t phase : {DMA_BUF_SYNC_START, DMA_BUF_SYNC_END}) {
        dma_buf_sync sync{phase | DMA_BUF_SYNC_RW};
        int result;
        do {
            result = ioctl(memory.fd, DMA_BUF_IOCTL_SYNC, &sync);
        } while (result < 0 && errno == EINTR);
        check(result == 0, "publish Vulkan DMA-BUF writes");
    }
}
} // namespace

struct VulkanVpp::Impl : std::enable_shared_from_this<Impl> {
    // Imported buffers retain the device, even after the VPP context is destroyed.
    // Memory releases its registration before closing the original DMA-BUF fd.
    struct Import {
        std::shared_ptr<Impl> owner;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        ~Import() {
            vkDestroyBuffer(owner->device, buffer, nullptr);
            vkFreeMemory(owner->device, memory, nullptr);
        }
    };
    std::mutex mutex;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    uint32_t max_buffer_range = 0;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    PFN_vkGetMemoryFdPropertiesKHR get_fd_properties = nullptr;

    ~Impl() {
        if (device) {
            vkDestroyFence(device, fence, nullptr);
            vkDestroyCommandPool(device, command_pool, nullptr);
            vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            vkDestroyPipeline(device, pipeline, nullptr);
            vkDestroyShaderModule(device, shader, nullptr);
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        vkDestroyInstance(instance, nullptr);
    }
    void initialize(dev_t render_device) {
        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "libva-v4l2";
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instance_info{};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pApplicationInfo = &application;
        vk_check(vkCreateInstance(&instance_info, nullptr, &instance), "create instance");
        uint32_t count = 0;
        vk_check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices");
        std::vector<VkPhysicalDevice> devices(count);
        vk_check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate devices");
        const char *extensions[] = {
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME};
        for (auto candidate : devices) {
            uint32_t extension_count = 0;
            vk_check(
                vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, nullptr),
                "enumerate extensions");
            std::vector<VkExtensionProperties> available(extension_count);
            vk_check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count,
                                                          available.data()),
                     "enumerate extensions");
            bool supported = true;
            for (auto name : extensions)
                supported &= std::any_of(available.begin(), available.end(), [&](const auto &e) {
                    return std::strcmp(name, e.extensionName) == 0;
                });
            if (!supported)
                continue;
            VkPhysicalDeviceDrmPropertiesEXT drm{};
            drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 properties{};
            properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties.pNext = &drm;
            vkGetPhysicalDeviceProperties2(candidate, &properties);
            bool matches_render = drm.hasRender && drm.renderMajor == major(render_device) &&
                                  drm.renderMinor == minor(render_device);
            bool matches_primary = drm.hasPrimary && drm.primaryMajor == major(render_device) &&
                                   drm.primaryMinor == minor(render_device);
            if ((!matches_render && !matches_primary) ||
                properties.properties.apiVersion < VK_API_VERSION_1_1)
                continue;
            VkPhysicalDevice16BitStorageFeatures storage{};
            storage.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
            VkPhysicalDeviceFeatures2 features{};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &storage;
            vkGetPhysicalDeviceFeatures2(candidate, &features);
            if (!storage.storageBuffer16BitAccess)
                continue;
            VkPhysicalDeviceExternalBufferInfo external{};
            external.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
            external.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            VkExternalBufferProperties external_properties{};
            external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
            vkGetPhysicalDeviceExternalBufferProperties(candidate, &external, &external_properties);
            if (!(external_properties.externalMemoryProperties.externalMemoryFeatures &
                  VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
                continue;
            uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
            for (uint32_t i = 0; i < family_count; ++i) {
                if (!(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
                    continue;
                physical = candidate;
                family = i;
                max_buffer_range = properties.properties.limits.maxStorageBufferRange;
                trace("Vulkan VPP: %s", properties.properties.deviceName);
                break;
            }
            if (physical)
                break;
        }
        check(physical, "Vulkan DMA-BUF compute unavailable", VA_STATUS_ERROR_UNSUPPORTED_FILTER);
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkPhysicalDevice16BitStorageFeatures storage{};
        storage.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
        storage.storageBuffer16BitAccess = VK_TRUE;
        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.pNext = &storage;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = std::size(extensions);
        device_info.ppEnabledExtensionNames = extensions;
        vk_check(vkCreateDevice(physical, &device_info, nullptr, &device), "create device");
        vkGetDeviceQueue(device, family, 0, &queue);
        get_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
            vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
        check(get_fd_properties, "Vulkan external memory entrypoint unavailable");
        VkDescriptorSetLayoutBinding bindings[2]{};
        for (unsigned i = 0; i < 2; ++i)
            bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                           nullptr};
        VkDescriptorSetLayoutCreateInfo set_info{};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_info.bindingCount = 2;
        set_info.pBindings = bindings;
        vk_check(vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout),
                 "create descriptor layout");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters)};
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &set_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        vk_check(vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout),
                 "create pipeline layout");
        VkShaderModuleCreateInfo shader_info{};
        shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_info.codeSize = sizeof(tonemap_spirv);
        shader_info.pCode = tonemap_spirv;
        vk_check(vkCreateShaderModule(device, &shader_info, nullptr, &shader), "create shader");
        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = shader;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = pipeline_layout;
        vk_check(
            vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline),
            "create pipeline");
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        vk_check(vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool),
                 "create descriptor pool");
        VkDescriptorSetAllocateInfo set_allocate{};
        set_allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        set_allocate.descriptorPool = descriptor_pool;
        set_allocate.descriptorSetCount = 1;
        set_allocate.pSetLayouts = &set_layout;
        vk_check(vkAllocateDescriptorSets(device, &set_allocate, &descriptor_set),
                 "allocate descriptors");
        VkCommandPoolCreateInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        command_info.queueFamilyIndex = family;
        vk_check(vkCreateCommandPool(device, &command_info, nullptr, &command_pool),
                 "create command pool");
        VkCommandBufferAllocateInfo command_allocate{};
        command_allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_allocate.commandPool = command_pool;
        command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_allocate.commandBufferCount = 1;
        vk_check(vkAllocateCommandBuffers(device, &command_allocate, &commands),
                 "allocate command buffer");
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vk_check(vkCreateFence(device, &fence_info, nullptr, &fence), "create fence");
    }
    std::shared_ptr<Import> import(Memory &memory) {
        if (memory.vulkan_registration) {
            auto imported = std::static_pointer_cast<Import>(memory.vulkan_registration);
            check(imported->owner.get() == this, "DMA-BUF belongs to another Vulkan device");
            return imported;
        }
        check(memory.size <= max_buffer_range, "Vulkan storage buffer exceeds device limit",
              VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
        auto imported = std::make_shared<Import>();
        imported->owner = shared_from_this();
        VkExternalMemoryBufferCreateInfo external{};
        external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.pNext = &external;
        buffer_info.size = memory.size;
        buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        vk_check(vkCreateBuffer(device, &buffer_info, nullptr, &imported->buffer),
                 "create external buffer");
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, imported->buffer, &requirements);
        check(requirements.size <= memory.size, "DMA-BUF too small for Vulkan alignment",
              VA_STATUS_ERROR_INVALID_SURFACE);
        VkMemoryFdPropertiesKHR properties{};
        properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
        vk_check(get_fd_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                   memory.fd, &properties),
                 "query DMA-BUF memory");
        uint32_t types = properties.memoryTypeBits & requirements.memoryTypeBits;
        check(types, "no compatible Vulkan DMA-BUF memory type", VA_STATUS_ERROR_INVALID_SURFACE);
        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.buffer = imported->buffer;
        VkImportMemoryFdInfoKHR fd_info{};
        fd_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        fd_info.pNext = &dedicated;
        fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        fd_info.fd = dup(memory.fd);
        check(fd_info.fd >= 0, "duplicate Vulkan DMA-BUF fd", VA_STATUS_ERROR_ALLOCATION_FAILED);
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.pNext = &fd_info;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = __builtin_ctz(types);
        VkResult result = vkAllocateMemory(device, &allocation, nullptr, &imported->memory);
        // Vulkan takes ownership of the duplicated fd only on successful import.
        if (result != VK_SUCCESS)
            close(fd_info.fd);
        vk_check(result, "import DMA-BUF");
        vk_check(vkBindBufferMemory(device, imported->buffer, imported->memory, 0), "bind DMA-BUF");
        memory.vulkan_registration = imported;
        return imported;
    }
};

VulkanVpp::VulkanVpp(int render_fd) {
    struct stat info{};
    check(fstat(render_fd, &info) == 0 && S_ISCHR(info.st_mode), "invalid Vulkan render device");
    static std::mutex cache_mutex;
    static std::map<dev_t, std::weak_ptr<Impl>> cache;
    std::lock_guard<std::mutex> guard(cache_mutex);
    impl_ = cache[info.st_rdev].lock();
    if (!impl_) {
        impl_ = std::make_shared<Impl>();
        impl_->initialize(info.st_rdev);
        cache[info.st_rdev] = impl_;
    }
}
void VulkanVpp::process(const VppPicture &picture, Surface &target) {
    auto &src = *picture.source->memory;
    auto &dst = *target.memory;
    validate_layout(src, *picture.source);
    validate_layout(dst, target);
    auto &gpu = *impl_;
    std::lock_guard<std::mutex> guard(gpu.mutex);
    auto input = gpu.import(src), output = gpu.import(dst);
    wait_surface_access(src, POLLIN);
    wait_surface_access(dst, POLLOUT);
    VkDescriptorBufferInfo buffers[2] = {{input->buffer, 0, src.size},
                                         {output->buffer, 0, dst.size}};
    VkWriteDescriptorSet writes[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = gpu.descriptor_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffers[i];
    }
    vkUpdateDescriptorSets(gpu.device, 2, writes, 0, nullptr);
    auto crop = picture.region;
    double source_pq = encode_pq(picture.source_peak);
    trace("Vulkan tone mapping %ux%u peak=%g src=%u/%u/%u dst=%u/%u/%u", crop.width, crop.height,
          picture.source_peak, src.stride, src.data_offset, src.storage_height, dst.stride,
          dst.data_offset, dst.storage_height);
    float destination = encode_pq(100.0) / source_pq;
    Parameters params{crop.width,
                      crop.height,
                      src.stride,
                      src.data_offset + unsigned(crop.y) * src.stride + unsigned(crop.x) * 2,
                      src.data_offset + src.storage_height * src.stride +
                          unsigned(crop.y / 2) * src.stride + unsigned(crop.x) * 2,
                      dst.stride,
                      dst.data_offset,
                      dst.data_offset + dst.storage_height * dst.stride,
                      float(source_pq),
                      destination,
                      1.5f * destination - 0.5f};
    vk_check(vkResetCommandPool(gpu.device, gpu.command_pool, 0), "reset command pool");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(gpu.commands, &begin), "begin command buffer");
    // DMA-BUF producers/consumers are outside this Vulkan queue family. Import
    // alone does not acquire ownership or replace their implicit fences.
    VkBufferMemoryBarrier barriers[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[i].dstAccessMask = i ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        barriers[i].dstQueueFamilyIndex = gpu.family;
        barriers[i].buffer = buffers[i].buffer;
        barriers[i].size = VK_WHOLE_SIZE;
    }
    vkCmdPipelineBarrier(gpu.commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2, barriers, 0,
                         nullptr);
    vkCmdBindPipeline(gpu.commands, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.pipeline);
    vkCmdBindDescriptorSets(gpu.commands, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.pipeline_layout, 0, 1,
                            &gpu.descriptor_set, 0, nullptr);
    vkCmdPushConstants(gpu.commands, gpu.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(params), &params);
    vkCmdDispatch(gpu.commands, (crop.width + 255) / 256, crop.height / 2, 1);
    for (auto &barrier : barriers) {
        barrier.srcAccessMask = barrier.dstAccessMask;
        barrier.dstAccessMask = 0;
        barrier.srcQueueFamilyIndex = gpu.family;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    }
    vkCmdPipelineBarrier(gpu.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 2, barriers, 0,
                         nullptr);
    vk_check(vkEndCommandBuffer(gpu.commands), "end command buffer");
    vk_check(vkResetFences(gpu.device, 1, &gpu.fence), "reset fence");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &gpu.commands;
    vk_check(vkQueueSubmit(gpu.queue, 1, &submit, gpu.fence), "submit tone mapping");
    // Complete before returning buffers to CDSP, V4L2, or the VA caller. No CPU
    // mapping or pixel copy is involved; imports persist for the Memory lifetime.
    vk_check(vkWaitForFences(gpu.device, 1, &gpu.fence, VK_TRUE, UINT64_MAX),
             "wait for tone mapping");
    publish_dma_buf(dst);
}
} // namespace irisva
