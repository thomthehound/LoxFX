// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#include "loxfx/vulkan/smoke_context.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

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
template<typename Fn>void reject(Fn&& fn, const char* what) {
    try { fn(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(what);
}
}

int main(int argc,char** argv) {
    if (argc != 2) return 2;
    try {
        namespace gpu = loxfx::vulkan::smoke;
        const auto a=loxfx::prepare_groom(fixture(0.0f,1),
                                           {.followers_per_guide=1});
        const auto b=loxfx::prepare_groom(fixture(5.0f,2),
                                           {.followers_per_guide=2});
        check(a&&b,"prepare independent grooms");
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
