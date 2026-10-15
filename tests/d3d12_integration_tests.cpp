// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
// Native D3D12 WARP numerical and host ownership contract smoke.
#include "loxfx/d3d12/smoke_context.h"

#include <wrl/client.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Microsoft::WRL::ComPtr;
namespace gpu=loxfx::d3d12::smoke;

void require(bool ok,const char* msg) {
    if (!ok) throw std::runtime_error(msg);
}
void hrcheck(HRESULT hr,const char* msg) {
    if (FAILED(hr)) throw std::runtime_error(msg);
}
loxfx::GroomAsset fixture(float base, std::uint32_t guides) {
    loxfx::GroomAsset result;
    result.guide_strands=guides;
    result.vertices_per_strand=4;
    for(std::uint32_t i=0;i<guides;++i)
        for(std::uint32_t j=0;j<4;++j)
            result.positions.push_back({
                static_cast<float>(i),base+static_cast<float>(j),0,
                j==0?0.0f:1.0f});
    return result;
}

// A deliberately independent host command allocator, list, queue signal and
// fence. The simulator never sees the allocator, fence, or host wait.
void submit_host_commands(gpu::DeviceContext& context,
                          const gpu::GroomState& groom) {
    const auto external=context.borrowed_handles();
    ComPtr<ID3D12CommandAllocator> allocator;
    hrcheck(external.device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_COMPUTE,
                IID_PPV_ARGS(allocator.GetAddressOf())),
            "host command allocator");
    ComPtr<ID3D12GraphicsCommandList> commands;
    hrcheck(external.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_COMPUTE,
                allocator.Get(),nullptr,IID_PPV_ARGS(commands.GetAddressOf())),
            "host command list");
    const auto submissions=context.diagnostic_submission_count();
    constexpr float dt=1.0f/60.0f;
    context.record_integration(commands.Get(),groom,
                {.time_step=dt,.gravity=9.81f,.damping=0},1);
    require(context.diagnostic_submission_count()==submissions,
            "external recording must not submit internal work");
    hrcheck(commands->Close(),"host close command list");
    ID3D12CommandList* submitted[]={commands.Get()};
    external.compute_queue->ExecuteCommandLists(1,submitted);
    ComPtr<ID3D12Fence> fence;
    hrcheck(external.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(fence.GetAddressOf())),"host fence");
    hrcheck(external.compute_queue->Signal(fence.Get(),1),"host queue signal");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    require(event!=nullptr,"host completion event");
    if (fence->GetCompletedValue()<1) {
        const HRESULT set=fence->SetEventOnCompletion(1,event);
        if (FAILED(set)) { CloseHandle(event); hrcheck(set,"host fence event"); }
        const DWORD result=WaitForSingleObject(event,INFINITE);
        CloseHandle(event);
        require(result==WAIT_OBJECT_0,"host waited for queued work");
    } else CloseHandle(event);
    require(context.diagnostic_submission_count()==submissions,
            "host submission cannot change SDK diagnostic counter");
}
}

