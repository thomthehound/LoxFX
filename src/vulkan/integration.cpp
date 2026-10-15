// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#include "loxfx/vulkan/integration.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace loxfx::vulkan {
namespace {

void vk_check(VkResult result, const char* action) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(action) + " (VkResult " +
                                 std::to_string(result) + ")");
}

struct PushParameters {
    std::uint32_t vertices_per_strand;
    std::uint32_t followers_per_guide;
    std::uint32_t padded_guides;
    std::uint32_t reserved = 0;
    float time_step;
    float gravity;
    float damping;
    float global_stiffness;
    float global_range;
    float reserved1 = 0, reserved2 = 0, reserved3 = 0;
};
static_assert(sizeof(PushParameters) == 48);

std::vector<std::uint32_t> load_spirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open SPIR-V: " + path.string());
    const std::streamsize size = file.tellg();
    if (size <= 0 || size % 4 != 0 || size > 8 * 1024 * 1024)
        throw std::runtime_error("invalid SPIR-V byte length");
    std::vector<std::uint32_t> words(static_cast<std::size_t>(size) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(words.data()), size))
        throw std::runtime_error("cannot read SPIR-V");
    if (words[0] != 0x07230203u)
        throw std::runtime_error("invalid SPIR-V magic");
    return words;
}

void valid_parameters(const IntegrationParameters& p) {
    if (!(std::isfinite(p.time_step) && p.time_step > 0 && p.time_step <= 0.1f &&
          std::isfinite(p.gravity) && std::isfinite(p.damping) && p.damping >= 0 &&
          std::isfinite(p.global_stiffness) && p.global_stiffness >= 0 &&
          p.global_stiffness <= 1.0f && std::isfinite(p.global_range) &&
          p.global_range >= 0 && p.global_range <= 1))
        throw std::invalid_argument("invalid integration parameters");
}

} // namespace

