// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#include "loxfx/vulkan/smoke_context.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
void check(bool value, const char* what) {
    if (!value) throw std::runtime_error(what);
}
loxfx::GroomAsset fixture(float base, std::uint32_t strands) {
    loxfx::GroomAsset g;
    g.guide_strands = strands;
    g.vertices_per_strand = 4;
    for (std::uint32_t i=0; i<strands; ++i)
        for (std::uint32_t v=0; v<4; ++v)
            g.positions.push_back({static_cast<float>(i),base+static_cast<float>(v),
                                   0,v==0?0.0f:1.0f});
    return g;
}
// A host-controlled command pool and queue, wholly independent of the
// DeviceContext's optional diagnostic submission utilities.
struct HostBatch {
    VkDevice device=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    VkCommandBuffer commands=VK_NULL_HANDLE;
    explicit HostBatch(VkDevice logical):device(logical){}
    HostBatch(const HostBatch&)=delete;
    HostBatch& operator=(const HostBatch&)=delete;
    ~HostBatch() {
        if(fence)vkDestroyFence(device,fence,nullptr);
        if(pool)vkDestroyCommandPool(device,pool,nullptr);
    }
    void init(std::uint32_t queue_family) {
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex=queue_family;
        VkCommandPool temp_pool=VK_NULL_HANDLE;
        check(vkCreateCommandPool(device,&pool_info,nullptr,&temp_pool)==VK_SUCCESS,
              "host creates its own command pool");
        pool=temp_pool;
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool=pool;
        allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount=1;
        check(vkAllocateCommandBuffers(device,&allocate,&commands)==VK_SUCCESS,
              "host allocates its own command buffer");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence temp_fence=VK_NULL_HANDLE;
        check(vkCreateFence(device,&fence_info,nullptr,&temp_fence)==VK_SUCCESS,
              "host creates its own submission fence");
        fence=temp_fence;
    }
    void begin() {
        VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(commands,&begin_info)==VK_SUCCESS,
              "host begins external command buffer");
    }
    void submit(VkQueue queue) {
        check(vkEndCommandBuffer(commands)==VK_SUCCESS,
              "host ends external command buffer");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount=1;
        si.pCommandBuffers=&commands;
        check(vkQueueSubmit(queue,1,&si,fence)==VK_SUCCESS,
              "host submits external integration commands");
        check(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS,
              "host waits for its own submission");
    }
};
template<typename Fn>void reject(Fn&& fn, const char* what) {
    try { fn(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(what);
}
}

int main(int argc,char** argv) {
    if (argc != 3) return 2;
    try {
        namespace gpu = loxfx::vulkan::smoke;
        const auto a=loxfx::prepare_groom(fixture(0.0f,1),
                                           {.followers_per_guide=1});
        const auto b=loxfx::prepare_groom(fixture(5.0f,2),
                                           {.followers_per_guide=2});
        check(a&&b,"prepare independent grooms");
        // Context construction owns its Vulkan device/instance and all
        // invariant resources. Inject failures at representative creation
        // phases and confirm unwinding never poisons subsequent context use.
        for (int creation_point : {0, 2, 4, 6}) {
            bool caught=false;
            gpu::diagnostic_fail_creation_after(creation_point);
            try {
                gpu::DeviceContext rejected(argv[1]);
            } catch (const std::runtime_error& failure) {
                caught=std::string_view(failure.what()).find(
                    "injected Vulkan resource creation failure")!=std::string_view::npos;
            }
            gpu::diagnostic_fail_creation_after(-1);
            check(caught,"context creation failure injection was not triggered");
        }
        gpu::DeviceContext context(argv[1]);
        const VkDevice shared=context.native_device();
        check(shared!=VK_NULL_HANDLE,"standalone device created");
        const auto first=context.prepare(*a);
        const auto second=context.prepare(*b);
        check(context.native_device()==shared,"one device for two groom uploads");
        check(first.allocated_buffer_bytes()>0&&second.allocated_buffer_bytes()>0,
              "two independent persistent groom histories");
        const auto before_a=context.diagnostic_read_positions(first);
        const auto before_b=context.diagnostic_read_positions(second);
        check(before_a[1].y==1.0f && before_b[1].y==6.0f,
              "correct initial state for both grooms");

        auto mutated=*a;
        mutated.vertices_per_strand=65;
        reject([&]{ auto ignored=context.prepare(mutated); },
               "invalid topology was accepted");
        mutated=*a;
        mutated.work.stages[0].thread_groups+=1;
        reject([&]{ auto ignored=context.prepare(mutated); },
               "invalid dispatch work was accepted");
        mutated=*a;
        mutated.rest_positions[0].y=std::numeric_limits<float>::quiet_NaN();
        reject([&]{ auto ignored=context.prepare(mutated); },
               "invalid rest positions were accepted");

        constexpr float dt=1.0f/60.0f, gravity=9.81f;
        // Fault-inject every resource-creation checkpoint used by groom
        // initialization. Failed construction must release buffers/pools,
        // leave the shared device functional, and never submit partial work.
        const auto before_faults=context.gpu_submission_count();
        for (int point=0; point<15; ++point) {
            bool caught=false;
            gpu::diagnostic_fail_creation_after(point);
            try {
                auto invalid_groom=context.prepare(*a);
                (void)invalid_groom;
            } catch (const std::runtime_error& failure) {
                caught=std::string_view(failure.what()).find(
                    "injected Vulkan resource creation failure")!=std::string_view::npos;
            }
            gpu::diagnostic_fail_creation_after(-1);
            check(caught,"fault injection did not cover a groom allocation stage");
        }
        check(context.gpu_submission_count()==before_faults,
              "failed groom creations did not submit partial GPU commands");
        {
            auto recovered=context.prepare(*a);
            check(recovered.allocated_buffer_bytes()>0,
                  "shared device recovered after injection series");
            const auto data=context.diagnostic_read_positions(recovered);
            check(data[1].y==1.0f, "recovered groom upload correct");
        }

        const auto prior_submissions=context.gpu_submission_count();
        const VkCommandBuffer command=context.begin_diagnostic_commands();
        context.record_integration(command,first,
                                   {.time_step=dt,.gravity=gravity,.damping=0},3);
        context.record_integration(command,second,
                                   {.time_step=dt,.gravity=gravity,.damping=0},2);
        check(context.gpu_submission_count()==prior_submissions,
              "stage recording must NOT submit or CPU-wait");
        context.submit_diagnostic_and_wait();
        const double kernel_batch_ms=context.last_diagnostic_gpu_milliseconds();
        check(context.gpu_submission_count()==prior_submissions+1,
              "caller submitted both grooms as one batch");

        const auto after_a=context.diagnostic_read_positions(first);
        const auto after_b=context.diagnostic_read_positions(second);
        const float step=gravity*dt*dt;
        check(std::abs(after_a[1].y-(1.0f-6*step))<0.0001f,
              "first groom's three integration steps");
        check(std::abs(after_b[1].y-(6.0f-3*step))<0.0001f,
              "second groom's two independent integration steps");
        check(after_a[0].y==0.0f&&after_b[0].y==5.0f,
              "both root attachments pinned");
        check(after_a[5].y==1.0f&&after_b[5].y==6.0f,
              "follower publication is not implemented");
        // Verify previous state from one explicit batch is visible in another.
        const auto next=context.begin_diagnostic_commands();
        context.record_integration(next,first,
                                   {.time_step=dt,.gravity=gravity,.damping=0},1);
        context.record_integration(next,second,
                                   {.time_step=dt,.gravity=gravity,.damping=0,
                                    .global_stiffness=1.0f,.global_range=1.0f},1);
        context.submit_diagnostic_and_wait();
        const auto frame2_a=context.diagnostic_read_positions(first);
        const auto frame2_b=context.diagnostic_read_positions(second);
        check(std::abs(frame2_a[1].y-(1.0f-10*step))<0.0001f,
              "position history persisted across frame command batches");
        check(std::abs(frame2_b[1].y-6.0f)<0.0001f,
              "second groom rest-shape relaxation");

        // Borrowed context uses the same host GPU without taking ownership.
        // Its destruction must not release the host's physical/logical device.
        {
            gpu::DeviceContext borrowed(context.borrowed_handles(),argv[1]);
            check(borrowed.native_device()==shared,
                  "borrowed context preserves caller-owned Vulkan device");
            auto third=borrowed.prepare(*a);
            const auto recorded=borrowed.begin_diagnostic_commands();
            borrowed.record_integration(recorded,third,
                {.time_step=dt,.gravity=gravity,.damping=0},1);
            borrowed.submit_diagnostic_and_wait();
            const auto third_after=borrowed.diagnostic_read_positions(third);
            check(std::abs(third_after[1].y-(1.0f-step))<0.0001f,
                  "borrowed device performs real compute");
        }
        check(context.native_device()==shared,
              "borrowed context destruction leaves host device intact");

        // The SDK recorder must also work in a command buffer allocated,
        // submitted, and fenced entirely by the host (not by diagnostics).
        const auto external=context.borrowed_handles();
        HostBatch host(external.logical);
        host.init(external.queue_family);
        host.begin();
        const auto submitted_before=context.gpu_submission_count();
        context.record_integration(host.commands,first,
            {.time_step=dt,.gravity=gravity,.damping=0},1);
        check(context.gpu_submission_count()==submitted_before,
              "host recording has no internal queue submission");
        host.submit(external.compute_queue);
        check(context.gpu_submission_count()==submitted_before,
              "host owns queue submission and fence independent of SDK diagnostics");
        const auto host_after=context.diagnostic_read_positions(first);
        check(std::abs(host_after[1].y-(1.0f-15*step))<0.0001f,
              "externally submitted commands advance the same GPU history");

        // Animated attachment offsets are passed as frame constants; no
        // per-step CPU groom repack. Root history stores two vertices/guide.
        const auto history_before=context.diagnostic_read_root_history(first);
        check(history_before.size()==a->work.packed_guide_strands*2,
              "compact root history has exactly two vertices per padded guide");
        std::vector<loxfx::Float4> positions_after_first_motion;
        for(float translation : {0.5f,0.9f,1.2f}) {
            const auto motion=context.begin_diagnostic_commands();
            context.record_integration(motion,first,
                {.time_step=dt,.gravity=gravity,.damping=0,
                 .root_translation_x=translation},1);
            context.submit_diagnostic_and_wait();
            const auto current=context.diagnostic_read_positions(first);
            if (translation==0.5f) positions_after_first_motion=current;
            check(std::abs(current[0].x-translation)<0.0001f,
                  "root follows host attachment offset");
            check(current[4].x==0.0f,
                  "follower publication is not performed during integration");
        }
        const auto root_history=context.diagnostic_read_root_history(first);
        check(std::abs(root_history[0].x-0.5f)<0.0001f,
              "previous-previous root tracks the older attachment frame");
        check(std::abs(root_history[1].y-positions_after_first_motion[1].y)<0.0001f,
              "second root vertex stores prior-prior dynamic guide position");
        const auto separate=context.diagnostic_read_positions(second);
        check(separate[0].y==5.0f,
              "moving one groom cannot modify another groom's roots");

        // Source CalculateStrandLevelData assumes proximal attachment
        // vertices 0 and 1 provide a nondegenerate rigid-motion basis.
        // Unlike the prior dynamic vertex-1 fixture, both are pinned here.
        auto attached=fixture(0.0f,1);
        attached.positions[1].w=0.0f;
        auto attached_prepared=loxfx::prepare_groom(
            attached,{.followers_per_guide=1});
        check(attached_prepared.has_value(),"prepare attached-root fixture");
        auto attached_gpu=context.prepare(*attached_prepared);
        for(float displacement : {0.25f,0.75f,1.25f}) {
            auto command_buffer=context.begin_diagnostic_commands();
            context.record_integration(command_buffer,attached_gpu,
                {.time_step=dt,.gravity=gravity,.damping=0,
                 .root_translation_x=displacement},1);
            context.submit_diagnostic_and_wait();
            const auto pos=context.diagnostic_read_positions(attached_gpu);
            check(std::abs(pos[0].x-displacement)<0.0001f &&
                  std::abs(pos[1].x-displacement)<0.0001f &&
                  pos[0].y==0.0f && pos[1].y==1.0f,
                  "both attached proximal guide vertices translate rigidly");
        }
        const auto anchored_history=context.diagnostic_read_root_history(attached_gpu);
        check(std::abs(anchored_history[0].x-0.25f)<0.0001f &&
              std::abs(anchored_history[1].x-0.25f)<0.0001f,
              "compact history preserves both older rigid attachment positions");

        auto cmd2=context.begin_diagnostic_commands();
        context.record_integration(cmd2,attached_gpu,
            {.time_step=dt,.gravity=gravity,.damping=0,.root_translation_x=1.5f},1);
        context.record_strand_level(cmd2,attached_gpu,
            {.velocity_shock_coefficient=0.25f,.acceleration_threshold=0.1f});
        context.submit_diagnostic_and_wait();
        const auto strand=context.diagnostic_read_strand_data(attached_gpu);
        check(strand.size()==attached_prepared->work.packed_guide_strands,
              "strand data is compact guide-level output");
        check(std::abs(strand[0].vsp_translation_coefficient.x-0.25f)<0.0001f &&
              std::abs(strand[0].vsp_translation_coefficient.w-1.0f)<0.0001f,
              "strand acceleration threshold and translation");
        check(std::abs(strand[0].vsp_quaternion.w-1.0f)<0.0001f &&
              std::abs(strand[0].skinning_quaternion.w-1.0f)<0.0001f,
              "identity quaternion for a translating scalp");
        auto again=context.begin_diagnostic_commands();
        context.record_strand_level(again,attached_gpu,
            {.velocity_shock_coefficient=0.35f,.acceleration_threshold=100.0f});
        context.submit_diagnostic_and_wait();
        const auto below=context.diagnostic_read_strand_data(attached_gpu);
        check(std::abs(below[0].vsp_translation_coefficient.w-0.35f)<0.0001f,
              "configured VSP coefficient below acceleration threshold");
        // Stage three: propagate rigid scalp translation without touching
        // pinned proximal vertices or unsimulated follower storage.
        auto vsp_asset=fixture(0.0f,1);
        vsp_asset.positions[1].w=0.0f;
        auto vsp_prep=loxfx::prepare_groom(vsp_asset,{.followers_per_guide=1});
        check(vsp_prep.has_value(),"prepare VSP Vulkan fixture");
        auto vsp_gpu=context.prepare(*vsp_prep);
        auto vsp_cmd=context.begin_diagnostic_commands();
        context.record_integration(vsp_cmd,vsp_gpu,
            {.time_step=dt,.gravity=0,.damping=0,.root_translation_x=1.0f},1);
        context.record_strand_level(vsp_cmd,vsp_gpu,
            {.velocity_shock_coefficient=0.2f,.acceleration_threshold=0.0f});
        context.record_velocity_shock(vsp_cmd,vsp_gpu);
        context.submit_diagnostic_and_wait();
        const auto full_vsp=context.diagnostic_read_positions(vsp_gpu);
        check(std::abs(full_vsp[0].x-1.0f)<0.0001f &&
              std::abs(full_vsp[1].x-1.0f)<0.0001f &&
              std::abs(full_vsp[2].x-1.0f)<0.0001f &&
              std::abs(full_vsp[3].x-1.0f)<0.0001f,
              "VSP acceleration override carries free guide vertices with scalp");
        check(full_vsp[2].w==1.0f && full_vsp[0].w==0.0f &&
              full_vsp[4].x==vsp_prep->rest_positions[4].x,
              "VSP preserves pin flags and skips follower strands");
        // A second integration proves VSP transformed the previous positions
        // as well as current positions; otherwise these vertices overshoot.
        auto vsp_next=context.begin_diagnostic_commands();
        context.record_integration(vsp_next,vsp_gpu,
            {.time_step=dt,.gravity=0,.damping=0,.root_translation_x=1.0f},1);
        context.submit_diagnostic_and_wait();
        const auto continued=context.diagnostic_read_positions(vsp_gpu);
        check(std::abs(continued[2].x-1.0f)<0.0001f &&
              std::abs(continued[3].x-1.0f)<0.0001f,
              "VSP previous-position history avoids next-frame velocity spikes");

        auto partial_gpu=context.prepare(*vsp_prep);
        auto partial_cmd=context.begin_diagnostic_commands();
        context.record_integration(partial_cmd,partial_gpu,
            {.time_step=dt,.gravity=0,.damping=0,.root_translation_x=1.0f},1);
        context.record_strand_level(partial_cmd,partial_gpu,
            {.velocity_shock_coefficient=0.25f,.acceleration_threshold=100.0f});
        context.record_velocity_shock(partial_cmd,partial_gpu);
        context.submit_diagnostic_and_wait();
        const auto partial_vsp=context.diagnostic_read_positions(partial_gpu);
        check(std::abs(partial_vsp[2].x-0.25f)<0.0001f &&
              std::abs(partial_vsp[3].x-0.25f)<0.0001f &&
              std::abs(partial_vsp[0].x-1.0f)<0.0001f,
              "VSP blends free vertices at configured coefficient");

        // Nonidentity rotation: a movable first segment tilts under
        // gravity. The source transform must rotate +X toward -Y.
        auto tilting=fixture(0.0f,1);
        tilting.positions[1].x=1.0f;
        tilting.positions[1].y=0.0f;
        auto tilted_prep=loxfx::prepare_groom(tilting,{.followers_per_guide=1});
        check(tilted_prep.has_value(),"prepare tilted guide");
        auto tilted_gpu=context.prepare(*tilted_prep);
        auto tilt_commands=context.begin_diagnostic_commands();
        context.record_integration(tilt_commands,tilted_gpu,
            {.time_step=dt,.gravity=500.0f,.damping=0},1);
        context.record_strand_level(tilt_commands,tilted_gpu,
            {.velocity_shock_coefficient=0.42f,.acceleration_threshold=100.0f});
        context.submit_diagnostic_and_wait();
        const auto tilt_data=context.diagnostic_read_strand_data(tilted_gpu);
        check(tilt_data[0].vsp_quaternion.z < -0.04f &&
              tilt_data[0].vsp_quaternion.z > -0.10f &&
              std::abs(tilt_data[0].vsp_translation_coefficient.w-0.42f)<0.0001f,
              "Vulkan quaternion rotates +X toward -Y without VSP override");
        const auto before_vsp=context.diagnostic_read_positions(tilted_gpu);
        auto tilt_vsp=context.begin_diagnostic_commands();
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
            check(std::abs(after_vsp[v].x-expected_x)<0.0002f &&
                  std::abs(after_vsp[v].y-expected_y)<0.0002f,
                  "VSP applies source rotation, translation and blend to distal guide");
        }
        check(after_vsp[0].x==before_vsp[0].x &&
              after_vsp[1].x==before_vsp[1].x &&
              after_vsp[4].x==before_vsp[4].x,
              "VSP rotation excludes proximal and follower vertices");

        // Codex P2 regression: near-antiparallel root motion must retain
        // the actual 179.x-degree turn, not snap to an arbitrary 180.
        // First vertex is pinned; the second falls from (offset,+1,0)
        // to (offset,-1,0) in one Verlet step (g*dt^2 == 2).
        for(float transverse : {0.008f,0.007f,-0.007f,0.000087f,0.0f}) {
            auto hair=fixture(0.0f,1);
            hair.positions[1].x=transverse;
            auto prepared=loxfx::prepare_groom(hair,{.followers_per_guide=1});
            check(prepared.has_value(),"prepare antiparallel test groom");
            auto gpu=context.prepare(*prepared);
            auto command=context.begin_diagnostic_commands();
            context.record_integration(command,gpu,
                {.time_step=dt,.gravity=7200.0f,.damping=0.0f},1);
            context.record_strand_level(command,gpu,
                {.velocity_shock_coefficient=0.2f,.acceleration_threshold=100.0f});
            context.submit_diagnostic_and_wait();
            const auto motion=context.diagnostic_read_strand_data(gpu);
            const auto q=motion[0].vsp_quaternion;
            const float expected_w=std::abs(transverse)/std::sqrt(1.0f+transverse*transverse);
            check(std::isfinite(q.x)&&std::isfinite(q.y)&&
                  std::isfinite(q.z)&&std::isfinite(q.w),
                  "near-antiparallel rotation quaternion remains finite");
            check(std::abs(q.w-expected_w)<0.00002f,
                  "near-antiparallel rotation keeps the actual half-angle");
            const float old_x=transverse, old_y=1.0f;
            // Apply the quaternion to the previous proximal direction.
            const float mapped_x=(1-2*(q.y*q.y+q.z*q.z))*old_x+
                                  2*(q.x*q.y-q.w*q.z)*old_y;
            const float mapped_y=2*(q.x*q.y+q.w*q.z)*old_x+
                                  (1-2*(q.x*q.x+q.z*q.z))*old_y;
            check(std::abs(mapped_x-transverse)<0.00005f &&
                  std::abs(mapped_y+1.0f)<0.00005f,
                  "quaternion maps previous direction onto actual new direction");
        }
        std::cout << "Vulkan device: "<<context.device_name()
                  <<(context.software_device()?" (software)":" (hardware)")
                  <<"\nShared GPU device: "<<shared
                  <<"\nGroom A buffers allocated: "<<first.allocated_buffer_bytes()
                  <<"\nGroom B buffers allocated: "<<second.allocated_buffer_bytes()
                  <<"\nTwo-groom simulation batch GPU milliseconds: "<<kernel_batch_ms
                  <<"\nPASS: two independently animated grooms on one context;"
                    " record-only simulation; explicit submission; no stage readback\n";
    } catch(const std::exception& ex) {
        std::cerr<<"FAIL: "<<ex.what()<<"\n";
        return 1;
    }
    return 0;
}
