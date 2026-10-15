// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
// INTERNAL native D3D12 stage-one runtime proof; not an SDK API.
#include "loxfx/d3d12/smoke_context.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <windows.h>

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace loxfx::d3d12::smoke {
namespace {
using Microsoft::WRL::ComPtr;

void checked(HRESULT hr, const char* operation) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(operation) + " (HRESULT " +
                                 std::to_string(static_cast<std::uint32_t>(hr)) + ")");
}

std::vector<char> shader_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open stage-one DXIL shader");
    auto n = input.tellg();
    if (n <= 0 || n > 8 * 1024 * 1024)
        throw std::runtime_error("invalid DXIL shader size");
    std::vector<char> bytes(static_cast<std::size_t>(n));
    input.seekg(0);
    if (!input.read(bytes.data(), n))
        throw std::runtime_error("cannot read DXIL shader");
    return bytes;
}

struct Constants {
    std::uint32_t vertices, followers, guides, reserved;
    float dt, gravity, damping, stiffness, range, pad0, pad1, pad2;
};
static_assert(sizeof(Constants) == 48);

void validate_parameters(const IntegrationParameters& p) {
    if (!std::isfinite(p.time_step) || p.time_step <= 0 || p.time_step > .1f ||
        !std::isfinite(p.gravity) || !std::isfinite(p.damping) || p.damping < 0 ||
        !std::isfinite(p.global_stiffness) || p.global_stiffness < 0 ||
        p.global_stiffness > 1 || !std::isfinite(p.global_range) ||
        p.global_range < 0 || p.global_range > 1)
        throw std::invalid_argument("invalid integration parameters");
}

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    return heap;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

ComPtr<ID3D12Resource> allocate_buffer(
    ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE type,
    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initial) {
    if (!bytes) throw std::invalid_argument("zero-sized D3D12 GPU buffer");
    ComPtr<ID3D12Resource> result;
    const auto heap = heap_properties(type);
    const auto desc = buffer_desc(bytes, flags);
    checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                    &desc, initial, nullptr, IID_PPV_ARGS(result.GetAddressOf())),
            "create D3D12 GPU buffer");
    return result;
}

void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &barrier);
}

void uav_barrier(ID3D12GraphicsCommandList* cmd) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = nullptr;  // Across all UAV resources.
    cmd->ResourceBarrier(1, &barrier);
}

struct ScopedEvent {
    HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ScopedEvent() {
        if (!handle) throw std::runtime_error("cannot create D3D12 fence event");
    }
    ~ScopedEvent() { if (handle) CloseHandle(handle); }
    ScopedEvent(const ScopedEvent&) = delete;
    ScopedEvent& operator=(const ScopedEvent&) = delete;
};

void wait_fence(ID3D12Fence* fence, UINT64 target) {
    if (fence->GetCompletedValue() >= target) return;
    ScopedEvent event;
    checked(fence->SetEventOnCompletion(target, event.handle),
            "set D3D12 fence completion event");
    if (WaitForSingleObject(event.handle, INFINITE) != WAIT_OBJECT_0)
        throw std::runtime_error("D3D12 fence wait failed");
}

std::string narrowed(const WCHAR* wide) {
    std::string name;
    while (*wide) {
        const WCHAR c = *wide++;
        name.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return name;
}
} // namespace

struct DeviceContext::Impl {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12CommandAllocator> diagnostic_allocator;
    ComPtr<ID3D12GraphicsCommandList> diagnostic_commands;
    ComPtr<ID3D12Fence> diagnostic_fence;
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timestamp_readback;
    UINT64 next_fence_value = 0;
    UINT64 timestamp_frequency = 0;
    double last_ms = -1;
    std::uint64_t diagnostic_submissions = 0;
    bool diagnostic_active = false, software = false;
    D3D12_COMMAND_LIST_TYPE queue_type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    std::string name;