int main(int argc,char** argv) {
    if(argc!=3)return 2;
    try {
        auto first=loxfx::prepare_groom(fixture(0,1),{.followers_per_guide=1});
        auto second=loxfx::prepare_groom(fixture(5,2),{.followers_per_guide=2});
        require(first&&second,"groom preparation");
        gpu::DeviceContext context(argv[1],gpu::AdapterChoice::warp);
        require(context.native_device()!=nullptr,"native D3D12 WARP device");
        auto g1=context.prepare_diagnostic(*first);
        auto g2=context.prepare_diagnostic(*second);
        require(g1.allocated_buffer_bytes()>=3*first->work.packed_total_vertices*
                    sizeof(loxfx::Float4) &&
                g2.allocated_buffer_bytes()>=3*second->work.packed_total_vertices*
                    sizeof(loxfx::Float4),
                "committed D3D12 allocations include resource alignment");
        const auto initial1=context.diagnostic_read_positions(g1);
        const auto initial2=context.diagnostic_read_positions(g2);
        require(initial1[1].y==1.0f&&initial2[1].y==6.0f,
                "initial D3D12 uploads/readback");
        require(initial1[5].y==1.0f&&initial2[5].y==6.0f,
                "follower copies initially untouched");

        constexpr float dt=1.0f/60.0f,g=9.81f;
        const auto previous=context.diagnostic_submission_count();
        auto* commands=context.begin_diagnostic_commands();
        context.record_integration(commands,g1,
            {.time_step=dt,.gravity=g,.damping=0},3);
        context.record_integration(commands,g2,
            {.time_step=dt,.gravity=g,.damping=0},2);
        require(context.diagnostic_submission_count()==previous,
                "recording two grooms performs zero queue submissions");
        context.submit_diagnostic_and_wait();
        require(context.diagnostic_submission_count()==previous+1,
                "single explicit D3D12 queue submission");
        const double gpu_ms=context.last_diagnostic_gpu_milliseconds();
        auto after1=context.diagnostic_read_positions(g1);
        auto after2=context.diagnostic_read_positions(g2);
        const float delta=g*dt*dt;
        require(std::abs(after1[1].y-(1.0f-6*delta))<0.0001f,
                "D3D12 three-step Verlet equals Vulkan fixture");
        require(std::abs(after2[1].y-(6.0f-3*delta))<0.0001f,
                "D3D12 second groom stays independent");
        require(after1[0].y==0.0f&&after2[0].y==5.0f,
                "pinned roots");
        require(after1[5].y==1.0f&&after2[5].y==6.0f,
                "follower publication not yet implemented");

        auto* next=context.begin_diagnostic_commands();
        context.record_integration(next,g1,
            {.time_step=dt,.gravity=g,.damping=0},1);
        context.record_integration(next,g2,
            {.time_step=dt,.gravity=g,.damping=0,
             .global_stiffness=1.0f,.global_range=1.0f},1);
        context.submit_diagnostic_and_wait();
        auto persisted=context.diagnostic_read_positions(g1);
        auto recovered=context.diagnostic_read_positions(g2);
        require(std::abs(persisted[1].y-(1.0f-10*delta))<0.0001f,
                "D3D12 history survives separate GPU submissions");
        require(std::abs(recovered[1].y-6.0f)<0.0001f,
                "D3D12 rest-shape restoration");

        // Borrowed handles AddRef but do not take over the host queue.
        {
            gpu::DeviceContext borrowed(context.borrowed_handles(),argv[1]);
            require(borrowed.native_device()==context.native_device(),
                    "borrowing must not create a new device");
            auto third=borrowed.prepare_diagnostic(*first);
            auto* list=borrowed.begin_diagnostic_commands();
            borrowed.record_integration(list,third,
                {.time_step=dt,.gravity=g,.damping=0},1);
            borrowed.submit_diagnostic_and_wait();
            auto result=borrowed.diagnostic_read_positions(third);
            require(std::abs(result[1].y-(1.0f-delta))<0.0001f,
                    "native compute through borrowed D3D12 context");
        }

        // Regress Codex P2 on merged PR #14: a queue from a *different*
        // device is not valid even if that device is the same WARP adapter.
        {
            ComPtr<IDXGIFactory4> factory;
            hrcheck(CreateDXGIFactory2(0,IID_PPV_ARGS(factory.GetAddressOf())),
                    "create alternate DXGI factory");
            ComPtr<IDXGIAdapter> warp_adapter;
            hrcheck(factory->EnumWarpAdapter(IID_PPV_ARGS(warp_adapter.GetAddressOf())),
                    "select alternate WARP adapter");
            ComPtr<ID3D12Device> other_device;
            hrcheck(D3D12CreateDevice(warp_adapter.Get(),D3D_FEATURE_LEVEL_12_0,
                        IID_PPV_ARGS(other_device.GetAddressOf())),
                    "create independent WARP device");
            ComPtr<IUnknown> primary_id, alternate_id;
            hrcheck(context.native_device()->QueryInterface(
                        IID_PPV_ARGS(primary_id.GetAddressOf())),
                    "inspect primary COM identity");
            hrcheck(other_device->QueryInterface(
                        IID_PPV_ARGS(alternate_id.GetAddressOf())),
                    "inspect alternate COM identity");
            // D3D12CreateDevice may return a cached device COM identity for
            // repeated calls on one adapter (notably WARP). Try other adapters
            // rather than pretending those are distinct devices.
            if (primary_id.Get()==alternate_id.Get()) {
                for(UINT index=0;;++index){
                    ComPtr<IDXGIAdapter1> candidate;
                    if(factory->EnumAdapters1(index,candidate.GetAddressOf())==
                       DXGI_ERROR_NOT_FOUND)break;
                    DXGI_ADAPTER_DESC1 desc{};
                    if(FAILED(candidate->GetDesc1(&desc)) ||
                       (desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))continue;
                    ComPtr<ID3D12Device> probe;
                    if(FAILED(D3D12CreateDevice(candidate.Get(),D3D_FEATURE_LEVEL_12_0,
                            IID_PPV_ARGS(probe.GetAddressOf()))))continue;
                    ComPtr<IUnknown> identity;
                    hrcheck(probe->QueryInterface(IID_PPV_ARGS(identity.GetAddressOf())),
                            "inspect alternate hardware device COM identity");
                    if(identity.Get()!=primary_id.Get()){
                        other_device=std::move(probe);
                        alternate_id=std::move(identity);
                        break;
                    }
                }
            }
            if(primary_id.Get()!=alternate_id.Get()){
                D3D12_COMMAND_QUEUE_DESC queue_desc{};
                queue_desc.Type=D3D12_COMMAND_LIST_TYPE_COMPUTE;
                ComPtr<ID3D12CommandQueue> wrong_queue;
                hrcheck(other_device->CreateCommandQueue(&queue_desc,
                            IID_PPV_ARGS(wrong_queue.GetAddressOf())),
                        "create wrong-device command queue");
                bool rejected=false;
                try {
                    gpu::DeviceContext invalid(
                        {context.native_device(),wrong_queue.Get()},argv[1]);
                } catch (const std::invalid_argument&) {
                    rejected=true;
                }
                require(rejected,"reject queue from a different D3D12 device");
            } else {
                std::cout << "Cross-device negative check skipped: only one "
                             "D3D12 device COM identity available on runner\\n";
            }
        }

        // A borrowed DIRECT queue must use DIRECT command allocators/lists
        // even though the shader work itself is compute-only.
        {
            D3D12_COMMAND_QUEUE_DESC queue_desc{};
            queue_desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
            ComPtr<ID3D12CommandQueue> direct;
            hrcheck(context.native_device()->CreateCommandQueue(
                        &queue_desc,IID_PPV_ARGS(direct.GetAddressOf())),
                    "create borrowed direct queue");
            gpu::DeviceContext borrowed_direct(
                {context.native_device(),direct.Get()},argv[1]);
            auto direct_groom=borrowed_direct.prepare_diagnostic(*first);
            auto* direct_commands=borrowed_direct.begin_diagnostic_commands();
            borrowed_direct.record_integration(direct_commands,direct_groom,
                {.time_step=dt,.gravity=g,.damping=0},1);
            borrowed_direct.submit_diagnostic_and_wait();
            auto direct_positions=borrowed_direct.diagnostic_read_positions(direct_groom);
            require(std::abs(direct_positions[1].y-(1.0f-delta))<0.0001f,
                    "borrowed direct queue executes compute with matching list type");
        }

        submit_host_commands(context,g1);
        auto host_result=context.diagnostic_read_positions(g1);
        require(std::abs(host_result[1].y-(1.0f-15*delta))<0.0001f,
                "externally allocated command list advances GPU history");

        // The same moving-root test as Vulkan. Compact root history is
        // two Float4 values per padded guide, never a full follower history.
        const auto history_before=context.diagnostic_read_root_history(g1);
        require(history_before.size()==first->work.packed_guide_strands*2,
                "compact root history allocation");
        std::vector<loxfx::Float4> positions_after_first_motion;
        for(float translation : {0.5f,0.9f,1.2f}) {
            auto* motion=context.begin_diagnostic_commands();
            context.record_integration(motion,g1,
                {.time_step=dt,.gravity=g,.damping=0,
                 .root_translation_x=translation},1);
            context.submit_diagnostic_and_wait();
            const auto current=context.diagnostic_read_positions(g1);
            if (translation==0.5f) positions_after_first_motion=current;
            require(std::abs(current[0].x-translation)<0.0001f,
                    "D3D12 root follows host attachment offset");
            require(current[4].x==0.0f,
                    "D3D12 follower publication remains separate");
        }
        const auto root_history=context.diagnostic_read_root_history(g1);
        require(std::abs(root_history[0].x-0.5f)<0.0001f,
                "D3D12 previous-previous roots match Vulkan history");
        require(std::abs(root_history[1].y-positions_after_first_motion[1].y)<0.0001f,
                "D3D12 dynamic second-root history matches older simulation state");
        require(context.diagnostic_read_positions(g2)[0].y==5.0f,
                "D3D12 other groom roots stay independent");

        // Both proximal vertices attached: the reference strand-motion
        // solver reads their rotation/translation basis across frames.
        auto attached=fixture(0.0f,1);
        attached.positions[1].w=0.0f;
        auto attached_prepared=loxfx::prepare_groom(
            attached,{.followers_per_guide=1});
        require(attached_prepared.has_value(),"prepare attached-root D3D12 fixture");
        auto attached_gpu=context.prepare_diagnostic(*attached_prepared);
        for(float displacement : {0.25f,0.75f,1.25f}) {
            auto* motion=context.begin_diagnostic_commands();
            context.record_integration(motion,attached_gpu,
                {.time_step=dt,.gravity=g,.damping=0,
                 .root_translation_x=displacement},1);
            context.submit_diagnostic_and_wait();
            const auto pos=context.diagnostic_read_positions(attached_gpu);
            require(std::abs(pos[0].x-displacement)<0.0001f &&
                    std::abs(pos[1].x-displacement)<0.0001f &&
                    pos[0].y==0.0f && pos[1].y==1.0f,
                    "D3D12 attached proximal guide vertices translate rigidly");
        }
        const auto anchored_history=context.diagnostic_read_root_history(attached_gpu);
        require(std::abs(anchored_history[0].x-0.25f)<0.0001f &&
                std::abs(anchored_history[1].x-0.25f)<0.0001f,
                "D3D12 compact history preserves both older attachment positions");

        auto* stage2=context.begin_diagnostic_commands();
        context.record_integration(stage2,attached_gpu,
            {.time_step=dt,.gravity=g,.damping=0,.root_translation_x=1.5f},1);
        context.record_strand_level(stage2,attached_gpu,
            {.velocity_shock_coefficient=0.25f,.acceleration_threshold=0.1f});
        context.submit_diagnostic_and_wait();
        const auto strand=context.diagnostic_read_strand_data(attached_gpu);
        require(strand.size()==attached_prepared->work.packed_guide_strands,
                "D3D12 compact guide motion records");
        require(std::abs(strand[0].vsp_translation_coefficient.x-0.25f)<0.0001f &&
                std::abs(strand[0].vsp_translation_coefficient.w-1.0f)<0.0001f,
                "D3D12 motion translation and accelerated VSP coefficient");
        require(std::abs(strand[0].vsp_quaternion.w-1.0f)<0.0001f &&
                std::abs(strand[0].skinning_quaternion.w-1.0f)<0.0001f,
                "D3D12 identity quaternion on translating scalp");
        auto* again=context.begin_diagnostic_commands();
        context.record_strand_level(again,attached_gpu,
            {.velocity_shock_coefficient=0.35f,.acceleration_threshold=100.0f});
        context.submit_diagnostic_and_wait();
        const auto below=context.diagnostic_read_strand_data(attached_gpu);
        require(std::abs(below[0].vsp_translation_coefficient.w-0.35f)<0.0001f,
                "D3D12 ordinary VSP coefficient below threshold");
        // Stage-three translation and history tests match the Vulkan fixture.
        auto vsp_asset=fixture(0.0f,1);
        vsp_asset.positions[1].w=0.0f;
        auto vsp_prep=loxfx::prepare_groom(vsp_asset,{.followers_per_guide=1});
        require(vsp_prep.has_value(),"prepare VSP D3D12 fixture");
        auto vsp_gpu=context.prepare_diagnostic(*vsp_prep);
        auto* vsp_cmd=context.begin_diagnostic_commands();
        context.record_integration(vsp_cmd,vsp_gpu,
            {.time_step=dt,.gravity=0,.damping=0,.root_translation_x=1.0f},1);
        context.record_strand_level(vsp_cmd,vsp_gpu,
            {.velocity_shock_coefficient=0.2f,.acceleration_threshold=0.0f});
        context.record_velocity_shock(vsp_cmd,vsp_gpu);
        context.submit_diagnostic_and_wait();
        const auto full_vsp=context.diagnostic_read_positions(vsp_gpu);
        require(std::abs(full_vsp[0].x-1.0f)<0.0001f &&
                std::abs(full_vsp[1].x-1.0f)<0.0001f &&
                std::abs(full_vsp[2].x-1.0f)<0.0001f &&
                std::abs(full_vsp[3].x-1.0f)<0.0001f,
                "D3D12 VSP moves guide tips with accelerated scalp");
        require(full_vsp[2].w==1.0f && full_vsp[0].w==0.0f &&
                full_vsp[4].x==vsp_prep->rest_positions[4].x,
                "D3D12 VSP skips followers and preserves pinned flags");
        auto* vsp_next=context.begin_diagnostic_commands();
        context.record_integration(vsp_next,vsp_gpu,
            {.time_step=dt,.gravity=0,.damping=0,.root_translation_x=1.0f},1);
        context.submit_diagnostic_and_wait();
        const auto continued=context.diagnostic_read_positions(vsp_gpu);
        require(std::abs(continued[2].x-1.0f)<0.0001f &&
                std::abs(continued[3].x-1.0f)<0.0001f,
                "D3D12 VSP updates previous-position history");

        auto partial_gpu=context.prepare_diagnostic(*vsp_prep);
        auto* partial_cmd=context.begin_diagnostic_commands();
        context.record_integration(partial_cmd,partial_gpu,
            {.time_step=dt,.gravity=0,.damping=0,.root_translation_x=1.0f},1);
        context.record_strand_level(partial_cmd,partial_gpu,
            {.velocity_shock_coefficient=0.25f,.acceleration_threshold=100.0f});
        context.record_velocity_shock(partial_cmd,partial_gpu);
        context.submit_diagnostic_and_wait();
        const auto partial_vsp=context.diagnostic_read_positions(partial_gpu);
        require(std::abs(partial_vsp[2].x-0.25f)<0.0001f &&
                std::abs(partial_vsp[3].x-0.25f)<0.0001f &&
                std::abs(partial_vsp[0].x-1.0f)<0.0001f,
                "D3D12 VSP fractional propagation preserves the attached root");

        // Same nonidentity quaternion fixture as Vulkan: gravity rotates a
        // movable proximal segment from +X toward -Y.
        auto tilting=fixture(0.0f,1);
        tilting.positions[1].x=1.0f;
        tilting.positions[1].y=0.0f;
        auto tilted_prep=loxfx::prepare_groom(tilting,{.followers_per_guide=1});
        require(tilted_prep.has_value(),"prepare tilted D3D12 guide");
        auto tilted_gpu=context.prepare_diagnostic(*tilted_prep);
        auto* tilt_commands=context.begin_diagnostic_commands();
        context.record_integration(tilt_commands,tilted_gpu,
            {.time_step=dt,.gravity=500.0f,.damping=0},1);
        context.record_strand_level(tilt_commands,tilted_gpu,
            {.velocity_shock_coefficient=0.42f,.acceleration_threshold=100.0f});
        context.submit_diagnostic_and_wait();
        const auto tilt_data=context.diagnostic_read_strand_data(tilted_gpu);
        require(tilt_data[0].vsp_quaternion.z < -0.04f &&
                tilt_data[0].vsp_quaternion.z > -0.10f &&
                std::abs(tilt_data[0].vsp_translation_coefficient.w-0.42f)<0.0001f,
                "D3D12 quaternion rotates +X toward -Y without coefficient override");
        const auto before_vsp=context.diagnostic_read_positions(tilted_gpu);
        auto* tilt_vsp=context.begin_diagnostic_commands();
        context.record_velocity_shock(tilt_vsp,tilted_gpu);
        context.submit_diagnostic_and_wait();
        const auto after_vsp=context.diagnostic_read_positions(tilted_gpu);
        const auto qrot=tilt_data[0].vsp_quaternion;
        const auto translation=tilt_data[0].vsp_translation_coefficient;
        for(std::uint32_t v=2;v<4;++v) {
            const auto& input=before_vsp[v];
            const float rx=(1-2*qrot.z*qrot.z)*input.x-2*qrot.w*qrot.z*input.y;
            const float ry=2*qrot.w*qrot.z*input.x+(1-2*qrot.z*qrot.z)*input.y;
            const float expected_x=input.x+translation.w*(rx+translation.x-input.x);
            const float expected_y=input.y+translation.w*(ry+translation.y-input.y);
            require(std::abs(after_vsp[v].x-expected_x)<0.0002f &&
                    std::abs(after_vsp[v].y-expected_y)<0.0002f,
                    "D3D12 VSP applies quaternion, translation, and coefficient");
        }
        require(after_vsp[0].x==before_vsp[0].x &&
                after_vsp[1].x==before_vsp[1].x &&
                after_vsp[4].x==before_vsp[4].x,
                "D3D12 VSP skips proximal and follower positions");

        // Shared P2 regression fixture with Vulkan: near 180-degree
        // turns must preserve the real angle and rotate old onto new.
        for(float transverse : {0.008f,0.007f,-0.007f,0.000087f,0.0f}) {
            auto hair=fixture(0.0f,1);
            hair.positions[1].x=transverse;
            auto prepared=loxfx::prepare_groom(hair,{.followers_per_guide=1});
            require(prepared.has_value(),"prepare antiparallel D3D12 groom");
            auto gpu=context.prepare_diagnostic(*prepared);
            auto* command=context.begin_diagnostic_commands();
            context.record_integration(command,gpu,
                {.time_step=dt,.gravity=7200.0f,.damping=0.0f},1);
            context.record_strand_level(command,gpu,
                {.velocity_shock_coefficient=0.2f,.acceleration_threshold=100.0f});
            context.submit_diagnostic_and_wait();
            const auto motion=context.diagnostic_read_strand_data(gpu);
            const auto q=motion[0].vsp_quaternion;
            const float expected_w=std::abs(transverse)/std::sqrt(1.0f+transverse*transverse);
            require(std::isfinite(q.x)&&std::isfinite(q.y)&&
                    std::isfinite(q.z)&&std::isfinite(q.w),
                    "D3D12 near-antiparallel quaternion is finite");
            require(std::abs(q.w-expected_w)<0.00002f,
                    "D3D12 near-antiparallel quaternion retains its half-angle");
            const float old_x=transverse,old_y=1.0f;
            const float mapped_x=(1-2*(q.y*q.y+q.z*q.z))*old_x+
                                  2*(q.x*q.y-q.w*q.z)*old_y;
            const float mapped_y=2*(q.x*q.y+q.w*q.z)*old_x+
                                  (1-2*(q.x*q.x+q.z*q.z))*old_y;
            require(std::abs(mapped_x-transverse)<0.00005f &&
                    std::abs(mapped_y+1.0f)<0.00005f,
                    "D3D12 quaternion correctly maps old proximal direction onto new");
        }
        // Stage four: pinned guide roots and source-normalized local stiffness.
        // One dispatch is enough for an exact numeric straight-rest fixture.
        auto shape_asset=fixture(0.0f,1);
        shape_asset.positions[1].w=0.0f;
        for(std::uint32_t iterations : {1u,3u}) {
            auto shape_prepared=loxfx::prepare_groom(
                shape_asset,{.followers_per_guide=1,.local_shape_iterations=iterations});
            require(shape_prepared.has_value(),"prepare local shape fixture");
            auto shape_gpu=context.prepare_diagnostic(*shape_prepared);
            auto* shape_cmd=context.begin_diagnostic_commands();
            context.record_integration(shape_cmd,shape_gpu,
                {.time_step=dt,.gravity=720.0f,.damping=0.0f},1);
            context.record_strand_level(shape_cmd,shape_gpu,
                {.velocity_shock_coefficient=0.0f,.acceleration_threshold=100.0f});
            context.record_velocity_shock(shape_cmd,shape_gpu);
            context.record_local_shape(shape_cmd,shape_gpu,1.0f);
            context.submit_diagnostic_and_wait();
            const auto result=context.diagnostic_read_positions(shape_gpu);
            require(result[0].y==0.0f && result[1].y==1.0f &&
                      result[0].w==0.0f && result[1].w==0.0f,
                      "D3D12 local shape preserves attached vertices");
            require(std::abs(result[4].y-shape_prepared->rest_positions[4].y)<0.0001f,
                      "D3D12 local shape does not simulate followers");
            if(iterations==1u) {
                // Source uses 0.5*min(1,0.95)=0.475 stiffness.
                // Gravity displaces free vertices by 0.2. Sequential
                // corrections yield y2=1.849875 and y3=2.845125.
                require(std::abs(result[2].y-1.849875f)<0.0003f &&
                          std::abs(result[3].y-2.845125f)<0.0003f,
                          "D3D12 source local-shape correction and vertex order");
            } else {
                require(std::abs(result[2].y-1.849875f)>0.0005f,
                          "D3D12 repeated short-strand shape iterations execute");
            }
        }
        // The 64-vertex path uses one dispatch with an internal iteration
        // loop; requested extra iterations must not silently be discarded.
        loxfx::GroomAsset long_asset;
        long_asset.guide_strands=1;
        long_asset.vertices_per_strand=64;
        for(std::uint32_t v=0;v<64;++v)
            long_asset.positions.push_back({0.0f,static_cast<float>(v),0.0f,
                                            v<2u?0.0f:1.0f});
        float long_once=0.0f;
        for(std::uint32_t iterations : {1u,3u}) {
            auto long_prepared=loxfx::prepare_groom(
                long_asset,{.followers_per_guide=1,.local_shape_iterations=iterations});
            require(long_prepared.has_value(),"prepare long-strand fixture");
            auto long_gpu=context.prepare_diagnostic(*long_prepared);
            auto* cmd=context.begin_diagnostic_commands();
            context.record_integration(cmd,long_gpu,
                {.time_step=dt,.gravity=720.0f,.damping=0.0f},1);
            context.record_strand_level(cmd,long_gpu,
                {.velocity_shock_coefficient=0.0f,.acceleration_threshold=100.0f});
            context.record_velocity_shock(cmd,long_gpu);
            context.record_local_shape(cmd,long_gpu,1.0f);
            context.submit_diagnostic_and_wait();
            const auto result=context.diagnostic_read_positions(long_gpu);
            require(result[0].y==0.0f && result[1].y==1.0f &&
                      result[64].y==long_prepared->rest_positions[64].y,
                      "D3D12 64-vertex shape preserves roots and follower storage");
            require(std::isfinite(result[2].y) && std::isfinite(result[63].y),
                      "D3D12 long-strand local correction stays finite");
            if(iterations==1u) long_once=result[2].y;
            else require(std::abs(result[2].y-long_once)>0.0001f,
                           "D3D12 64-vertex internal iterations run");
        }

        // Length/wind stage: true guide-parallel odd/even constraints,
        // source rest lengths, pinned roots, and follower isolation.
        auto length_asset=fixture(0.0f,1);
        length_asset.positions[1].w=0.0f;
        auto prepared=loxfx::prepare_groom(length_asset,
            {.followers_per_guide=2,.local_shape_iterations=1});
        require(prepared.has_value(),"prepare length fixture");
        auto length_gpu=context.prepare_diagnostic(*prepared);
        auto* cmd=context.begin_diagnostic_commands();
        context.record_integration(cmd,length_gpu,
            {.time_step=dt,.gravity=720.0f,.damping=0.0f},1);
        context.record_strand_level(cmd,length_gpu,
            {.velocity_shock_coefficient=0.0f,.acceleration_threshold=100.0f});
        context.record_velocity_shock(cmd,length_gpu);
        context.record_length_wind(cmd,length_gpu,
            {.time_step=dt,.length_iterations=4});
        context.submit_diagnostic_and_wait();
        const auto length_result=context.diagnostic_read_positions(length_gpu);
        require(length_result[0].y==0.0f && length_result[1].y==1.0f,
                 "D3D12 length constraints preserve attached roots");
        require(std::abs(length_result[2].y-2.0f)<0.0003f &&
                 std::abs(length_result[3].y-2.975f)<0.0003f,
                 "D3D12 odd/even shared-memory length solver matches expected positions");
        require(length_result[4].y==prepared->rest_positions[4].y &&
                 length_result[8].y==prepared->rest_positions[8].y,
                 "D3D12 length pass excludes generated followers");

        // Source four-wind blend: at guide zero the two (1-a) winds
        // contribute, applying force only to movable interior vertices.
        auto wind_gpu=context.prepare_diagnostic(*prepared);
        auto* wind_cmd=context.begin_diagnostic_commands();
        loxfx::LengthWindParameters wind{};
        wind.time_step=dt;
        wind.length_iterations=0;
        wind.winds[1].x=360.0f;
        context.record_length_wind(wind_cmd,wind_gpu,wind);
        context.submit_diagnostic_and_wait();
        const auto wind_result=context.diagnostic_read_positions(wind_gpu);
        require(std::abs(wind_result[2].x+0.1f)<0.0003f &&
                 wind_result[3].x==0.0f && wind_result[1].x==0.0f,
                 "D3D12 four-wind blend moves only interior free vertices");

        // One strand per workgroup at 64 vertices: all group barriers must
        // remain convergent and outputs must be finite.
        loxfx::GroomAsset long_length;
        long_length.guide_strands=1;
        long_length.vertices_per_strand=64;
        for(std::uint32_t v=0;v<64;++v)
            long_length.positions.push_back({0.0f,static_cast<float>(v),0.0f,
                                             v<2u?0.0f:1.0f});
        auto long_prepared=loxfx::prepare_groom(long_length,{.followers_per_guide=1});
        require(long_prepared.has_value(),"prepare 64-vertex length fixture");
        auto long_gpu=context.prepare_diagnostic(*long_prepared);
        auto* long_cmd=context.begin_diagnostic_commands();
        context.record_integration(long_cmd,long_gpu,
            {.time_step=dt,.gravity=720.0f,.damping=0.0f},1);
        context.record_strand_level(long_cmd,long_gpu,
            {.velocity_shock_coefficient=0.0f,.acceleration_threshold=100.0f});
        context.record_velocity_shock(long_cmd,long_gpu);
        context.record_length_wind(long_cmd,long_gpu,
            {.time_step=dt,.length_iterations=3});
        context.submit_diagnostic_and_wait();
        const auto long_result=context.diagnostic_read_positions(long_gpu);
        require(long_result[0].y==0.0f && long_result[1].y==1.0f &&
                 std::isfinite(long_result[2].y) &&
                 std::isfinite(long_result[63].y) &&
                 long_result[64].y==long_prepared->rest_positions[64].y,
                 "D3D12 long strand length simulation and follower isolation");

        // Final output: animated guide positions plus exact source-prepared
        // per-follower offsets, including nonzero tip separation.
        auto follow_asset=fixture(0.0f,1);
        follow_asset.positions[1].w=0.0f;
        auto follow_prep=loxfx::prepare_groom(follow_asset,
            {.followers_per_guide=2,.follower_root_radius=0.35f,
             .follower_tip_separation=0.65f,.seed=17u});
        require(follow_prep.has_value(),"prepare follower publication");
        auto follow_gpu=context.prepare_diagnostic(*follow_prep);
        auto* follow_cmd=context.begin_diagnostic_commands();
        context.record_integration(follow_cmd,follow_gpu,
            {.time_step=dt,.gravity=720.0f,.damping=0.0f,
             .root_translation_x=1.0f},1);
        context.record_strand_level(follow_cmd,follow_gpu,
            {.velocity_shock_coefficient=0.0f,.acceleration_threshold=100.0f});
        context.record_velocity_shock(follow_cmd,follow_gpu);
        context.record_local_shape(follow_cmd,follow_gpu,0.6f);
        context.record_length_wind(follow_cmd,follow_gpu,
            {.time_step=dt,.length_iterations=4});
        context.record_followers(follow_cmd,follow_gpu);
        context.submit_diagnostic_and_wait();
        const auto published=context.diagnostic_read_positions(follow_gpu);
        const auto published_tangents=context.diagnostic_read_tangents(follow_gpu);
        require(published.size()==follow_prep->rest_positions.size() &&
                  published_tangents.size()==published.size(),
                  "D3D12 follower publication allocates full outputs");
        require(std::abs(published[2].x-follow_prep->rest_positions[2].x)>0.01f,
                  "D3D12 guide actually moves before publication");
        for(std::uint32_t v=0;v<4u;++v) {
            const auto& tangent=published_tangents[v];
            const float norm=std::sqrt(tangent.x*tangent.x+
                                       tangent.y*tangent.y+tangent.z*tangent.z);
            require(std::isfinite(norm) && std::abs(norm-1.0f)<0.0003f,
                      "D3D12 guide tangents normalized");
            for(std::uint32_t f=1u;f<=2u;++f) {
                const std::uint32_t index=f*4u+v;
                const auto& q=published[index];
                const auto& rest_f=follow_prep->rest_positions[index];
                const auto& rest_g=follow_prep->rest_positions[v];
                require(std::abs(q.x-(published[v].x+rest_f.x-rest_g.x))<0.0003f &&
                          std::abs(q.y-(published[v].y+rest_f.y-rest_g.y))<0.0003f &&
                          std::abs(q.z-(published[v].z+rest_f.z-rest_g.z))<0.0003f,
                          "D3D12 followers preserve prepared offsets including tip ease");
                require(std::abs(published_tangents[index].x-tangent.x)<0.0001f &&
                          std::abs(published_tangents[index].y-tangent.y)<0.0001f &&
                          std::abs(published_tangents[index].z-tangent.z)<0.0001f,
                          "D3D12 follower tangents copy guide final direction");
            }
        }
        auto* repeat_cmd=context.begin_diagnostic_commands();
        context.record_followers(repeat_cmd,follow_gpu);
        context.submit_diagnostic_and_wait();
        const auto repeated=context.diagnostic_read_positions(follow_gpu);
        for(std::uint32_t v=0;v<12u;++v)
            require(std::abs(repeated[v].x-published[v].x)<0.0001f &&
                      std::abs(repeated[v].y-published[v].y)<0.0001f,
                      "D3D12 repeated publication does not accumulate offsets");

        auto no_follow_prep=loxfx::prepare_groom(follow_asset);
        require(no_follow_prep.has_value(),"prepare guide-only publication");
        auto no_follow_gpu=context.prepare_diagnostic(*no_follow_prep);
        auto* no_follow_cmd=context.begin_diagnostic_commands();
        context.record_followers(no_follow_cmd,no_follow_gpu);
        context.submit_diagnostic_and_wait();
        const auto guide_only=context.diagnostic_read_tangents(no_follow_gpu);
        require(std::abs(guide_only[2].y-1.0f)<0.0001f,
                  "D3D12 guide-only output produces valid tangents");

        std::cout<<"D3D12 adapter: "<<context.adapter_name()
                 <<" (WARP; functionality only)"
                 <<"\nTwo-groom first-stage batch GPU ms: "<<gpu_ms
                 <<"\nPASS: DXIL native compute, independent histories,"
                   " borrowed context, host-owned command list\n";
    } catch(const std::exception& e) {
        std::cerr<<"FAIL: "<<e.what()<<"\n";
        return 1;
    }
    return 0;
}