struct IntegrationRuntime::Impl {
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize bytes = 0;
    };

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    std::string device_name;
    bool software = false;
    float timestamp_period = 0.0f;
    bool timestamp_supported = false;
    std::uint32_t timestamp_valid_bits = 0;
    double last_gpu_ms = -1.0;

    Buffer current, previous, rest, staging;
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    VkDeviceSize vertex_bytes = 0;
    std::uint32_t groups = 0, vertices = 0, followers = 0, guides = 0;

    ~Impl() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (queries) vkDestroyQueryPool(device, queries, nullptr);
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            if (descriptor_layout) vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
            for (const Buffer* b : {&current, &previous, &rest, &staging}) {
                if (b->buffer) vkDestroyBuffer(device, b->buffer, nullptr);
                if (b->memory) vkFreeMemory(device, b->memory, nullptr);
            }
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    std::uint32_t memory_index(std::uint32_t bits, VkMemoryPropertyFlags requested) const {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical, &properties);
        for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((bits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & requested) == requested)
                return i;
        throw std::runtime_error("required Vulkan buffer memory type unavailable");
    }

    void allocate(Buffer& result, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags memory_flags) {
        result.bytes = size;
        VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        create.size = size;
        create.usage = usage;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vk_check(vkCreateBuffer(device, &create, nullptr, &result.buffer), "create buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = memory_index(requirements.memoryTypeBits, memory_flags);
        vk_check(vkAllocateMemory(device, &alloc, nullptr, &result.memory), "allocate buffer");
        vk_check(vkBindBufferMemory(device, result.buffer, result.memory, 0), "bind buffer");
    }

    void submit_and_wait() {
        vk_check(vkEndCommandBuffer(commands), "end command buffer");
        vk_check(vkResetFences(device, 1, &fence), "reset fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        vk_check(vkQueueSubmit(queue, 1, &submit, fence), "submit compute work");
        vk_check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX),
                 "wait for compute work");
    }

    void begin_commands() {
        vk_check(vkResetCommandBuffer(commands, 0), "reset command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(vkBeginCommandBuffer(commands, &begin), "begin command buffer");
    }

    void barrier(VkPipelineStageFlags source_stage, VkAccessFlags source_access,
                 VkPipelineStageFlags target_stage, VkAccessFlags target_access) {
        VkMemoryBarrier barrier_info{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier_info.srcAccessMask = source_access;
        barrier_info.dstAccessMask = target_access;
        vkCmdPipelineBarrier(commands, source_stage, target_stage, 0, 1,
                             &barrier_info, 0, nullptr, 0, nullptr);
    }

    void create_device() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "LoxFX stage-one smoke";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        vk_check(vkCreateInstance(&ci, nullptr, &instance), "create Vulkan instance");

        std::uint32_t count = 0;
        vk_check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices");
        if (!count) throw std::runtime_error("no Vulkan physical device found");
        std::vector<VkPhysicalDevice> devices(count);
        vk_check(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
                 "enumerate physical devices");
        // Never silently prefer a CPU Vulkan adapter over a real GPU.
        // This policy is queried ONCE during initialization.
        for (int tier = 0; tier < 2 && !physical; ++tier) {
            for (VkPhysicalDevice candidate : devices) {
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(candidate, &props);
                if (props.apiVersion < VK_API_VERSION_1_2) continue;
                const bool cpu = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
                if (cpu != (tier == 1)) continue;
                std::uint32_t families = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
                std::vector<VkQueueFamilyProperties> queue_props(families);
                vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, queue_props.data());
                for (std::uint32_t i = 0; i < families; ++i) {
                    if (queue_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                        physical = candidate;
                        family = i;
                        device_name = props.deviceName;
                        software = cpu;
                        timestamp_valid_bits = queue_props[i].timestampValidBits;
                        timestamp_supported = timestamp_valid_bits != 0;
                        timestamp_period = props.limits.timestampPeriod;
                        break;
                    }
                }
                if (physical) break;
            }
        }
        if (!physical) throw std::runtime_error("no Vulkan 1.2+ compute queue");
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_create{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_create.queueFamilyIndex = family;
        queue_create.queueCount = 1;
        queue_create.pQueuePriorities = &priority;
        VkDeviceCreateInfo device_create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_create.queueCreateInfoCount = 1;
        device_create.pQueueCreateInfos = &queue_create;
        vk_check(vkCreateDevice(physical, &device_create, nullptr, &device),
                 "create Vulkan device");
        vkGetDeviceQueue(device, family, 0, &queue);
    }

    void init(const PreparedGroom& groom, const std::filesystem::path& spirv_path) {
        if (!groom.work.packed_total_vertices ||
            groom.rest_positions.size() != groom.work.packed_total_vertices ||
            groom.vertices_per_strand == 0 ||
            groom.work.packed_guide_strands == 0)
            throw std::invalid_argument("prepared groom has inconsistent geometry");
        if (groom.work.stages[0].stage != SimulationStage::integration_and_global_shape ||
            !groom.work.stages[0].thread_groups)
            throw std::invalid_argument("invalid first-stage dispatch");
        vertices = groom.vertices_per_strand;
        followers = groom.followers_per_guide;
        guides = groom.work.packed_guide_strands;
        groups = groom.work.stages[0].thread_groups;
        vertex_bytes = static_cast<VkDeviceSize>(groom.rest_positions.size()) * sizeof(Float4);
        if (vertex_bytes > std::numeric_limits<VkDeviceSize>::max() / 3)
            throw std::invalid_argument("prepared groom exceeds staging capacity");

        create_device();
        VkPhysicalDeviceProperties limits{};
        vkGetPhysicalDeviceProperties(physical, &limits);
        if (vertex_bytes > limits.limits.maxStorageBufferRange)
            throw std::runtime_error(
                "groom exceeds selected device maxStorageBufferRange; "
                "partitioning oversized grooms is not yet implemented");
        allocate(current, vertex_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        allocate(previous, vertex_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        allocate(rest, vertex_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        allocate(staging, vertex_bytes * 3, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        for (std::uint32_t i = 0; i < 3; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout.pBindings = bindings.data();
        vk_check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &descriptor_layout),
                 "create descriptor layout");
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &pool_size;
        vk_check(vkCreateDescriptorPool(device, &pool, nullptr, &descriptor_pool),
                 "create descriptor pool");
        VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sets.descriptorPool = descriptor_pool;
        sets.descriptorSetCount = 1;
        sets.pSetLayouts = &descriptor_layout;
        vk_check(vkAllocateDescriptorSets(device, &sets, &descriptor_set),
                 "allocate descriptor set");

        std::array<VkDescriptorBufferInfo, 3> buffer_infos{{
            {current.buffer, 0, vertex_bytes},
            {previous.buffer, 0, vertex_bytes},
            {rest.buffer, 0, vertex_bytes}
        }};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (std::uint32_t i = 0; i < 3; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = descriptor_set;
            writes[i].dstBinding = i;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
            writes[i].pBufferInfo = &buffer_infos[i];
        }
        vkUpdateDescriptorSets(device, 3, writes.data(), 0, nullptr);

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size = sizeof(PushParameters);
        VkPipelineLayoutCreateInfo pipe_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipe_layout.setLayoutCount = 1;
        pipe_layout.pSetLayouts = &descriptor_layout;
        pipe_layout.pushConstantRangeCount = 1;
        pipe_layout.pPushConstantRanges = &push;
        vk_check(vkCreatePipelineLayout(device, &pipe_layout, nullptr, &pipeline_layout),
                 "create pipeline layout");

        const auto words = load_spirv(spirv_path);
        VkShaderModuleCreateInfo module_create{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_create.codeSize = words.size() * sizeof(std::uint32_t);
        module_create.pCode = words.data();
        VkShaderModule module = VK_NULL_HANDLE;
        vk_check(vkCreateShaderModule(device, &module_create, nullptr, &module),
                 "create integration shader module");
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "IntegrationAndGlobalShapeConstraints";
        VkComputePipelineCreateInfo pipe{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipe.stage = stage;
        pipe.layout = pipeline_layout;
        const VkResult pipe_result =
            vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipe, nullptr, &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        vk_check(pipe_result, "create compute pipeline");

        VkCommandPoolCreateInfo cmd_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cmd_pool.queueFamilyIndex = family;
        cmd_pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vk_check(vkCreateCommandPool(device, &cmd_pool, nullptr, &command_pool),
                 "create command pool");
        VkCommandBufferAllocateInfo cmd_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmd_alloc.commandPool = command_pool;
        cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_alloc.commandBufferCount = 1;
        vk_check(vkAllocateCommandBuffers(device, &cmd_alloc, &commands),
                 "allocate command buffer");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vk_check(vkCreateFence(device, &fence_info, nullptr, &fence), "create fence");
        if (timestamp_supported) {
            VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = 2;
            vk_check(vkCreateQueryPool(device, &query, nullptr, &queries),
                     "create timestamp pool");
        }

        // Transfer once: the prepared CPU buffers and GPU histories are
        // persistent. No CPU repacking or data copies happen in advance().
        void* mapped = nullptr;
        vk_check(vkMapMemory(device, staging.memory, 0, staging.bytes, 0, &mapped),
                 "map upload staging");
        for (int i = 0; i < 3; ++i)
            std::memcpy(static_cast<char*>(mapped) + vertex_bytes * i,
                        groom.rest_positions.data(), static_cast<std::size_t>(vertex_bytes));
        vkUnmapMemory(device, staging.memory);
        begin_commands();
        const std::array<Buffer*, 3> destinations{&current, &previous, &rest};
        for (std::uint32_t i = 0; i < 3; ++i) {
            VkBufferCopy copy{};
            copy.srcOffset = vertex_bytes * i;
            copy.size = vertex_bytes;
            vkCmdCopyBuffer(commands, staging.buffer, destinations[i]->buffer, 1, &copy);
        }
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        submit_and_wait();
    }

    void advance(const IntegrationParameters& p, std::uint32_t steps) {
        valid_parameters(p);
        if (steps == 0 || steps > 4096)
            throw std::invalid_argument("integration batch must contain 1..4096 steps");
        const PushParameters push{
            vertices, followers, guides, 0,
            p.time_step, p.gravity, p.damping,
            p.global_stiffness, p.global_range, 0, 0, 0
        };
        begin_commands();
        // Explicit visibility from previous batch's position writes to this
        // batch's position/history reads, even across a queue submission.
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        if (timestamp_supported) {
            vkCmdResetQueryPool(commands, queries, 0, 2);
            vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 0);
        }
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
        vkCmdPushConstants(commands, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);
        for (std::uint32_t i = 0; i < steps; ++i) {
            vkCmdDispatch(commands, groups, 1, 1);
            if (i + 1 < steps) {
                barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            }
        }
        if (timestamp_supported)
            vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 1);
        submit_and_wait();
        last_gpu_ms = -1.0;
        if (timestamp_supported) {
            std::array<std::uint64_t, 2> times{};
            vk_check(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(times),
                     times.data(), sizeof(times[0]),
                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                     "read GPU timestamps");
            const std::uint64_t delta = times[1] - times[0];
            const std::uint64_t diff = timestamp_valid_bits == 64 ? delta :
                (delta & ((std::uint64_t{1} << timestamp_valid_bits) - 1));
            last_gpu_ms = static_cast<double>(diff) *
                static_cast<double>(timestamp_period) / 1.0e6;
        }
    }

    std::vector<Float4> read_positions() {
        begin_commands();
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferCopy copy{};
        copy.size = vertex_bytes;
        vkCmdCopyBuffer(commands, current.buffer, staging.buffer, 1, &copy);
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        submit_and_wait();
        std::vector<Float4> result(static_cast<std::size_t>(vertex_bytes) / sizeof(Float4));
        void* mapped = nullptr;
        vk_check(vkMapMemory(device, staging.memory, 0, vertex_bytes, 0, &mapped),
                 "map readback staging");
        std::memcpy(result.data(), mapped, static_cast<std::size_t>(vertex_bytes));
        vkUnmapMemory(device, staging.memory);
        return result;
    }
};

IntegrationRuntime::IntegrationRuntime(const PreparedGroom& groom,
                                       const std::filesystem::path& spirv)
    : impl_(std::make_unique<Impl>()) {
    impl_->init(groom, spirv);
}
IntegrationRuntime::~IntegrationRuntime() = default;

void IntegrationRuntime::advance(const IntegrationParameters& p, std::uint32_t steps) {
    impl_->advance(p, steps);
}
std::vector<Float4> IntegrationRuntime::read_positions() {
    return impl_->read_positions();
}
const std::string& IntegrationRuntime::device_name() const noexcept {
    return impl_->device_name;
}
bool IntegrationRuntime::is_software_device() const noexcept {
    return impl_->software;
}
double IntegrationRuntime::last_gpu_milliseconds() const noexcept {
    return impl_->last_gpu_ms;
}
std::uint64_t IntegrationRuntime::resident_buffer_bytes() const noexcept {
    return static_cast<std::uint64_t>(impl_->vertex_bytes) * 3;
}

} // namespace loxfx::vulkan