    void create_standalone(AdapterChoice choice) {
        ComPtr<IDXGIFactory4> factory;
        checked(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf())),
                "create DXGI factory");
        if (choice == AdapterChoice::warp) {
            checked(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.GetAddressOf())),
                    "select DXGI WARP adapter");
            software = true;
        } else {
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> candidate;
                if (factory->EnumAdapters1(i, candidate.GetAddressOf()) == DXGI_ERROR_NOT_FOUND)
                    break;
                DXGI_ADAPTER_DESC1 desc{};
                checked(candidate->GetDesc1(&desc), "inspect DXGI adapter");
                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                ComPtr<ID3D12Device> probe;
                if (SUCCEEDED(D3D12CreateDevice(candidate.Get(),
                                D3D_FEATURE_LEVEL_12_0,
                                IID_PPV_ARGS(probe.GetAddressOf())))) {
                    D3D12_FEATURE_DATA_SHADER_MODEL shader_model{};
                    shader_model.HighestShaderModel = D3D_SHADER_MODEL_6_6;
                    if (SUCCEEDED(probe->CheckFeatureSupport(
                            D3D12_FEATURE_SHADER_MODEL,
                            &shader_model, sizeof(shader_model))) &&
                        shader_model.HighestShaderModel >= D3D_SHADER_MODEL_6_6) {
                        adapter = std::move(candidate);
                        device = std::move(probe);
                        break;
                    }
                }
            }
            if (!device) throw std::runtime_error("no D3D12 hardware adapter; "
                                                 "pass AdapterChoice::warp explicitly for diagnostic execution");
        }
        if (!device)
            checked(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                          IID_PPV_ARGS(device.GetAddressOf())), "create D3D12 device");
        DXGI_ADAPTER_DESC1 desc{};
        checked(adapter->GetDesc1(&desc), "read DXGI adapter identity");
        name = narrowed(desc.Description);
        create_queue();
    }

    void configure(ExternalDevice external) {
        if (!external.device || !external.compute_queue)
            throw std::invalid_argument("null borrowed D3D12 device/queue");
        device = external.device; // AddRef, never destroy host's own reference.
        queue = external.compute_queue;
        const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
        if (desc.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
            desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            throw std::invalid_argument("borrowed D3D12 queue cannot execute compute");
        // The diagnostic command-list TYPE must match the host queue.
        // The simulation recorder itself accepts either compute or direct lists.
        queue_type = desc.Type;
        name = "externally selected D3D12 adapter";
    }

    void create_queue() {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        queue_type = desc.Type;
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        ComPtr<ID3D12CommandQueue> created;
        checked(device->CreateCommandQueue(&desc,IID_PPV_ARGS(created.GetAddressOf())),
                "create native D3D12 compute queue");
        queue=std::move(created);
    }

    void init(const std::filesystem::path& path) {
        D3D12_FEATURE_DATA_SHADER_MODEL model{};
        model.HighestShaderModel = D3D_SHADER_MODEL_6_6;
        checked(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
                                             &model, sizeof(model)),
                "query D3D12 Shader Model capability");
        if (model.HighestShaderModel < D3D_SHADER_MODEL_6_6)
            throw std::runtime_error("D3D12 device does not support Shader Model 6.6");

        std::array<D3D12_ROOT_PARAMETER,4> roots{};
        for (UINT i=0;i<2;++i) {
            roots[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
            roots[i].Descriptor.ShaderRegister=i;
            roots[i].Descriptor.RegisterSpace=0;
            roots[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        }
        roots[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
        roots[2].Descriptor.ShaderRegister=2;
        roots[2].Descriptor.RegisterSpace=0;
        roots[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        roots[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        roots[3].Constants.ShaderRegister=0;
        roots[3].Constants.RegisterSpace=0;
        roots[3].Constants.Num32BitValues=sizeof(Constants)/4;
        roots[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters=static_cast<UINT>(roots.size());
        signature.pParameters=roots.data();
        signature.Flags=D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> signature_blob,errors;
        const HRESULT compiled=D3D12SerializeRootSignature(
            &signature,D3D_ROOT_SIGNATURE_VERSION_1,
            signature_blob.GetAddressOf(),errors.GetAddressOf());
        if (FAILED(compiled) && errors)
            throw std::runtime_error(std::string("D3D12 root signature: ") +
                static_cast<const char*>(errors->GetBufferPointer()));
        checked(compiled,"serialize D3D12 root signature");
        checked(device->CreateRootSignature(0,signature_blob->GetBufferPointer(),
                          signature_blob->GetBufferSize(),
                          IID_PPV_ARGS(root_signature.GetAddressOf())),
                "create D3D12 root signature");

        const auto bytecode=shader_file(path);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature=root_signature.Get();
        pso.CS.pShaderBytecode=bytecode.data();
        pso.CS.BytecodeLength=bytecode.size();
        checked(device->CreateComputePipelineState(&pso,
                        IID_PPV_ARGS(pipeline.GetAddressOf())),
                "create native DXIL compute pipeline");

        checked(device->CreateCommandAllocator(queue_type,
                        IID_PPV_ARGS(diagnostic_allocator.GetAddressOf())),
                "create diagnostic command allocator");
        checked(device->CreateCommandList(0,queue_type,
                        diagnostic_allocator.Get(),nullptr,
                        IID_PPV_ARGS(diagnostic_commands.GetAddressOf())),
                "create diagnostic command list");
        checked(diagnostic_commands->Close(),"close initial diagnostic list");
        checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,
                        IID_PPV_ARGS(diagnostic_fence.GetAddressOf())),
                "create diagnostic fence");

        if (SUCCEEDED(queue->GetTimestampFrequency(&timestamp_frequency)) &&
            timestamp_frequency != 0) {
            D3D12_QUERY_HEAP_DESC q{};
            q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            q.Count=2;
            checked(device->CreateQueryHeap(&q,
                        IID_PPV_ARGS(timestamps.GetAddressOf())),
                    "create timestamp query heap");
            timestamp_readback=allocate_buffer(device.Get(),2*sizeof(UINT64),
                D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }

    ID3D12GraphicsCommandList* begin() {
        if(diagnostic_active)throw std::logic_error("diagnostic list already recording");
        checked(diagnostic_allocator->Reset(),"reset diagnostic allocator");
        checked(diagnostic_commands->Reset(diagnostic_allocator.Get(),nullptr),
                "reset diagnostic command list");
        diagnostic_active=true;
        if(timestamps)
            diagnostic_commands->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        return diagnostic_commands.Get();
    }

    void submit() {
        if(!diagnostic_active)throw std::logic_error("no diagnostic recording active");
        if(timestamps) {
            diagnostic_commands->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
            diagnostic_commands->ResolveQueryData(timestamps.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,0,2,timestamp_readback.Get(),0);
        }
        checked(diagnostic_commands->Close(),"close diagnostic command list");
        diagnostic_active=false;
        ID3D12CommandList* lists[]={diagnostic_commands.Get()};
        queue->ExecuteCommandLists(1,lists);
        ++diagnostic_submissions;
        checked(queue->Signal(diagnostic_fence.Get(),++next_fence_value),
                "signal diagnostic completion");
        wait_fence(diagnostic_fence.Get(),next_fence_value);

        last_ms=-1;
        if (timestamps) {
            void* mapped=nullptr;
            D3D12_RANGE range{0,2*sizeof(UINT64)};
            checked(timestamp_readback->Map(0,&range,&mapped),"map D3D12 timestamps");
            std::array<UINT64,2> values{};
            std::memcpy(values.data(),mapped,sizeof(values));
            D3D12_RANGE write_range{0,0};
            timestamp_readback->Unmap(0,&write_range);
            last_ms=static_cast<double>(values[1]-values[0])*
                    1000.0/static_cast<double>(timestamp_frequency);
        }
    }
};

struct GroomState::Impl {
    std::shared_ptr<void> context_lifetime;
    const void* context_id = nullptr;
    ComPtr<ID3D12Resource> current, previous, rest;
    std::uint32_t vertices=0, followers=0, guides=0, groups=0;
    std::uint64_t bytes=0;
};
GroomState::GroomState(std::unique_ptr<Impl> g) noexcept:impl_(std::move(g)){}
GroomState::~GroomState()=default;
GroomState::GroomState(GroomState&&) noexcept=default;
GroomState& GroomState::operator=(GroomState&&) noexcept=default;
std::uint64_t GroomState::allocated_buffer_bytes()const noexcept{
    return impl_?3*impl_->bytes:0;
}

DeviceContext::DeviceContext(const std::filesystem::path& dxil,AdapterChoice preference)
:impl_(std::make_shared<Impl>()) {
    impl_->create_standalone(preference);
    impl_->init(dxil);
}
DeviceContext::DeviceContext(ExternalDevice host,const std::filesystem::path& dxil)
:impl_(std::make_shared<Impl>()) {
    impl_->configure(host);
    impl_->init(dxil);
}
DeviceContext::~DeviceContext()=default;
DeviceContext::DeviceContext(DeviceContext&&) noexcept=default;
DeviceContext& DeviceContext::operator=(DeviceContext&&) noexcept=default;

GroomState DeviceContext::prepare_diagnostic(const PreparedGroom& source){
    if(!impl_)throw std::logic_error("device context was moved");
    const auto validation=validate_prepared_groom(source);
    if(!validation)throw std::invalid_argument(validation.error());
    auto g=std::make_unique<GroomState::Impl>();
    g->context_lifetime=impl_;
    g->context_id=impl_.get();
    g->vertices=source.vertices_per_strand;
    g->followers=source.followers_per_guide;
    g->guides=source.work.packed_guide_strands;
    g->groups=source.work.stages[0].thread_groups;
    g->bytes=static_cast<UINT64>(source.rest_positions.size())*sizeof(Float4);
    if(!g->bytes)throw std::invalid_argument("empty prepared hair buffers");

    g->current=allocate_buffer(impl_->device.Get(),g->bytes,D3D12_HEAP_TYPE_DEFAULT,
              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
    g->previous=allocate_buffer(impl_->device.Get(),g->bytes,D3D12_HEAP_TYPE_DEFAULT,
              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
    g->rest=allocate_buffer(impl_->device.Get(),g->bytes,D3D12_HEAP_TYPE_DEFAULT,
              D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
    auto upload=allocate_buffer(impl_->device.Get(),g->bytes,D3D12_HEAP_TYPE_UPLOAD,
              D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped=nullptr;
    D3D12_RANGE none{0,0};
    checked(upload->Map(0,&none,&mapped),"map one-time D3D12 upload");
    std::memcpy(mapped,source.rest_positions.data(),static_cast<std::size_t>(g->bytes));
    upload->Unmap(0,nullptr);

    auto* cmd=impl_->begin();
    for(ID3D12Resource* target:{g->current.Get(),g->previous.Get(),g->rest.Get()})
        cmd->CopyBufferRegion(target,0,upload.Get(),0,g->bytes);
    transition(cmd,g->current.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cmd,g->previous.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cmd,g->rest.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    impl_->submit(); // Diagnostic upload fence only; staging then released.
    return GroomState(std::move(g));
}

void DeviceContext::record_integration(ID3D12GraphicsCommandList* cmd,
             const GroomState& groom,const IntegrationParameters& p,
             std::uint32_t steps)const{
    if(!impl_||!cmd||!groom.impl_||
       groom.impl_->context_id!=impl_.get())
        throw std::invalid_argument("D3D12 groom/command context mismatch");
    validate_parameters(p);
    if(steps==0||steps>4096)throw std::invalid_argument("integration steps out of range");
    const auto& g=*groom.impl_;
    Constants c{g.vertices,g.followers,g.guides,0,p.time_step,p.gravity,
                p.damping,p.global_stiffness,p.global_range,0,0,0};
    // Exactly the same source-derived stage maths as Vulkan. Resource and
    // root bindings are backend-native; no descriptors/heap for this stage.
    uav_barrier(cmd);
    cmd->SetComputeRootSignature(impl_->root_signature.Get());
    cmd->SetPipelineState(impl_->pipeline.Get());
    cmd->SetComputeRootUnorderedAccessView(0,g.current->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(1,g.previous->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(2,g.rest->GetGPUVirtualAddress());
    cmd->SetComputeRoot32BitConstants(3,sizeof(Constants)/4,&c,0);
    for (std::uint32_t i=0;i<steps;++i) {
        cmd->Dispatch(g.groups,1,1);
        if(i+1<steps)uav_barrier(cmd);
    }
}

ID3D12GraphicsCommandList* DeviceContext::begin_diagnostic_commands(){
    if(!impl_)throw std::logic_error("device context was moved");
    return impl_->begin();
}
void DeviceContext::submit_diagnostic_and_wait(){
    if(!impl_)throw std::logic_error("device context was moved");
    impl_->submit();
}
std::vector<Float4> DeviceContext::diagnostic_read_positions(const GroomState& state){
    if(!impl_||!state.impl_||state.impl_->context_id!=impl_.get())
        throw std::invalid_argument("readback from unrelated D3D12 context");
    const auto& g=*state.impl_;
    auto readback=allocate_buffer(impl_->device.Get(),g.bytes,
                    D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
    auto* cmd=impl_->begin();
    transition(cmd,g.current.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(readback.Get(),0,g.current.Get(),0,g.bytes);
    transition(cmd,g.current.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    impl_->submit();
    std::vector<Float4> result(static_cast<std::size_t>(g.bytes)/sizeof(Float4));
    void* mapped=nullptr;
    D3D12_RANGE reading{0,static_cast<SIZE_T>(g.bytes)};
    checked(readback->Map(0,&reading,&mapped),"map D3D12 position readback");
    std::memcpy(result.data(),mapped,static_cast<std::size_t>(g.bytes));
    D3D12_RANGE none{0,0};
    readback->Unmap(0,&none);
    return result;
}
double DeviceContext::last_diagnostic_gpu_milliseconds()const noexcept{
    return impl_?impl_->last_ms:-1;
}
std::uint64_t DeviceContext::diagnostic_submission_count()const noexcept{
    return impl_?impl_->diagnostic_submissions:0;
}
ID3D12Device* DeviceContext::native_device()const noexcept{
    return impl_?impl_->device.Get():nullptr;
}
ExternalDevice DeviceContext::borrowed_handles()const noexcept{
    return impl_?ExternalDevice{impl_->device.Get(),impl_->queue.Get()}:ExternalDevice{};
}
const std::string& DeviceContext::adapter_name()const noexcept{
    static const std::string empty;
    return impl_?impl_->name:empty;
}
bool DeviceContext::software_adapter()const noexcept{
    return impl_&&impl_->software;
}

} // namespace loxfx::d3d12::smoke
