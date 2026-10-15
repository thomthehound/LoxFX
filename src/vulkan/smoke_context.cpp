// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
// INTERNAL experimental GPU proof: replace with production context later.
#include "loxfx/vulkan/smoke_context.h"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace loxfx::vulkan::smoke {
namespace {
thread_local int creation_failure_countdown = -1;
void creation_checkpoint() {
    if (creation_failure_countdown < 0) return;
    if (creation_failure_countdown-- == 0) {
        creation_failure_countdown = -1;
        throw std::runtime_error("injected Vulkan resource creation failure");
    }
}
} // namespace

void diagnostic_fail_creation_after(int checkpoints) noexcept {
    creation_failure_countdown = checkpoints;
}

namespace {
void verify(VkResult rc, const char* operation) {
    if (rc != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " (VkResult " +
                                 std::to_string(rc) + ")");
}
struct Constants {
    std::uint32_t vertices, followers, guides, reserved = 0;
    float dt, gravity, damping, stiffness, range;
    float root_translation_x, root_translation_y, root_translation_z;
};
static_assert(sizeof(Constants) == 48);
std::vector<std::uint32_t> load_shader(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("cannot open integration SPIR-V");
    const auto n = stream.tellg();
    if (n <= 0 || n % 4 != 0 || n > 8 * 1024 * 1024)
        throw std::runtime_error("invalid SPIR-V length");
    std::vector<std::uint32_t> result(static_cast<std::size_t>(n) / 4);
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(result.data()), n) ||
        result.front() != 0x07230203u)
        throw std::runtime_error("bad SPIR-V contents");
    return result;
}
void validate_parameters(const IntegrationParameters& p) {
    if (!std::isfinite(p.time_step) || p.time_step <= 0 || p.time_step > 0.1f ||
        !std::isfinite(p.gravity) || !std::isfinite(p.damping) || p.damping < 0 ||
        !std::isfinite(p.global_stiffness) || p.global_stiffness < 0 ||
        p.global_stiffness > 1 || !std::isfinite(p.global_range) ||
        p.global_range < 0 || p.global_range > 1 ||
        !std::isfinite(p.root_translation_x) ||
        !std::isfinite(p.root_translation_y) ||
        !std::isfinite(p.root_translation_z))
        throw std::invalid_argument("invalid integration parameters");
}
struct Buffer {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0, allocation_bytes = 0;

    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer() { reset(); }
    void reset() noexcept {
        if (device) {
            if (handle) vkDestroyBuffer(device, handle, nullptr);
            if (memory) vkFreeMemory(device, memory, nullptr);
        }
        handle = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        size = allocation_bytes = 0;
        device = VK_NULL_HANDLE;
    }
    void create(VkPhysicalDevice physical, VkDevice logical,
                VkDeviceSize bytes, VkBufferUsageFlags usage,
                VkMemoryPropertyFlags properties) {
        if (device || bytes == 0) throw std::invalid_argument("invalid buffer init");
        device = logical;
        size = bytes;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = bytes;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer created = VK_NULL_HANDLE;
        creation_checkpoint();
        verify(vkCreateBuffer(logical, &ci, nullptr, &created), "create Vulkan buffer");
        handle = created; // Commit only a successfully created resource.
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(logical, handle, &req);
        VkPhysicalDeviceMaintenance3Properties maintenance{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
        VkPhysicalDeviceProperties2 props{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &maintenance;
        vkGetPhysicalDeviceProperties2(physical, &props);
        if (req.size > maintenance.maxMemoryAllocationSize)
            throw std::runtime_error("Vulkan allocation exceeds device limit");

        VkPhysicalDeviceMemoryProperties mem{};
        vkGetPhysicalDeviceMemoryProperties(physical, &mem);
        bool matching = false;
        VkResult last = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        for (std::uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
            if (!(req.memoryTypeBits & (1u << i)) ||
                (mem.memoryTypes[i].propertyFlags & properties) != properties ||
                mem.memoryHeaps[mem.memoryTypes[i].heapIndex].size < req.size)
                continue;
            matching = true;
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = i;
            VkDeviceMemory allocation = VK_NULL_HANDLE;
            creation_checkpoint();
            last = vkAllocateMemory(logical, &ai, nullptr, &allocation);
            if (last == VK_SUCCESS) {
                memory = allocation;
                allocation_bytes = req.size;
                creation_checkpoint();
                verify(vkBindBufferMemory(logical, handle, memory, 0),
                       "bind Vulkan buffer memory");
                return;
            }
            if (last != VK_ERROR_OUT_OF_DEVICE_MEMORY)
                verify(last, "allocate Vulkan buffer");
        }
        if (!matching)
            throw std::runtime_error("no compatible memory type for Vulkan buffer");
        verify(last, "compatible Vulkan heaps cannot allocate buffer");
    }
};
void memory_barrier(VkCommandBuffer command, VkPipelineStageFlags src,
                    VkAccessFlags from, VkPipelineStageFlags dst, VkAccessFlags to) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = from;
    barrier.dstAccessMask = to;
    vkCmdPipelineBarrier(command, src, dst, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
void check_groom(const PreparedGroom& groom) {
    const auto result = validate_prepared_groom(groom);
    if (!result) throw std::invalid_argument(result.error());
}
} // namespace

struct DeviceContext::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    bool own_device = false, software = false;
    std::string name;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipeline strand_pipeline = VK_NULL_HANDLE;
    VkPipeline velocity_shock_pipeline = VK_NULL_HANDLE;
    VkPipeline local_shape_pipeline = VK_NULL_HANDLE;
    VkPipeline length_wind_pipeline = VK_NULL_HANDLE;
    VkPipeline follower_pipeline = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer diagnostic_commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    std::uint32_t timestamp_bits = 0;
    float timestamp_period = 0;
    double last_ms = -1;
    std::uint64_t submissions = 0;
    bool diagnostic_active = false;

    ~Impl() {
        if (device) {
            // Caller must synchronize borrowed device use before destruction.
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (queries) vkDestroyQueryPool(device, queries, nullptr);
            if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
            if (follower_pipeline) vkDestroyPipeline(device, follower_pipeline, nullptr);
            if (length_wind_pipeline) vkDestroyPipeline(device, length_wind_pipeline, nullptr);
            if (local_shape_pipeline) vkDestroyPipeline(device, local_shape_pipeline, nullptr);
            if (velocity_shock_pipeline) vkDestroyPipeline(device, velocity_shock_pipeline, nullptr);
            if (strand_pipeline) vkDestroyPipeline(device, strand_pipeline, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            if (layout) vkDestroyDescriptorSetLayout(device, layout, nullptr);
            if (own_device) vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    void configure(ExternalDevice ext) {
        if (!ext.physical || !ext.logical || !ext.compute_queue)
            throw std::invalid_argument("invalid borrowed Vulkan device/queue");
        std::uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(ext.physical, &families, nullptr);
        if (ext.queue_family >= families)
            throw std::invalid_argument("invalid compute queue family index");
        std::vector<VkQueueFamilyProperties> q(families);
        vkGetPhysicalDeviceQueueFamilyProperties(ext.physical, &families, q.data());
        if (!(q[ext.queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT))
            throw std::invalid_argument("selected queue family cannot compute");
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(ext.physical, &props);
        if (props.apiVersion < VK_API_VERSION_1_2 ||
            props.limits.maxComputeWorkGroupSize[0] < 64 ||
            props.limits.maxComputeWorkGroupInvocations < 64 ||
            props.limits.maxPushConstantsSize < 96 ||
            props.limits.maxDescriptorSetStorageBuffers < 7 ||
            props.limits.maxPerStageDescriptorStorageBuffers < 7)
            throw std::invalid_argument("selected Vulkan device cannot run integration shader");
        physical = ext.physical;
        device = ext.logical;
        queue = ext.compute_queue;
        family = ext.queue_family;
        name = props.deviceName;
        software = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        timestamp_bits = q[family].timestampValidBits;
        timestamp_period = props.limits.timestampPeriod;
    }
    void initialize_standalone() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "LoxFX GPU smoke";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        VkInstance fresh = VK_NULL_HANDLE;
        creation_checkpoint();
        verify(vkCreateInstance(&ici, nullptr, &fresh), "create Vulkan instance");
        instance = fresh;
        std::uint32_t count = 0;
        verify(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate GPUs");
        if (!count) throw std::runtime_error("no Vulkan physical device");
        std::vector<VkPhysicalDevice> candidates(count);
        verify(vkEnumeratePhysicalDevices(instance, &count, candidates.data()), "enumerate GPUs");
        // Explicit context selection once, never in per-groom construction.
        for (int tier = 0; tier < 2 && !physical; ++tier) {
            for (auto candidate : candidates) {
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(candidate, &props);
                if ((props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) != (tier == 1) ||
                    props.apiVersion < VK_API_VERSION_1_2 ||
                    props.limits.maxComputeWorkGroupSize[0] < 64 ||
                    props.limits.maxComputeWorkGroupInvocations < 64 ||
                    props.limits.maxPushConstantsSize < 96 ||
                    props.limits.maxDescriptorSetStorageBuffers < 7 ||
                    props.limits.maxPerStageDescriptorStorageBuffers < 7)
                    continue;
                std::uint32_t count_q = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count_q, nullptr);
                std::vector<VkQueueFamilyProperties> families(count_q);
                vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count_q, families.data());
                for (std::uint32_t i = 0; i < count_q; ++i) {
                    if (!(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
                    const float priority = 1.0f;
                    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                    qi.queueFamilyIndex = i;
                    qi.queueCount = 1;
                    qi.pQueuePriorities = &priority;
                    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                    di.queueCreateInfoCount = 1;
                    di.pQueueCreateInfos = &qi;
                    VkDevice fresh_device = VK_NULL_HANDLE;
                    creation_checkpoint();
                    if (vkCreateDevice(candidate, &di, nullptr, &fresh_device) != VK_SUCCESS)
                        continue;
                    // Commit only a valid device. This test-owned adapter
                    // will not silently switch based on individual grooms.
                    VkQueue selected_queue = VK_NULL_HANDLE;
                    vkGetDeviceQueue(fresh_device, i, 0, &selected_queue);
                    // Commit the device before any fallible validation.
                    // The Impl destructor now owns it on every throw path.
                    device = fresh_device;
                    own_device = true;
                    configure({candidate, fresh_device, selected_queue, i});
                    break;
                }
                if (physical) break;
            }
        }
        if (!physical) throw std::runtime_error("no qualified Vulkan compute device");
    }
    void initialize_pipeline(const std::filesystem::path& spirv) {
        std::array<VkDescriptorSetLayoutBinding,7> bindings{};
        for (std::uint32_t i=0;i<bindings.size();++i) {
            bindings[i].binding=i;
            bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount=1;
            bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount=static_cast<std::uint32_t>(bindings.size());
        lci.pBindings=bindings.data();
        VkDescriptorSetLayout created_layout=VK_NULL_HANDLE;
        creation_checkpoint();
        verify(vkCreateDescriptorSetLayout(device,&lci,nullptr,&created_layout),
               "create shader resource layout");
        layout=created_layout;
        VkPushConstantRange push{};
        push.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
        push.size=96;
        VkPipelineLayoutCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pci.setLayoutCount=1;
        pci.pSetLayouts=&layout;
        pci.pushConstantRangeCount=1;
        pci.pPushConstantRanges=&push;
        VkPipelineLayout created_pipeline_layout=VK_NULL_HANDLE;
        creation_checkpoint();
        verify(vkCreatePipelineLayout(device,&pci,nullptr,&created_pipeline_layout),
               "create pipeline layout");
        pipeline_layout=created_pipeline_layout;
        const auto shader=load_shader(spirv);
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize=shader.size()*sizeof(std::uint32_t);
        smci.pCode=shader.data();
        VkShaderModule module=VK_NULL_HANDLE;
        creation_checkpoint();
        verify(vkCreateShaderModule(device,&smci,nullptr,&module),"create integration module");
        VkPipelineShaderStageCreateInfo sci{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        sci.stage=VK_SHADER_STAGE_COMPUTE_BIT;
        sci.module=module;
        sci.pName="IntegrationAndGlobalShapeConstraints";
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage=sci;
        cpi.layout=pipeline_layout;
        VkPipeline created_pipeline=VK_NULL_HANDLE;
        creation_checkpoint();
        const VkResult build=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cpi,nullptr,&created_pipeline);
        vkDestroyShaderModule(device,module,nullptr);
        verify(build,"create integration pipeline");
        pipeline=created_pipeline;
        const auto strand_shader=load_shader(spirv.parent_path()/"loxfx_strand_level.spv");
        VkShaderModuleCreateInfo strand_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        strand_info.codeSize=strand_shader.size()*sizeof(std::uint32_t);
        strand_info.pCode=strand_shader.data();
        VkShaderModule strand_module=VK_NULL_HANDLE;
        verify(vkCreateShaderModule(device,&strand_info,nullptr,&strand_module),
               "create strand-level module");
        sci.module=strand_module;
        sci.pName="CalculateStrandLevelData";
        cpi.stage=sci;
        VkPipeline created_strand=VK_NULL_HANDLE;
        const VkResult strand_result=vkCreateComputePipelines(
            device,VK_NULL_HANDLE,1,&cpi,nullptr,&created_strand);
        vkDestroyShaderModule(device,strand_module,nullptr);
        verify(strand_result,"create strand-level pipeline");
        strand_pipeline=created_strand;
        const auto vsp_shader=load_shader(spirv.parent_path()/"loxfx_velocity_shock.spv");
        VkShaderModuleCreateInfo vsp_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        vsp_info.codeSize=vsp_shader.size()*sizeof(std::uint32_t);
        vsp_info.pCode=vsp_shader.data();
        VkShaderModule vsp_module=VK_NULL_HANDLE;
        verify(vkCreateShaderModule(device,&vsp_info,nullptr,&vsp_module),
               "create velocity-shock module");
        sci.module=vsp_module;
        sci.pName="VelocityShockPropagation";
        cpi.stage=sci;
        VkPipeline created_vsp=VK_NULL_HANDLE;
        const VkResult vsp_result=vkCreateComputePipelines(
            device,VK_NULL_HANDLE,1,&cpi,nullptr,&created_vsp);
        vkDestroyShaderModule(device,vsp_module,nullptr);
        verify(vsp_result,"create velocity-shock pipeline");
        velocity_shock_pipeline=created_vsp;
        const auto shape_shader=load_shader(spirv.parent_path()/"loxfx_local_shape.spv");
        VkShaderModuleCreateInfo shape_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shape_info.codeSize=shape_shader.size()*sizeof(std::uint32_t);
        shape_info.pCode=shape_shader.data();
        VkShaderModule shape_module=VK_NULL_HANDLE;
        verify(vkCreateShaderModule(device,&shape_info,nullptr,&shape_module),
               "create local-shape shader module");
        sci.module=shape_module;
        sci.pName="LocalShapeConstraints";
        cpi.stage=sci;
        VkPipeline created_shape=VK_NULL_HANDLE;
        const VkResult shape_result=vkCreateComputePipelines(
            device,VK_NULL_HANDLE,1,&cpi,nullptr,&created_shape);
        vkDestroyShaderModule(device,shape_module,nullptr);
        verify(shape_result,"create local-shape pipeline");
        local_shape_pipeline=created_shape;
        const auto shader5=load_shader(spirv.parent_path()/"loxfx_length_wind.spv");
        VkShaderModuleCreateInfo shader5_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader5_info.codeSize=shader5.size()*sizeof(std::uint32_t);
        shader5_info.pCode=shader5.data();
        VkShaderModule module5=VK_NULL_HANDLE;
        verify(vkCreateShaderModule(device,&shader5_info,nullptr,&module5),
               "create length-wind module");
        sci.module=module5;
        sci.pName="LengthWindConstraints";
        cpi.stage=sci;
        VkPipeline created5=VK_NULL_HANDLE;
        const VkResult result5=vkCreateComputePipelines(
            device,VK_NULL_HANDLE,1,&cpi,nullptr,&created5);
        vkDestroyShaderModule(device,module5,nullptr);
        verify(result5,"create length-wind pipeline");
        length_wind_pipeline=created5;
        const auto followers=load_shader(spirv.parent_path()/"loxfx_follower_publish.spv");
        VkShaderModuleCreateInfo follower_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        follower_info.codeSize=followers.size()*sizeof(std::uint32_t);
        follower_info.pCode=followers.data();
        VkShaderModule follower_module=VK_NULL_HANDLE;
        verify(vkCreateShaderModule(device,&follower_info,nullptr,&follower_module),
               "create follower publication shader module");
        sci.module=follower_module;
        sci.pName="UpdateFollowHairVertices";
        cpi.stage=sci;
        VkPipeline created_followers=VK_NULL_HANDLE;
        const VkResult follower_result=vkCreateComputePipelines(
            device,VK_NULL_HANDLE,1,&cpi,nullptr,&created_followers);
        vkDestroyShaderModule(device,follower_module,nullptr);
        verify(follower_result,"create follower publication pipeline");
        follower_pipeline=created_followers;

        VkCommandPoolCreateInfo poolci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolci.queueFamilyIndex=family;
        poolci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCommandPool created_pool=VK_NULL_HANDLE;
        creation_checkpoint();
        verify(vkCreateCommandPool(device,&poolci,nullptr,&created_pool),"create diagnostic pool");
        command_pool=created_pool;
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool=command_pool;
        cbai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount=1;
        VkCommandBuffer command=VK_NULL_HANDLE;
        verify(vkAllocateCommandBuffers(device,&cbai,&command),"create diagnostic command");
        diagnostic_commands=command;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence created_fence=VK_NULL_HANDLE;
        creation_checkpoint();
        verify(vkCreateFence(device,&fci,nullptr,&created_fence),"create diagnostic fence");
        fence=created_fence;
        if (timestamp_bits) {
            VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qci.queryType=VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount=2;
            VkQueryPool created_queries=VK_NULL_HANDLE;
            verify(vkCreateQueryPool(device,&qci,nullptr,&created_queries),"create timestamp pool");
            queries=created_queries;
        }
    }
    VkCommandBuffer begin_diagnostics() {
        if (diagnostic_active) throw std::logic_error("diagnostic batch already recording");
        verify(vkResetCommandBuffer(diagnostic_commands,0),"reset diagnostic buffer");
        VkCommandBufferBeginInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        ci.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        verify(vkBeginCommandBuffer(diagnostic_commands,&ci),"begin diagnostic batch");
        diagnostic_active=true;
        if (queries) {
            vkCmdResetQueryPool(diagnostic_commands,queries,0,2);
            vkCmdWriteTimestamp(diagnostic_commands,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,0);
        }
        return diagnostic_commands;
    }
    void submit_diagnostics() {
        if (!diagnostic_active) throw std::logic_error("no active diagnostic batch");
        if (queries)
            vkCmdWriteTimestamp(diagnostic_commands,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,1);
        verify(vkEndCommandBuffer(diagnostic_commands),"end diagnostic commands");
        diagnostic_active=false;
        verify(vkResetFences(device,1,&fence),"reset diagnostic fence");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount=1;
        si.pCommandBuffers=&diagnostic_commands;
        verify(vkQueueSubmit(queue,1,&si,fence),"submit diagnostic batch");
        ++submissions;
        verify(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"wait diagnostic fence");
        last_ms=-1;
        if (queries) {
            std::array<std::uint64_t,2> t{};
            verify(vkGetQueryPoolResults(device,queries,0,2,sizeof(t),t.data(),
                                        sizeof(t[0]),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT),
                   "read timestamp results");
            const auto delta=t[1]-t[0];
            const auto diff=timestamp_bits==64?delta:
                (delta&((std::uint64_t{1}<<timestamp_bits)-1));
            last_ms=static_cast<double>(diff)*timestamp_period/1.0e6;
        }
    }
};

struct GroomState::Impl {
    std::shared_ptr<void> device_lifetime;
    VkDevice device=VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool=VK_NULL_HANDLE;
    VkDescriptorSet set=VK_NULL_HANDLE, strand_set=VK_NULL_HANDLE;
    Buffer current, previous, rest, root_previous_previous, strand_data,
           guide_rest_lengths, tangents;
    std::uint32_t vertices=0, followers=0, guides=0, groups=0, strand_groups=0;
    std::uint32_t local_dispatch_iterations=0, local_shader_iterations=0;
    VkDeviceSize vertex_bytes=0, root_history_bytes=0, strand_bytes=0,
                 guide_length_bytes=0;
    const void* context_id=nullptr;
    ~Impl() {
        if (device && descriptor_pool)
            vkDestroyDescriptorPool(device,descriptor_pool,nullptr);
    }
};

GroomState::GroomState(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
GroomState::~GroomState()=default;
GroomState::GroomState(GroomState&&) noexcept=default;
GroomState& GroomState::operator=(GroomState&&) noexcept=default;
std::uint64_t GroomState::allocated_buffer_bytes() const noexcept {
    if(!impl_)return 0;
    return impl_->current.allocation_bytes+impl_->previous.allocation_bytes+
           impl_->rest.allocation_bytes+
           impl_->root_previous_previous.allocation_bytes+
           impl_->strand_data.allocation_bytes+
           impl_->guide_rest_lengths.allocation_bytes+
           impl_->tangents.allocation_bytes;
}


VkBuffer GroomState::positions_buffer() const noexcept {
    return impl_?impl_->current.handle:VK_NULL_HANDLE;
}
VkBuffer GroomState::tangents_buffer() const noexcept {
    return impl_?impl_->tangents.handle:VK_NULL_HANDLE;
}

DeviceContext::DeviceContext(const std::filesystem::path& spirv)
:impl_(std::make_shared<Impl>()){
    impl_->initialize_standalone();
    impl_->initialize_pipeline(spirv);
}
DeviceContext::DeviceContext(ExternalDevice borrowed,const std::filesystem::path& spirv)
:impl_(std::make_shared<Impl>()){
    impl_->configure(borrowed);
    impl_->initialize_pipeline(spirv);
}
DeviceContext::~DeviceContext()=default;
DeviceContext::DeviceContext(DeviceContext&&) noexcept=default;
DeviceContext& DeviceContext::operator=(DeviceContext&&) noexcept=default;

GroomState DeviceContext::prepare(const PreparedGroom& source) {
    if(!impl_)throw std::logic_error("moved-from device context");
    check_groom(source);
    const VkDeviceSize bytes=static_cast<VkDeviceSize>(source.rest_positions.size())*sizeof(Float4);
    const VkDeviceSize root_bytes=static_cast<VkDeviceSize>(
        source.work.packed_guide_strands)*2*sizeof(Float4);
    const VkDeviceSize strand_bytes=static_cast<VkDeviceSize>(
        source.work.packed_guide_strands)*sizeof(StrandMotionData);
    const VkDeviceSize guide_length_bytes=static_cast<VkDeviceSize>(
        source.work.packed_guide_strands)*source.vertices_per_strand*sizeof(float);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(impl_->physical,&props);
    if(bytes>props.limits.maxStorageBufferRange ||
       root_bytes>props.limits.maxStorageBufferRange ||
       strand_bytes>props.limits.maxStorageBufferRange ||
       guide_length_bytes>props.limits.maxStorageBufferRange ||
       source.work.stages[1].thread_groups>props.limits.maxComputeWorkGroupCount[0] ||
       source.work.stages[0].thread_groups>props.limits.maxComputeWorkGroupCount[0])
        throw std::invalid_argument("groom exceeds selected device resource or dispatch limit");

    auto groom=std::make_unique<GroomState::Impl>();
    groom->device_lifetime=impl_;
    groom->device=impl_->device;
    groom->context_id=impl_.get();
    groom->vertex_bytes=bytes;
    groom->root_history_bytes=root_bytes;
    groom->strand_bytes=strand_bytes;
    groom->guide_length_bytes=guide_length_bytes;
    groom->strand_groups=source.work.stages[1].thread_groups;
    groom->vertices=source.vertices_per_strand;
    groom->followers=source.followers_per_guide;
    groom->guides=source.work.packed_guide_strands;
    groom->groups=source.work.stages[0].thread_groups;
    groom->local_dispatch_iterations=source.work.stages[3].iterations;
    groom->local_shader_iterations=source.work.shader_local_shape_iterations;
    constexpr auto transfer=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    constexpr auto storage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    groom->current.create(impl_->physical,impl_->device,bytes,
                          storage|transfer|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    groom->previous.create(impl_->physical,impl_->device,bytes,storage|transfer,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    groom->rest.create(impl_->physical,impl_->device,bytes,storage|transfer,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    groom->root_previous_previous.create(
        impl_->physical,impl_->device,root_bytes,
        storage|transfer|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    groom->strand_data.create(
        impl_->physical,impl_->device,strand_bytes,
        storage|transfer|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    groom->guide_rest_lengths.create(
        impl_->physical,impl_->device,guide_length_bytes,storage|transfer,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    groom->tangents.create(impl_->physical,impl_->device,bytes,
                          storage|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,14};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets=2;
    pool.poolSizeCount=1;
    pool.pPoolSizes=&size;
    VkDescriptorPool created_pool=VK_NULL_HANDLE;
    creation_checkpoint();
    verify(vkCreateDescriptorPool(impl_->device,&pool,nullptr,&created_pool),
           "create groom descriptor pool");
    groom->descriptor_pool=created_pool;
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool=groom->descriptor_pool;
    std::array<VkDescriptorSetLayout,2> layouts{impl_->layout,impl_->layout};
    alloc.descriptorSetCount=static_cast<std::uint32_t>(layouts.size());
    alloc.pSetLayouts=layouts.data();
    std::array<VkDescriptorSet,2> sets{VK_NULL_HANDLE,VK_NULL_HANDLE};
    creation_checkpoint();
    verify(vkAllocateDescriptorSets(impl_->device,&alloc,sets.data()),
           "create groom descriptor sets");
    groom->set=sets[0];
    groom->strand_set=sets[1];
    std::array<VkDescriptorBufferInfo,7> info{{
       {groom->current.handle,0,bytes},
       {groom->previous.handle,0,bytes},
       {groom->rest.handle,0,bytes},
       {groom->root_previous_previous.handle,0,root_bytes},
       {groom->rest.handle,0,bytes},
       {groom->guide_rest_lengths.handle,0,guide_length_bytes},
       {groom->tangents.handle,0,bytes}}};
    std::array<VkWriteDescriptorSet,7> writes{};
    for(std::uint32_t i=0;i<writes.size();++i){
        writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet=groom->set;
        writes[i].dstBinding=i;
        writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount=1;
        writes[i].pBufferInfo=&info[i];
    }
    std::array<VkDescriptorBufferInfo,7> motion_info{{
       {groom->current.handle,0,bytes},
       {groom->previous.handle,0,bytes},
       {groom->root_previous_previous.handle,0,root_bytes},
       {groom->strand_data.handle,0,strand_bytes},
       {groom->rest.handle,0,bytes},
       {groom->guide_rest_lengths.handle,0,guide_length_bytes},
       {groom->tangents.handle,0,bytes}}};
    std::array<VkWriteDescriptorSet,7> motion_writes{};
    for(std::uint32_t i=0;i<motion_writes.size();++i){
        motion_writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        motion_writes[i].dstSet=groom->strand_set;
        motion_writes[i].dstBinding=i;
        motion_writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        motion_writes[i].descriptorCount=1;
        motion_writes[i].pBufferInfo=&motion_info[i];
    }
    vkUpdateDescriptorSets(impl_->device,
        static_cast<std::uint32_t>(writes.size()),writes.data(),0,nullptr);
    vkUpdateDescriptorSets(impl_->device,
        static_cast<std::uint32_t>(motion_writes.size()),motion_writes.data(),0,nullptr);

    // One temporary staging image, reclaimed immediately AFTER the explicit
    // initialization submit has completed. Never retained by a groom.
    Buffer staging;
    staging.create(impl_->physical,impl_->device,bytes+root_bytes+guide_length_bytes,
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped=nullptr;
    creation_checkpoint();
    verify(vkMapMemory(impl_->device,staging.memory,0,bytes+root_bytes+guide_length_bytes,0,&mapped),
           "map initial upload");
    std::memcpy(mapped,source.rest_positions.data(),static_cast<std::size_t>(bytes));
    auto* roots=reinterpret_cast<Float4*>(static_cast<char*>(mapped)+bytes);
    for(std::uint32_t guide=0;guide<source.work.packed_guide_strands;++guide){
        const std::size_t first=static_cast<std::size_t>(guide)*
            (source.followers_per_guide+1)*source.vertices_per_strand;
        roots[guide*2]=source.rest_positions[first];
        roots[guide*2+1]=source.rest_positions[first+1];
    }
    float* lengths=reinterpret_cast<float*>(
        static_cast<char*>(mapped)+bytes+root_bytes);
    for(std::uint32_t guide=0;guide<source.work.packed_guide_strands;++guide) {
        const std::size_t base=static_cast<std::size_t>(guide)*
            (source.followers_per_guide+1)*source.vertices_per_strand;
        for(std::uint32_t v=0;v<source.vertices_per_strand;++v)
            lengths[static_cast<std::size_t>(guide)*source.vertices_per_strand+v]=
                source.rest_segment_lengths[base+v];
    }
    vkUnmapMemory(impl_->device,staging.memory);
    auto cb=impl_->begin_diagnostics();
    for(VkBuffer dest:{groom->current.handle,groom->previous.handle,groom->rest.handle}) {
        VkBufferCopy copy{};
        copy.size=bytes;
        vkCmdCopyBuffer(cb,staging.handle,dest,1,&copy);
    }
    VkBufferCopy roots_copy{};
    roots_copy.srcOffset=bytes;
    roots_copy.size=root_bytes;
    vkCmdCopyBuffer(cb,staging.handle,
                    groom->root_previous_previous.handle,1,&roots_copy);
    VkBufferCopy length_copy{};
    length_copy.srcOffset=bytes+root_bytes;
    length_copy.size=guide_length_bytes;
    vkCmdCopyBuffer(cb,staging.handle,
                    groom->guide_rest_lengths.handle,1,&length_copy);
    memory_barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    impl_->submit_diagnostics();
    return GroomState(std::move(groom));
}

void DeviceContext::record_integration(VkCommandBuffer cb,const GroomState& state,
                                       const IntegrationParameters& p,
                                       std::uint32_t steps) const {
    if(!impl_||!state.impl_||!cb)throw std::invalid_argument("invalid integration context");
    if(state.impl_->context_id!=impl_.get())
        throw std::invalid_argument("groom belongs to different device context");
    validate_parameters(p);
    if(steps==0||steps>4096)throw std::invalid_argument("invalid integration step count");
    const auto& g=*state.impl_;
    const Constants params{g.vertices,g.followers,g.guides,0,
                           p.time_step,p.gravity,p.damping,
                           p.global_stiffness,p.global_range,
                           p.root_translation_x,p.root_translation_y,
                           p.root_translation_z};
    // Source-derived inter-step dependency, also covers prior frame's writes.
    memory_barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,impl_->pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,impl_->pipeline_layout,
                            0,1,&g.set,0,nullptr);
    vkCmdPushConstants(cb,impl_->pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,
                       0,sizeof(params),&params);
    for(std::uint32_t i=0;i<steps;++i){
        vkCmdDispatch(cb,g.groups,1,1);
        if(i+1<steps)
            memory_barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    }
}
void DeviceContext::record_strand_level(VkCommandBuffer cb,const GroomState& state,
                                        const StrandMotionParameters& p) const {
    if(!impl_||!state.impl_||!cb||state.impl_->context_id!=impl_.get())
        throw std::invalid_argument("strand pass belongs to another Vulkan context");
    if(!std::isfinite(p.velocity_shock_coefficient) ||
       p.velocity_shock_coefficient<0 || p.velocity_shock_coefficient>1 ||
       !std::isfinite(p.acceleration_threshold) || p.acceleration_threshold<0)
        throw std::invalid_argument("invalid strand motion parameters");
    const auto& g=*state.impl_;
    struct MotionConstants {
        std::uint32_t vertices, followers, guides, reserved;
        float coefficient, threshold, padding0, padding1;
    };
    const MotionConstants data{g.vertices,g.followers,g.guides,0,
                               p.velocity_shock_coefficient,p.acceleration_threshold,0,0};
    memory_barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,impl_->strand_pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,impl_->pipeline_layout,
                            0,1,&g.strand_set,0,nullptr);
    vkCmdPushConstants(cb,impl_->pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,
                       0,sizeof(data),&data);
    vkCmdDispatch(cb,g.strand_groups,1,1);
}
void DeviceContext::record_velocity_shock(VkCommandBuffer cb,
                                          const GroomState& state) const {
    if(!impl_||!state.impl_||!cb||state.impl_->context_id!=impl_.get())
        throw std::invalid_argument("velocity-shock pass belongs to another Vulkan context");
    const auto& g=*state.impl_;
    struct VspConstants {
        std::uint32_t vertices, followers, guides, reserved;
    };
    const VspConstants data{g.vertices,g.followers,g.guides,0};
    // Stage-two output and the integration buffers must be visible to VSP.
    memory_barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,
                      impl_->velocity_shock_pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,impl_->pipeline_layout,
                            0,1,&g.strand_set,0,nullptr);
    vkCmdPushConstants(cb,impl_->pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,
                       0,sizeof(data),&data);
    vkCmdDispatch(cb,g.groups,1,1);
}

void DeviceContext::record_local_shape(VkCommandBuffer cb,const GroomState& state,
                                       float stiffness) const {
    if(!impl_||!cb||!state.impl_||state.impl_->context_id!=impl_.get())
        throw std::invalid_argument("local-shape pass belongs to another Vulkan context");
    if(!std::isfinite(stiffness)||stiffness<0.0f||stiffness>1.0f)
        throw std::invalid_argument("invalid local-shape stiffness");
    const auto& g=*state.impl_;
    if(g.local_dispatch_iterations==0 || stiffness==0.0f) return;
    struct ShapeConstants {
        std::uint32_t vertices,followers,guides,shader_iterations;
        float local_stiffness;
    };
    const ShapeConstants c{g.vertices,g.followers,g.guides,
                           g.local_shader_iterations,stiffness};
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,impl_->local_shape_pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,impl_->pipeline_layout,
                            0,1,&g.strand_set,0,nullptr);
    vkCmdPushConstants(cb,impl_->pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,
                       0,sizeof(c),&c);
    for(std::uint32_t i=0;i<g.local_dispatch_iterations;++i) {
        // Prior VSP writes and each repeated pass must be visible to
        // the next local-shape dispatch. Each guide owns its own vertices.
        memory_barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdDispatch(cb,g.strand_groups,1,1);
    }
}
void DeviceContext::record_length_wind(VkCommandBuffer cb,const GroomState& state,
                                       const LengthWindParameters& p) const {
    if(!impl_||!cb||!state.impl_||state.impl_->context_id!=impl_.get())
        throw std::invalid_argument("length-wind pass belongs to another Vulkan context");
    if(!std::isfinite(p.time_step)||p.time_step<=0.0f||p.time_step>0.1f||
       !std::isfinite(p.maximum_position_delta)||p.maximum_position_delta<0.0f||
       p.length_iterations>64u)
        throw std::invalid_argument("invalid length-wind parameters");
    for(const auto& w:p.winds)
        if(!std::isfinite(w.x)||!std::isfinite(w.y)||!std::isfinite(w.z))
            throw std::invalid_argument("nonfinite wind parameters");
    const auto& g=*state.impl_;
    struct LengthConstants {
        std::uint32_t vertices,followers,guides,iterations;
        float dt,clamp,zero0,zero1;
        std::array<Float4,4> winds;
    };
    static_assert(sizeof(LengthConstants)==96);
    const LengthConstants c{g.vertices,g.followers,g.guides,
        p.length_iterations,p.time_step,p.maximum_position_delta,0,0,p.winds};
    memory_barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,impl_->length_wind_pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,impl_->pipeline_layout,
                            0,1,&g.strand_set,0,nullptr);
    vkCmdPushConstants(cb,impl_->pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,
                       0,sizeof(c),&c);
    vkCmdDispatch(cb,g.groups,1,1);
}

void DeviceContext::record_followers(VkCommandBuffer cb,
                                     const GroomState& state) const {
    if(!impl_||!cb||!state.impl_||state.impl_->context_id!=impl_.get())
        throw std::invalid_argument("follower pass belongs to another Vulkan context");
    const auto& g=*state.impl_;
    struct FollowerConstants {
        std::uint32_t vertices,followers,guides,reserved;
    };
    const FollowerConstants data{g.vertices,g.followers,g.guides,0};
    memory_barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,impl_->follower_pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,impl_->pipeline_layout,
                            0,1,&g.strand_set,0,nullptr);
    vkCmdPushConstants(cb,impl_->pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,
                       0,sizeof(data),&data);
    vkCmdDispatch(cb,g.groups,1,1);
}

VkCommandBuffer DeviceContext::begin_diagnostic_commands(){
    if(!impl_)throw std::logic_error("context was moved");
    return impl_->begin_diagnostics();
}
void DeviceContext::submit_diagnostic_and_wait(){
    if(!impl_)throw std::logic_error("context was moved");
    impl_->submit_diagnostics();
}
std::vector<Float4> DeviceContext::diagnostic_read_positions(const GroomState& g){
    if(!impl_||!g.impl_||g.impl_->context_id!=impl_.get())
        throw std::invalid_argument("diagnostic readback on wrong device context");
    const auto bytes=g.impl_->vertex_bytes;
    Buffer staging;
    staging.create(impl_->physical,impl_->device,bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const auto cb=impl_->begin_diagnostics();
    memory_barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy copy{};
    copy.size=bytes;
    vkCmdCopyBuffer(cb,g.impl_->current.handle,staging.handle,1,&copy);
    memory_barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
    impl_->submit_diagnostics();
    std::vector<Float4> result(static_cast<std::size_t>(bytes)/sizeof(Float4));
    void* mapped=nullptr;
    verify(vkMapMemory(impl_->device,staging.memory,0,bytes,0,&mapped),
           "map diagnostic readback");
    std::memcpy(result.data(),mapped,static_cast<std::size_t>(bytes));
    vkUnmapMemory(impl_->device,staging.memory);
    return result;
}
std::vector<Float4> DeviceContext::diagnostic_read_tangents(const GroomState& g){
    if(!impl_||!g.impl_||g.impl_->context_id!=impl_.get())
        throw std::invalid_argument("diagnostic readback on wrong device context");
    const auto bytes=g.impl_->vertex_bytes;
    Buffer staging;
    staging.create(impl_->physical,impl_->device,bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const auto cb=impl_->begin_diagnostics();
    memory_barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy copy{};
    copy.size=bytes;
    vkCmdCopyBuffer(cb,g.impl_->tangents.handle,staging.handle,1,&copy);
    memory_barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
    impl_->submit_diagnostics();
    std::vector<Float4> result(static_cast<std::size_t>(bytes)/sizeof(Float4));
    void* mapped=nullptr;
    verify(vkMapMemory(impl_->device,staging.memory,0,bytes,0,&mapped),
           "map tangent diagnostic readback");
    std::memcpy(result.data(),mapped,static_cast<std::size_t>(bytes));
    vkUnmapMemory(impl_->device,staging.memory);
    return result;
}
std::vector<Float4> DeviceContext::diagnostic_read_root_history(const GroomState& g){
    if(!impl_||!g.impl_||g.impl_->context_id!=impl_.get())
        throw std::invalid_argument("diagnostic readback on wrong device context");
    const auto bytes=g.impl_->root_history_bytes;
    Buffer staging;
    staging.create(impl_->physical,impl_->device,bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const auto cb=impl_->begin_diagnostics();
    memory_barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy copy{};
    copy.size=bytes;
    vkCmdCopyBuffer(cb,g.impl_->root_previous_previous.handle,staging.handle,1,&copy);
    memory_barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
    impl_->submit_diagnostics();
    std::vector<Float4> result(static_cast<std::size_t>(bytes)/sizeof(Float4));
    void* mapped=nullptr;
    verify(vkMapMemory(impl_->device,staging.memory,0,bytes,0,&mapped),
           "map diagnostic readback");
    std::memcpy(result.data(),mapped,static_cast<std::size_t>(bytes));
    vkUnmapMemory(impl_->device,staging.memory);
    return result;
}
std::vector<StrandMotionData> DeviceContext::diagnostic_read_strand_data(const GroomState& g){
    if(!impl_||!g.impl_||g.impl_->context_id!=impl_.get())
        throw std::invalid_argument("diagnostic readback on wrong device context");
    const auto bytes=g.impl_->strand_bytes;
    Buffer staging;
    staging.create(impl_->physical,impl_->device,bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const auto cb=impl_->begin_diagnostics();
    memory_barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy copy{};
    copy.size=bytes;
    vkCmdCopyBuffer(cb,g.impl_->strand_data.handle,staging.handle,1,&copy);
    memory_barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
    impl_->submit_diagnostics();
    std::vector<StrandMotionData> result(static_cast<std::size_t>(bytes)/sizeof(StrandMotionData));
    void* mapped=nullptr;
    verify(vkMapMemory(impl_->device,staging.memory,0,bytes,0,&mapped),
           "map diagnostic readback");
    std::memcpy(result.data(),mapped,static_cast<std::size_t>(bytes));
    vkUnmapMemory(impl_->device,staging.memory);
    return result;
}
double DeviceContext::last_diagnostic_gpu_milliseconds()const noexcept{
    return impl_?impl_->last_ms:-1;
}
const std::string& DeviceContext::device_name()const noexcept{
    static const std::string empty;
    return impl_?impl_->name:empty;
}
bool DeviceContext::software_device()const noexcept{return impl_&&impl_->software;}
VkDevice DeviceContext::native_device()const noexcept{
    return impl_?impl_->device:VK_NULL_HANDLE;
}
std::uint64_t DeviceContext::gpu_submission_count()const noexcept{
    return impl_?impl_->submissions:0;
}
ExternalDevice DeviceContext::borrowed_handles()const noexcept{
    if(!impl_)return {};
    return {impl_->physical,impl_->device,impl_->queue,impl_->family};
}
} // namespace loxfx::vulkan::smoke
