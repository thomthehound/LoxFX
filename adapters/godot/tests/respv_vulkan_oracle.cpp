// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
// Minimal Vulkan runtime oracle for int[8] set=0 binding=0 compute outputs.
// Vulkan 1.2, local_size_x=8, single workgroup; runs on Lavapipe.
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
static void need(VkResult r, const char *name) {
    if(r!=VK_SUCCESS) throw std::runtime_error(std::string(name)+" VkResult "+std::to_string(r));
}
int main(int argc,char**argv) {
  try {
    if(argc<2 || (argc-2)%2) {std::cerr<<"usage: vulkan_oracle module.spv [id value]...\n";return 2;}
    std::ifstream file(argv[1],std::ios::binary);
    std::vector<char> data{std::istreambuf_iterator<char>{file},{}};
    if(data.empty() || data.size()%4) throw std::runtime_error("Invalid input SPIR-V");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;
    VkInstance instance=VK_NULL_HANDLE;need(vkCreateInstance(&ic,nullptr,&instance),"instance");
    uint32_t count=0;need(vkEnumeratePhysicalDevices(instance,&count,nullptr),"devices");
    if(!count) throw std::runtime_error("No Vulkan physical device");
    std::vector<VkPhysicalDevice> gpus(count);
    need(vkEnumeratePhysicalDevices(instance,&count,gpus.data()),"device list");
    VkPhysicalDevice gpu=VK_NULL_HANDLE;uint32_t family=UINT32_MAX;
    for(auto p:gpus) {
      uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(p,&n,nullptr);
      std::vector<VkQueueFamilyProperties> q(n);vkGetPhysicalDeviceQueueFamilyProperties(p,&n,q.data());
      for(uint32_t k=0;k<n;++k) if(q[k].queueFlags&VK_QUEUE_COMPUTE_BIT){gpu=p;family=k;break;}
      if(gpu)break;
    }
    if(!gpu)throw std::runtime_error("No compute-capable queue");
    float priority=1.0f;VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qc.queueFamilyIndex=family;qc.queueCount=1;qc.pQueuePriorities=&priority;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;
    VkDevice device=VK_NULL_HANDLE;need(vkCreateDevice(gpu,&dc,nullptr,&device),"device");
    VkQueue queue=VK_NULL_HANDLE;vkGetDeviceQueue(device,family,0,&queue);
    constexpr VkDeviceSize length=sizeof(int32_t)*8;
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=length;
    bc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;bc.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer=VK_NULL_HANDLE;need(vkCreateBuffer(device,&bc,nullptr,&buffer),"buffer");
    VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,buffer,&req);
    VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(gpu,&props);
    uint32_t mt=UINT32_MAX;
    for(uint32_t i=0;i<props.memoryTypeCount;++i)
      if((req.memoryTypeBits&(1U<<i)) &&
         (props.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){mt=i;break;}
    if(mt==UINT32_MAX)throw std::runtime_error("No coherent host-visible memory");
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=req.size;ma.memoryTypeIndex=mt;
    VkDeviceMemory mem=VK_NULL_HANDLE;need(vkAllocateMemory(device,&ma,nullptr,&mem),"memory");
    need(vkBindBufferMemory(device,buffer,mem,0),"bind memory");
    void*mapped=nullptr;need(vkMapMemory(device,mem,0,length,0,&mapped),"map");std::memset(mapped,0xa5,length);
    VkDescriptorSetLayoutBinding bind{};bind.binding=0;bind.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bind.descriptorCount=1;bind.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo lc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lc.bindingCount=1;lc.pBindings=&bind;
    VkDescriptorSetLayout sl=VK_NULL_HANDLE;need(vkCreateDescriptorSetLayout(device,&lc,nullptr,&sl),"set layout");
    VkDescriptorPoolSize psz{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
    VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pc.maxSets=1;pc.poolSizeCount=1;pc.pPoolSizes=&psz;
    VkDescriptorPool pool=VK_NULL_HANDLE;need(vkCreateDescriptorPool(device,&pc,nullptr,&pool),"pool");
    VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sa.descriptorPool=pool;sa.descriptorSetCount=1;sa.pSetLayouts=&sl;
    VkDescriptorSet ds=VK_NULL_HANDLE;need(vkAllocateDescriptorSets(device,&sa,&ds),"set");
    VkDescriptorBufferInfo db{buffer,0,length};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};wr.dstSet=ds;
    wr.descriptorCount=1;wr.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;wr.pBufferInfo=&db;
    vkUpdateDescriptorSets(device,1,&wr,0,nullptr);
    VkShaderModuleCreateInfo sc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sc.codeSize=data.size();
    sc.pCode=reinterpret_cast<const uint32_t*>(data.data());
    VkShaderModule shader=VK_NULL_HANDLE;need(vkCreateShaderModule(device,&sc,nullptr,&shader),"shader module");
    VkPipelineLayoutCreateInfo plc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};plc.setLayoutCount=1;plc.pSetLayouts=&sl;
    VkPipelineLayout layout=VK_NULL_HANDLE;need(vkCreatePipelineLayout(device,&plc,nullptr,&layout),"pipeline layout");
    std::vector<VkSpecializationMapEntry> map;
    std::vector<uint32_t> vals;
    for(int i=2;i<argc;i+=2){
      map.push_back({uint32_t(std::stoul(argv[i])),uint32_t(vals.size()*4),4});
      vals.push_back(uint32_t(std::stoul(argv[i+1])));
    }
    VkSpecializationInfo specs{};specs.mapEntryCount=uint32_t(map.size());specs.pMapEntries=map.data();
    specs.dataSize=4*vals.size();specs.pData=vals.data();
    VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    st.stage=VK_SHADER_STAGE_COMPUTE_BIT;st.module=shader;st.pName="main";
    if(!map.empty())st.pSpecializationInfo=&specs;
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cp.stage=st;cp.layout=layout;
    VkPipeline pipeline=VK_NULL_HANDLE;need(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cp,nullptr,&pipeline),"pipeline");
    VkCommandPoolCreateInfo cpc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cpc.queueFamilyIndex=family;
    VkCommandPool cmdpool=VK_NULL_HANDLE;need(vkCreateCommandPool(device,&cpc,nullptr,&cmdpool),"command pool");
    VkCommandBufferAllocateInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb.commandPool=cmdpool;cb.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cb.commandBufferCount=1;
    VkCommandBuffer command=VK_NULL_HANDLE;need(vkAllocateCommandBuffers(device,&cb,&command),"command");
    VkCommandBufferBeginInfo start{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    need(vkBeginCommandBuffer(command,&start),"begin");
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&ds,0,nullptr);
    vkCmdDispatch(command,1,1,1);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    need(vkEndCommandBuffer(command),"end");
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};sub.commandBufferCount=1;sub.pCommandBuffers=&command;
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence=VK_NULL_HANDLE;
    need(vkCreateFence(device,&fc,nullptr,&fence),"fence");
    need(vkQueueSubmit(queue,1,&sub,fence),"submit");
    need(vkWaitForFences(device,1,&fence,VK_TRUE,15000000000ULL),"wait");
    auto values=reinterpret_cast<const int32_t*>(mapped);
    for(int i=0;i<8;++i)std::cout<<(i?",":"")<<values[i];
    std::cout<<"\n";
    vkDestroyFence(device,fence,nullptr);vkDestroyCommandPool(device,cmdpool,nullptr);
    vkDestroyPipeline(device,pipeline,nullptr);vkDestroyPipelineLayout(device,layout,nullptr);
    vkDestroyShaderModule(device,shader,nullptr);vkDestroyDescriptorPool(device,pool,nullptr);
    vkDestroyDescriptorSetLayout(device,sl,nullptr);vkUnmapMemory(device,mem);
    vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,mem,nullptr);
    vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
  }catch(const std::exception&e){std::cerr<<"VULKAN_ORACLE_ERROR: "<<e.what()<<"\n";return 1;}
  return 0;
}
