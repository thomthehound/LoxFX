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
        // Host-owned GPU upload: allocation/packing cannot create a
        // diagnostic submission. The two transfer batches share a single
        // caller-owned command buffer and one caller-owned completion fence.
        const auto uploads_before=context.gpu_submission_count();
        auto staged_a=context.prepare_upload(*a);
        auto staged_b=context.prepare_upload(*b);
        check(context.gpu_submission_count()==uploads_before,
              "Vulkan groom allocation performed hidden queue submission");
        const auto upload_external=context.borrowed_handles();
        HostBatch upload_host(upload_external.logical);
        upload_host.init(upload_external.queue_family);
        upload_host.begin();
        context.record_initial_upload(upload_host.commands,staged_a);
        context.record_initial_upload(upload_host.commands,staged_b);
        check(context.gpu_submission_count()==uploads_before,
              "Vulkan upload recording submitted or waited on the GPU");
        reject([&] { context.record_initial_upload(upload_host.commands,staged_a); },
               "Vulkan accepted duplicate staged upload recording");
        upload_host.submit(upload_external.compute_queue);
        auto uploaded_a=std::move(staged_a).finish_after_completion();
        auto uploaded_b=std::move(staged_b).finish_after_completion();
        check(context.gpu_submission_count()==uploads_before,
              "host-controlled upload was counted as diagnostic submission");
        const auto initial_a=context.diagnostic_read_positions(uploaded_a);
        const auto initial_b=context.diagnostic_read_positions(uploaded_b);
        check(initial_a.size()==a->rest_positions.size() &&
              initial_b.size()==b->rest_positions.size(),
              "host-recorded Vulkan groom upload sizes");
        for(std::size_t i=0;i<initial_a.size();++i)
            check(initial_a[i].x==a->rest_positions[i].x &&
                  initial_a[i].y==a->rest_positions[i].y,
                  "host-uploaded Vulkan groom A rest positions");
        for(std::size_t i=0;i<initial_b.size();++i)
            check(initial_b[i].x==b->rest_positions[i].x &&
                  initial_b[i].y==b->rest_positions[i].y,
                  "host-uploaded Vulkan groom B rest positions");

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
        // Stage four: pinned guide roots and source-normalized local stiffness.
        // One dispatch is enough for an exact numeric straight-rest fixture.
        auto shape_asset=fixture(0.0f,1);
        shape_asset.positions[1].w=0.0f;
        for(std::uint32_t iterations : {1u,3u}) {
            auto shape_prepared=loxfx::prepare_groom(
                shape_asset,{.followers_per_guide=1,.local_shape_iterations=iterations});
            check(shape_prepared.has_value(),"prepare local shape fixture");
            auto shape_gpu=context.prepare(*shape_prepared);
            auto shape_cmd=context.begin_diagnostic_commands();
            context.record_integration(shape_cmd,shape_gpu,
                {.time_step=dt,.gravity=720.0f,.damping=0.0f},1);
            context.record_strand_level(shape_cmd,shape_gpu,
                {.velocity_shock_coefficient=0.0f,.acceleration_threshold=100.0f});
            context.record_velocity_shock(shape_cmd,shape_gpu);
            context.record_local_shape(shape_cmd,shape_gpu,1.0f);
            context.submit_diagnostic_and_wait();
            const auto result=context.diagnostic_read_positions(shape_gpu);
            check(result[0].y==0.0f && result[1].y==1.0f &&
                      result[0].w==0.0f && result[1].w==0.0f,
                      "Vulkan local shape preserves attached vertices");
            check(std::abs(result[4].y-shape_prepared->rest_positions[4].y)<0.0001f,
                      "Vulkan local shape does not simulate followers");
            if(iterations==1u) {
                // Source uses 0.5*min(1,0.95)=0.475 stiffness.
                // Gravity displaces free vertices by 0.2. Sequential
                // corrections yield y2=1.849875 and y3=2.845125.
                check(std::abs(result[2].y-1.849875f)<0.0003f &&
                          std::abs(result[3].y-2.845125f)<0.0003f,
                          "Vulkan source local-shape correction and vertex order");
            } else {
                check(std::abs(result[2].y-1.849875f)>0.0005f,
                          "Vulkan repeated short-strand shape iterations execute");
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
            check(long_prepared.has_value(),"prepare long-strand fixture");
            auto long_gpu=context.prepare(*long_prepared);
            auto cmd=context.begin_diagnostic_commands();
            context.record_integration(cmd,long_gpu,
                {.time_step=dt,.gravity=720.0f,.damping=0.0f},1);
            context.record_strand_level(cmd,long_gpu,
                {.velocity_shock_coefficient=0.0f,.acceleration_threshold=100.0f});
            context.record_velocity_shock(cmd,long_gpu);
            context.record_local_shape(cmd,long_gpu,1.0f);
            context.submit_diagnostic_and_wait();
            const auto result=context.diagnostic_read_positions(long_gpu);
            check(result[0].y==0.0f && result[1].y==1.0f &&
                      result[64].y==long_prepared->rest_positions[64].y,
                      "Vulkan 64-vertex shape preserves roots and follower storage");
            check(std::isfinite(result[2].y) && std::isfinite(result[63].y),
                      "Vulkan long-strand local correction stays finite");
            if(iterations==1u) long_once=result[2].y;
            else check(std::abs(result[2].y-long_once)>0.0001f,
                           "Vulkan 64-vertex internal iterations run");
        }

        // Length/wind stage: true guide-parallel odd/even constraints,
        // source rest lengths, pinned roots, and follower isolation.
        auto length_asset=fixture(0.0f,1);
        length_asset.positions[1].w=0.0f;
        auto prepared=loxfx::prepare_groom(length_asset,
            {.followers_per_guide=2,.local_shape_iterations=1});
        check(prepared.has_value(),"prepare length fixture");
        auto length_gpu=context.prepare(*prepared);
        auto cmd=context.begin_diagnostic_commands();
        context.record_integration(cmd,length_gpu,
            {.time_step=dt,.gravity=720.0f,.damping=0.0f},1);
        context.record_strand_level(cmd,length_gpu,
            {.velocity_shock_coefficient=0.0f,.acceleration_threshold=100.0f});
        context.record_velocity_shock(cmd,length_gpu);
        context.record_length_wind(cmd,length_gpu,
            {.time_step=dt,.length_iterations=4});
        context.submit_diagnostic_and_wait();
        const auto length_result=context.diagnostic_read_positions(length_gpu);
        check(length_result[0].y==0.0f && length_result[1].y==1.0f,
                 "Vulkan length constraints preserve attached roots");
        check(std::abs(length_result[2].y-2.0f)<0.0003f &&
                 std::abs(length_result[3].y-2.975f)<0.0003f,
                 "Vulkan odd/even shared-memory length solver matches expected positions");
        check(length_result[4].y==prepared->rest_positions[4].y &&
                 length_result[8].y==prepared->rest_positions[8].y,
                 "Vulkan length pass excludes generated followers");

        // Source four-wind blend: at guide zero the two (1-a) winds
        // contribute, applying force only to movable interior vertices.
        auto wind_gpu=context.prepare(*prepared);
        auto wind_cmd=context.begin_diagnostic_commands();
        loxfx::LengthWindParameters wind{};
        wind.time_step=dt;
        wind.length_iterations=0;
        wind.winds[1].x=360.0f;
        context.record_length_wind(wind_cmd,wind_gpu,wind);
        context.submit_diagnostic_and_wait();
        const auto wind_result=context.diagnostic_read_positions(wind_gpu);
        check(std::abs(wind_result[2].x+0.1f)<0.0003f &&
                 wind_result[3].x==0.0f && wind_result[1].x==0.0f,
                 "Vulkan four-wind blend moves only interior free vertices");

        // One strand per workgroup at 64 vertices: all group barriers must
        // remain convergent and outputs must be finite.
        loxfx::GroomAsset long_length;
        long_length.guide_strands=1;
        long_length.vertices_per_strand=64;
        for(std::uint32_t v=0;v<64;++v)
            long_length.positions.push_back({0.0f,static_cast<float>(v),0.0f,
                                             v<2u?0.0f:1.0f});
        auto long_prepared=loxfx::prepare_groom(long_length,{.followers_per_guide=1});
        check(long_prepared.has_value(),"prepare 64-vertex length fixture");
        auto long_gpu=context.prepare(*long_prepared);
        auto long_cmd=context.begin_diagnostic_commands();
        context.record_integration(long_cmd,long_gpu,
            {.time_step=dt,.gravity=720.0f,.damping=0.0f},1);
        context.record_strand_level(long_cmd,long_gpu,
            {.velocity_shock_coefficient=0.0f,.acceleration_threshold=100.0f});
        context.record_velocity_shock(long_cmd,long_gpu);
        context.record_length_wind(long_cmd,long_gpu,
            {.time_step=dt,.length_iterations=3});
        context.submit_diagnostic_and_wait();
        const auto long_result=context.diagnostic_read_positions(long_gpu);
        check(long_result[0].y==0.0f && long_result[1].y==1.0f &&
                 std::isfinite(long_result[2].y) &&
                 std::isfinite(long_result[63].y) &&
                 long_result[64].y==long_prepared->rest_positions[64].y,
                 "Vulkan long strand length simulation and follower isolation");

        // Final output: animated guide positions plus exact source-prepared
        // per-follower offsets, including nonzero tip separation.
        auto follow_asset=fixture(0.0f,1);
        follow_asset.positions[1].w=0.0f;
        auto follow_prep=loxfx::prepare_groom(follow_asset,
            {.followers_per_guide=2,.follower_root_radius=0.35f,
             .follower_tip_separation=0.65f,.seed=17u});
        check(follow_prep.has_value(),"prepare follower publication");
        auto follow_gpu=context.prepare(*follow_prep);
        auto follow_cmd=context.begin_diagnostic_commands();
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
        check(published.size()==follow_prep->rest_positions.size() &&
                  published_tangents.size()==published.size(),
                  "Vulkan follower publication allocates full outputs");
        check(std::abs(published[2].x-follow_prep->rest_positions[2].x)>0.01f,
                  "Vulkan guide actually moves before publication");
        for(std::uint32_t v=0;v<4u;++v) {
            const auto& tangent=published_tangents[v];
            const float norm=std::sqrt(tangent.x*tangent.x+
                                       tangent.y*tangent.y+tangent.z*tangent.z);
            check(std::isfinite(norm) && std::abs(norm-1.0f)<0.0003f,
                      "Vulkan guide tangents normalized");
            for(std::uint32_t f=1u;f<=2u;++f) {
                const std::uint32_t index=f*4u+v;
                const auto& q=published[index];
                const auto& rest_f=follow_prep->rest_positions[index];
                const auto& rest_g=follow_prep->rest_positions[v];
                check(std::abs(q.x-(published[v].x+rest_f.x-rest_g.x))<0.0003f &&
                          std::abs(q.y-(published[v].y+rest_f.y-rest_g.y))<0.0003f &&
                          std::abs(q.z-(published[v].z+rest_f.z-rest_g.z))<0.0003f,
                          "Vulkan followers preserve prepared offsets including tip ease");
                check(std::abs(published_tangents[index].x-tangent.x)<0.0001f &&
                          std::abs(published_tangents[index].y-tangent.y)<0.0001f &&
                          std::abs(published_tangents[index].z-tangent.z)<0.0001f,
                          "Vulkan follower tangents copy guide final direction");
            }
        }
        // OPT-001 regression: no follower rest-vertex reads during GPU
        // publication. Disturb the prepared follower rest positions while
        // retaining the canonical compact offset records and guide rest.
        auto compact_only=*follow_prep;
        for(std::uint32_t strand=1;strand<=2u;++strand)
            for(std::uint32_t v=0;v<4u;++v) {
                auto& p=compact_only.rest_positions[strand*4u+v];
                p.x+=10.0f; p.y-=7.0f; p.z+=5.0f;
            }
        auto compact_gpu=context.prepare(compact_only);
        auto compact_cmd=context.begin_diagnostic_commands();
        context.record_followers(compact_cmd,compact_gpu);
        context.submit_diagnostic_and_wait();
        const auto compact_result=context.diagnostic_read_positions(compact_gpu);
        for(std::uint32_t strand=1;strand<=2u;++strand)
            for(std::uint32_t v=0;v<4u;++v) {
                const auto i=strand*4u+v;
                const auto& original=follow_prep->rest_positions[i];
                check(std::abs(compact_result[i].x-original.x)<0.0003f &&
                      std::abs(compact_result[i].y-original.y)<0.0003f &&
                      std::abs(compact_result[i].z-original.z)<0.0003f,
                      "Vulkan publication requires only compact offsets, not follower rest vertices");
            }

        auto repeat_cmd=context.begin_diagnostic_commands();
        context.record_followers(repeat_cmd,follow_gpu);
        context.submit_diagnostic_and_wait();
        const auto repeated=context.diagnostic_read_positions(follow_gpu);
        for(std::uint32_t v=0;v<12u;++v)
            check(std::abs(repeated[v].x-published[v].x)<0.0001f &&
                      std::abs(repeated[v].y-published[v].y)<0.0001f,
                      "Vulkan repeated publication does not accumulate offsets");

        auto no_follow_prep=loxfx::prepare_groom(follow_asset);
        check(no_follow_prep.has_value(),"prepare guide-only publication");
        auto no_follow_gpu=context.prepare(*no_follow_prep);
        auto no_follow_cmd=context.begin_diagnostic_commands();
        context.record_followers(no_follow_cmd,no_follow_gpu);
        context.submit_diagnostic_and_wait();
        const auto guide_only=context.diagnostic_read_tangents(no_follow_gpu);
        check(std::abs(guide_only[2].y-1.0f)<0.0001f,
                  "Vulkan guide-only output produces valid tangents");

        // Manual review: cross-group publication with three followers
        // and guide padding. This exercises packed offsets beyond one
        // 64-lane workgroup, not just the single-guide fixture.
        auto many_asset=fixture(1.0f,17);
        auto many_prep=loxfx::prepare_groom(many_asset,
            {.followers_per_guide=3,.follower_root_radius=0.2f,
             .follower_tip_separation=0.7f,.seed=42u});
        check(many_prep.has_value(),"prepare cross-group followers");
        auto many_gpu=context.prepare(*many_prep);
        auto many_cmd=context.begin_diagnostic_commands();
        context.record_followers(many_cmd,many_gpu);
        context.submit_diagnostic_and_wait();
        const auto many_positions=context.diagnostic_read_positions(many_gpu);
        const auto many_tangents=context.diagnostic_read_tangents(many_gpu);
        for(std::uint32_t guide=0;guide<17u;++guide) {
            const std::uint32_t base=guide*4u*4u;
            for(std::uint32_t v=0;v<4u;++v) {
                const auto& q=many_positions[base+v];
                const auto& rest=many_prep->rest_positions[base+v];
                check(std::abs(q.x-rest.x)<0.0003f &&
                          std::abs(q.y-rest.y)<0.0003f,
                          "Vulkan cross-group guide positions");
                for(std::uint32_t follower=1;follower<=3u;++follower) {
                    const std::uint32_t idx=base+follower*4u+v;
                    const auto& expected=many_prep->rest_positions[idx];
                    const auto& result=many_positions[idx];
                    check(std::abs(result.x-expected.x)<0.0003f &&
                              std::abs(result.y-expected.y)<0.0003f &&
                              std::abs(result.z-expected.z)<0.0003f &&
                              std::abs(many_tangents[idx].y-1.0f)<0.0003f,
                              "Vulkan cross-group three-follower offsets and tangents");
                }
            }
        }

        // Full source-ordered simulation of two grooms recorded into ONE
        // independent host-owned command buffer/queue submission.
        // Unlike diagnostic_read_positions, record_frame must never wait
        // or submit on behalf of the host.
        auto host_a_source=loxfx::prepare_groom(fixture(0.0f,1),
            {.followers_per_guide=1});
        auto host_b_source=loxfx::prepare_groom(fixture(5.0f,2),
            {.followers_per_guide=2});
        check(host_a_source && host_b_source,"prepare frame recording grooms");
        auto host_a=context.prepare(*host_a_source);
        auto host_b=context.prepare(*host_b_source);
        auto external_frame=context.borrowed_handles();
        HostBatch frame_batch(external_frame.logical);
        frame_batch.init(external_frame.queue_family);
        frame_batch.begin();
        const auto before_recording=context.gpu_submission_count();
        loxfx::NativeFrameParameters frame_a{};
        frame_a.gravity=0.0f;
        frame_a.root_translation_x=0.25f;
        frame_a.length_iterations=2;
        loxfx::NativeFrameParameters frame_b=frame_a;
        frame_b.root_translation_x=-0.40f;
        context.record_frame(frame_batch.commands,host_a,frame_a);
        context.record_frame(frame_batch.commands,host_b,frame_b);
        check(context.gpu_submission_count()==before_recording,
              "complete native Vulkan frame recorder performed hidden submission");
        frame_batch.submit(external_frame.compute_queue);
        check(context.gpu_submission_count()==before_recording,
              "host-owned full-frame Vulkan queue was counted as diagnostic submission");
        auto frame_a_positions=context.diagnostic_read_positions(host_a);
        auto frame_b_positions=context.diagnostic_read_positions(host_b);
        check(std::abs(frame_a_positions[0].x-0.25f)<0.001f &&
              std::abs(frame_b_positions[0].x+0.40f)<0.001f,
              "host-queued full native frame did not move independent roots");
        // Stage six follows each *guide vertex* after shape/length passes:
        // distal vertices need not translate rigidly with the guide root.
        check(std::abs(frame_a_positions[4].x-frame_a_positions[0].x)<0.001f &&
              std::abs(frame_a_positions[5].x-frame_a_positions[1].x)<0.001f &&
              std::abs(frame_b_positions[4].x-frame_b_positions[0].x)<0.001f &&
              std::abs(frame_b_positions[5].x-frame_b_positions[1].x)<0.001f,
              "host-queued Vulkan full frame failed guide-relative follower publication");

        // Regression for a single frame clock: a non-default 1/30 s
        // frame with active wind must agree with explicit stage-by-stage
        // recording when BOTH stages receive the same time step.
        auto clock_prep=loxfx::prepare_groom(fixture(0.0f,1),
            {.followers_per_guide=1});
        check(clock_prep.has_value(),"prepare wind/timestep regression groom");
        auto composite=context.prepare(*clock_prep);
        auto manual=context.prepare(*clock_prep);
        auto no_wind=context.prepare(*clock_prep);
        HostBatch clock_batch(external_frame.logical);
        clock_batch.init(external_frame.queue_family);
        clock_batch.begin();
        loxfx::NativeFrameParameters clock{};
        clock.time_step=1.0f/30.0f;
        clock.gravity=0.0f;
        clock.root_translation_x=0.15f;
        clock.length_iterations=2;
        clock.winds[1]={420.0f,0.0f,0.0f,0.0f};
        context.record_frame(clock_batch.commands,composite,clock);
        loxfx::IntegrationParameters manual_integration{};
        manual_integration.time_step=clock.time_step;
        manual_integration.gravity=clock.gravity;
        manual_integration.root_translation_x=clock.root_translation_x;
        loxfx::LengthWindParameters manual_wind{};
        manual_wind.time_step=clock.time_step;
        manual_wind.length_iterations=clock.length_iterations;
        manual_wind.winds=clock.winds;
        context.record_integration(clock_batch.commands,manual,manual_integration);
        context.record_strand_level(clock_batch.commands,manual,clock.strand_motion);
        context.record_velocity_shock(clock_batch.commands,manual);
        context.record_local_shape(clock_batch.commands,manual,clock.local_shape_stiffness);
        context.record_length_wind(clock_batch.commands,manual,manual_wind);
        context.record_followers(clock_batch.commands,manual);
        auto calm=clock;
        calm.winds={};
        context.record_frame(clock_batch.commands,no_wind,calm);
        clock_batch.submit(external_frame.compute_queue);
        const auto composite_positions=context.diagnostic_read_positions(composite);
        const auto manual_positions=context.diagnostic_read_positions(manual);
        const auto calm_positions=context.diagnostic_read_positions(no_wind);
        check(composite_positions.size()==manual_positions.size(),
              "composite/manual frame output has different vertex counts");
        for(std::size_t i=0;i<composite_positions.size();++i) {
            const auto& a=composite_positions[i];
            const auto& b=manual_positions[i];
            check(std::abs(a.x-b.x)<0.0003f &&
                  std::abs(a.y-b.y)<0.0003f &&
                  std::abs(a.z-b.z)<0.0003f,
                  "composite Vulkan frame disagrees with unified manual wind clock");
        }
        // The no-wind control excludes root translation and shape effects:
        // any measurable difference at the interior guide vertex comes from
        // wind itself, rather than unrelated motion of the strand.
        check(std::abs(manual_positions[2].x-calm_positions[2].x)>0.001f,
              "Vulkan clock regression did not produce measurable wind motion");

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
