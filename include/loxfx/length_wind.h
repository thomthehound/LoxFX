// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "loxfx/groom.h"
#include <array>
#include <cstdint>

namespace loxfx {

// Stage-five length/wind pass parameters. Collision providers and output
// publication are separate subsequent additions.
struct LengthWindParameters {
    float time_step = 1.0f / 60.0f;
    std::uint32_t length_iterations = 2;
    float maximum_position_delta = 0.0f; // 0 disables velocity clamping
    std::array<Float4, 4> winds{}; // xyz force vectors
};

} // namespace loxfx
