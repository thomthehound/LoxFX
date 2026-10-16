// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
#include "loxfx/native_frame_parameters.h"
#include <cmath>
#include <iostream>

int main() {
    loxfx::NativeFrameParameters frame{};
    frame.time_step = 1.0f/30.0f; // explicitly not the legacy 60 Hz default
    frame.gravity = 2.0f;
    frame.damping = 0.2f;
    frame.root_translation_x = 0.75f;
    frame.length_iterations = 3;
    frame.winds[1] = {420.0f,3.0f,0.0f,0.0f};

    const auto lowered = loxfx::native_frame_stage_parameters(frame);
    if (lowered.integration.time_step != frame.time_step ||
        lowered.length_wind.time_step != frame.time_step ||
        lowered.integration.time_step != lowered.length_wind.time_step ||
        lowered.integration.gravity != frame.gravity ||
        lowered.integration.damping != frame.damping ||
        lowered.integration.root_translation_x != frame.root_translation_x ||
        lowered.length_wind.length_iterations != frame.length_iterations ||
        lowered.length_wind.winds[1].x != frame.winds[1].x) {
        std::cerr << "FAIL: native frame stages disagree on clock or parameters\n";
        return 1;
    }
    std::cout << "PASS: one native frame time step lowers to both stages\n";
    return 0;
}
