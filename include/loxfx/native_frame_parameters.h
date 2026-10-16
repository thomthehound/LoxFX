// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "loxfx/integration_parameters.h"
#include "loxfx/strand_motion.h"
#include "loxfx/length_wind.h"
#include <array>
#include <cstdint>

namespace loxfx {

// Neutral inputs for one source-ordered native simulation frame.
// There is exactly ONE simulation clock. Per-stage legacy parameter objects
// are assembled only at the native recording boundary; they cannot drift.
// Provisional internal GPU interface, not a stable public SDK ABI.
struct NativeFrameParameters {
    float time_step = 1.0f / 60.0f;
    float gravity = 9.81f;
    float damping = 0.08f;
    float global_stiffness = 0.0f;
    float global_range = 0.0f;
    // Translation-only attachment milestone, in authored world units.
    float root_translation_x = 0.0f;
    float root_translation_y = 0.0f;
    float root_translation_z = 0.0f;
    StrandMotionParameters strand_motion{};
    float local_shape_stiffness = 0.6f;
    std::uint32_t length_iterations = 2;
    float maximum_position_delta = 0.0f;
    std::array<Float4, 4> winds{};
    std::uint32_t integration_steps = 1;
};

// One backend-neutral lowering for both native APIs. The existing individual
// pass methods keep their established parameters and validation; composite
// recording never exposes two independently configurable frame clocks.
struct NativeFrameStageParameters {
    IntegrationParameters integration;
    LengthWindParameters length_wind;
};

[[nodiscard]] inline NativeFrameStageParameters
native_frame_stage_parameters(const NativeFrameParameters& p) {
    IntegrationParameters integration{};
    integration.time_step = p.time_step;
    integration.gravity = p.gravity;
    integration.damping = p.damping;
    integration.global_stiffness = p.global_stiffness;
    integration.global_range = p.global_range;
    integration.root_translation_x = p.root_translation_x;
    integration.root_translation_y = p.root_translation_y;
    integration.root_translation_z = p.root_translation_z;

    LengthWindParameters length_wind{};
    length_wind.time_step = p.time_step;
    length_wind.length_iterations = p.length_iterations;
    length_wind.maximum_position_delta = p.maximum_position_delta;
    length_wind.winds = p.winds;
    return {integration, length_wind};
}

} // namespace loxfx
