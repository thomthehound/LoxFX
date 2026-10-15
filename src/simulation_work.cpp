// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#include "loxfx/simulation_work.h"

#include <limits>
#include <string>

namespace loxfx {
namespace {

constexpr std::uint64_t group_size = 64;
constexpr std::uint64_t u32_max = std::numeric_limits<std::uint32_t>::max();

bool is_legal_vertex_count(std::uint32_t n) {
    return n >= 4 && n <= 64 && (n & (n - 1u)) == 0;
}

bool mul_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& result) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a)
        return false;
    result = a * b;
    return true;
}

bool add_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& result) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a)
        return false;
    result = a + b;
    return true;
}

} // namespace

std::expected<SimulationWorkPlan, std::string>
make_simulation_work_plan(const SimulationShape& shape) {
    if (shape.authored_guide_strands == 0 || !is_legal_vertex_count(shape.vertices_per_strand))
        return std::unexpected("invalid authored guides or vertices per strand");

    // 64-guide padding is needed by the unguarded source master-index kernels,
    // even when each workgroup processes only 64 / vertices_per_strand guides.
    // Compute in 64 bits before any narrowing.
    const std::uint64_t padded =
        ((static_cast<std::uint64_t>(shape.authored_guide_strands) + group_size - 1) /
         group_size) * group_size;
    const std::uint64_t multiplier = static_cast<std::uint64_t>(shape.followers_per_guide) + 1;
    std::uint64_t total_strands = 0, total_vertices = 0;
    if (!mul_u64(padded, multiplier, total_strands) ||
        !mul_u64(total_strands, shape.vertices_per_strand, total_vertices) ||
        total_vertices > u32_max)
        return std::unexpected("packed simulation exceeds 32-bit strand/vertex indexing");

    const std::uint64_t guide_vertices = padded * shape.vertices_per_strand;
    const std::uint64_t vertex_groups = guide_vertices / group_size;
    const std::uint64_t strand_groups = padded / group_size;
    // All counts fit in uint32 because total_vertices is checked above.
    SimulationWorkPlan plan{
        static_cast<std::uint32_t>(padded),
        static_cast<std::uint32_t>(total_strands),
        static_cast<std::uint32_t>(total_vertices),
        {{
            {SimulationStage::integration_and_global_shape, static_cast<std::uint32_t>(vertex_groups), 1},
            {SimulationStage::calculate_strand_level_data, static_cast<std::uint32_t>(strand_groups), 1},
            {SimulationStage::velocity_shock_propagation, static_cast<std::uint32_t>(vertex_groups), 1},
            {SimulationStage::local_shape_constraints, static_cast<std::uint32_t>(strand_groups), shape.local_shape_iterations},
            {SimulationStage::length_wind_and_collision, static_cast<std::uint32_t>(vertex_groups), 1},
            {SimulationStage::update_follow_vertices, static_cast<std::uint32_t>(vertex_groups), 1}
        }},
        0, 0
    };

    // Logical lane dispatch accounting, *not* GPU timing or a speedup claim.
    // The legacy host launches based on total guides + generated followers,
    // while all six shader entry points use master/guide-strand indexing.
    for (const auto& stage : plan.stages) {
        std::uint64_t guide_lanes = 0, reference_lanes = 0, count = 0;
        if (!mul_u64(static_cast<std::uint64_t>(stage.thread_groups) * group_size,
                     stage.iterations, guide_lanes) ||
            !add_u64(plan.planned_lane_invocations, guide_lanes, count))
            return std::unexpected("planned dispatch count overflow");
        plan.planned_lane_invocations = count;

        const std::uint64_t legacy_groups =
            (stage.stage == SimulationStage::calculate_strand_level_data ||
             stage.stage == SimulationStage::local_shape_constraints)
            ? total_strands / group_size
            : total_vertices / group_size;
        if (!mul_u64(legacy_groups * group_size, stage.iterations, reference_lanes) ||
            !add_u64(plan.reference_lane_invocations, reference_lanes, count))
            return std::unexpected("reference dispatch count overflow");
        plan.reference_lane_invocations = count;
    }
    return plan;
}

} // namespace loxfx
