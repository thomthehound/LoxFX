// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "loxfx/integration_parameters.h"
#include "loxfx/strand_motion.h"
#include "loxfx/length_wind.h"
#include <cstdint>

namespace loxfx {

// Neutral inputs for one source-ordered native simulation frame.
// This is a provisional full-stage recording contract, not a public
// promise of solver/renderer ABI stability or a command submission API.
// Backend command list/buffer and prepared GPU groom are separate.
struct NativeFrameParameters {
    IntegrationParameters integration{};
    StrandMotionParameters strand_motion{};
    float local_shape_stiffness = 0.6f;
    LengthWindParameters length_wind{};
    std::uint32_t integration_steps = 1;
};

} // namespace loxfx
