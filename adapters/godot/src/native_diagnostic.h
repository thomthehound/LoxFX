// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "loxfx/prepared_groom.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace loxfx::godot_adapter {

// Opt-in, non-production execution bridge using LoxFX's real native passes.
// Entirely engine-neutral and deliberately opaque to the GDExtension node.
// The SDK owns a standalone diagnostic compute device, not Godot's device.
// Only final snapshots cross to the engine's CPU diagnostic line renderer.
class NativeDiagnostic {
public:
    NativeDiagnostic(std::filesystem::path shader, const PreparedGroom& groom,
                     bool software_device);
    ~NativeDiagnostic();
    NativeDiagnostic(const NativeDiagnostic&) = delete;
    NativeDiagnostic& operator=(const NativeDiagnostic&) = delete;

    void reset(const PreparedGroom& groom);
    [[nodiscard]] std::vector<Float4> step(float seconds,
                float gravity, float damping, float root_x, float root_y,
                float root_z, float wind_x, float wind_y, float wind_z);
    [[nodiscard]] const char* backend_name() const noexcept;
    [[nodiscard]] bool software_device() const noexcept;
    // Diagnostic instrumentation: count active, separately owned device
    // contexts (not grooms). Useful for multi-groom regression checks.
    [[nodiscard]] static std::uint32_t active_context_count() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace loxfx::godot_adapter
