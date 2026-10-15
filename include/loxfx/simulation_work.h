// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>

namespace loxfx {

// Stages and their order are traced to AMD TressFX 4.1
// src/TressFX/TressFXSimulation.cpp::Simulate.
enum class SimulationStage : std::uint8_t {
    integration_and_global_shape,
    calculate_strand_level_data,
    velocity_shock_propagation,
    local_shape_constraints,
    length_wind_and_collision,
    update_follow_vertices
};

struct SimulationShape {
    std::uint32_t authored_guide_strands;
    std::uint32_t vertices_per_strand;
    std::uint32_t followers_per_guide = 0;
    std::uint32_t local_shape_iterations = 2;
};

struct SimulationDispatch {
    SimulationStage stage;
    std::uint32_t thread_groups;
    std::uint32_t iterations;
};

// Immutable work quantities determined during groom preparation or LOD changes.
// This does not allocate, submit, synchronize, or simulate. The future GPU
// executor owns actual resources and stage-major scheduling across all grooms.
struct SimulationWorkPlan {
    std::uint32_t packed_guide_strands;
    std::uint32_t packed_total_strands;
    std::uint32_t packed_total_vertices;
    // When 64 vertices occupy a strand, a single lane can perform all local
    // iterations; short strands use separate host dispatches instead.
    std::uint32_t shader_local_shape_iterations;
    std::array<SimulationDispatch, 6> stages;
    std::uint64_t planned_lane_invocations;
    std::uint64_t reference_lane_invocations;
};

// The source kernels use 64 lanes, and master (guide) indexing. LoxFX must
// allocate the padded storage before executing a plan: generated followers
// remain in storage but are not independently simulated by these six kernels.
// No device/capability queries occur in this function or on the dispatch path.
std::expected<SimulationWorkPlan, std::string>
make_simulation_work_plan(const SimulationShape& shape);

} // namespace loxfx
