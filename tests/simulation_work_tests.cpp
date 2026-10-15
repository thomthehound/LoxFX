// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#include "loxfx/simulation_work.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void expect(bool passed, const char* why) {
    if (!passed) throw std::runtime_error(why);
}

void guide_vs_follow() {
    const auto plan = loxfx::make_simulation_work_plan({64, 16, 3, 2});
    expect(plan.has_value(), "valid guide/follower workload");
    expect(plan->packed_guide_strands == 64, "guide storage");
    expect(plan->packed_total_strands == 256, "follow storage");
    expect(plan->packed_total_vertices == 4096, "vertex storage");
    expect(plan->stages[0].thread_groups == 16, "guide vertex dispatch groups");
    expect(plan->stages[1].thread_groups == 1, "guide strand dispatch groups");
    expect(plan->stages[2].thread_groups == 16, "VSP guide vertex groups");
    expect(plan->stages[3].thread_groups == 1, "local constraints guide strands");
    expect(plan->stages[3].iterations == 2, "local iterations");
    expect(plan->stages[4].thread_groups == 16, "wind/length groups");
    expect(plan->stages[5].thread_groups == 16, "follower output guide threads");
    expect(plan->planned_lane_invocations == 4288, "guide lane accounting");
    expect(plan->reference_lane_invocations == 34304,
           "legacy work includes extra block on exact 64-guide boundary");
}

void padding() {
    for (std::uint32_t vertices : {4u, 8u, 16u, 32u, 64u}) {
        auto before = loxfx::make_simulation_work_plan({63, vertices, 0, 1});
        auto exact = loxfx::make_simulation_work_plan({64, vertices, 0, 1});
        auto after = loxfx::make_simulation_work_plan({65, vertices, 0, 1});
        expect(before && exact && after, "legal topology");
        expect(before->packed_guide_strands == 64, "under 64 padded");
        expect(exact->packed_guide_strands == 64, "exact multiple does not over-pad");
        expect(after->packed_guide_strands == 128, "65 rounds to 128");
        expect(exact->stages[0].thread_groups == vertices, "guide vertices / 64");
        expect(exact->stages[1].thread_groups == 1, "guide strands / 64");
        expect(before->planned_lane_invocations == before->reference_lane_invocations,
               "unaligned 63 guides, no followers");
        expect(exact->reference_lane_invocations == 2 * exact->planned_lane_invocations,
               "reference loader adds 64 extra strands at exact boundary");
        expect(after->planned_lane_invocations == after->reference_lane_invocations,
               "unaligned 65 guides, no followers");
    }
}

void iteration_and_error_bounds() {
    auto zero = loxfx::make_simulation_work_plan({1, 4, 0, 0});
    expect(zero && zero->stages[3].iterations == 0, "disabled local iterations");
    expect(zero->planned_lane_invocations == zero->reference_lane_invocations,
           "no followers still no wasted follower dispatch");

    auto many = loxfx::make_simulation_work_plan({64, 8, 1, 20});
    expect(many && many->stages[3].iterations == 20, "short strands use host iterations");
    expect(many->shader_local_shape_iterations == 1, "short strands use one shader iteration");
    auto long_strands = loxfx::make_simulation_work_plan({64, 64, 1, 20});
    expect(long_strands && long_strands->stages[3].iterations == 1,
           "64-vertex strands use one host dispatch");
    expect(long_strands->shader_local_shape_iterations == 20,
           "64-vertex shader is assigned the requested internal iterations");
    expect(!loxfx::make_simulation_work_plan({0, 4, 0, 1}), "zero guides");
    expect(!loxfx::make_simulation_work_plan({64, 3, 0, 1}), "invalid vertex topology");
    expect(!loxfx::make_simulation_work_plan({64, 128, 0, 1}), "non-reference vertex topology");
    expect(!loxfx::make_simulation_work_plan({
       std::numeric_limits<std::uint32_t>::max(), 64, 0, 2}), "extreme guide count");
    expect(!loxfx::make_simulation_work_plan({64, 16, 
       std::numeric_limits<std::uint32_t>::max(), 2}), "extreme follower count");
}
}

int main() {
    try {
        guide_vs_follow();
        padding();
        iteration_and_error_bounds();
        std::cout << "Simulation work-plan tests passed\n";
    } catch(const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
