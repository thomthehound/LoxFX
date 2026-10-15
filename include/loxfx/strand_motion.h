// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "loxfx/groom.h"
#include <cstdint>
#include <type_traits>

namespace loxfx {

// One compact 48-byte record per padded guide, matching the three float4
// fields used by TressFX 4.1's StrandLevelData (not one record per follower).
// Identity skinning is the only meaningful skinning value in the current
// translation-only attachment milestone.
struct StrandMotionData {
    Float4 skinning_quaternion;
    Float4 vsp_quaternion;
    Float4 vsp_translation_coefficient;
};
static_assert(sizeof(StrandMotionData)==3*sizeof(Float4));
static_assert(std::is_trivially_copyable_v<StrandMotionData>);

struct StrandMotionParameters {
    float velocity_shock_coefficient = 0.0f;
    float acceleration_threshold = 1000000.0f;
};
} // namespace loxfx
