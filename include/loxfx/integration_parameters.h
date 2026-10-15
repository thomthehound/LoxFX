// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace loxfx {

// Simulation semantics are backend-neutral. Vulkan and D3D12 must receive
// identical integration parameters; HLSL resource binding is backend-specific.
// This is not a promise of binary ABI stability before the complete solver.
struct IntegrationParameters {
    float time_step = 1.0f / 60.0f;
    float gravity = 9.81f;
    float damping = 0.08f;
    float global_stiffness = 0.0f;
    float global_range = 0.0f;
};

} // namespace loxfx
