// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
#include "native_diagnostic.h"

#include "loxfx/integration_parameters.h"
#include "loxfx/length_wind.h"
#include "loxfx/strand_motion.h"

#if defined(LOXFX_GODOT_NATIVE_VULKAN)
#include "loxfx/vulkan/smoke_context.h"
namespace native = loxfx::vulkan::smoke;
#elif defined(LOXFX_GODOT_NATIVE_D3D12)
#include "loxfx/d3d12/smoke_context.h"
namespace native = loxfx::d3d12::smoke;
#else
#error "Select exactly one native diagnostic backend"
#endif

#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

namespace loxfx::godot_adapter {

struct NativeDiagnostic::Impl {
    native::DeviceContext context;
    std::unique_ptr<native::GroomState> state;
    bool using_software = false;

    Impl(const std::filesystem::path& shader, const PreparedGroom& groom,
         bool software_device)
#if defined(LOXFX_GODOT_NATIVE_VULKAN)
      : context(shader)
#else
      : context(shader, software_device
                    ? native::AdapterChoice::warp
                    : native::AdapterChoice::hardware)
#endif
    {
#if defined(LOXFX_GODOT_NATIVE_VULKAN)
        // A Vulkan standalone context picks one compatible device; it does
        // not implement a silent hardware/software fallback preference.
        (void)software_device;
        using_software = context.software_device();
#else
        using_software = context.software_adapter();
#endif
        reset(groom);
    }

    void reset(const PreparedGroom& groom) {
#if defined(LOXFX_GODOT_NATIVE_VULKAN)
        auto prepared = context.prepare(groom);
#else
        auto prepared = context.prepare_diagnostic(groom);
#endif
        state = std::make_unique<native::GroomState>(std::move(prepared));
    }

    std::vector<Float4> step(float seconds, float gravity, float damping,
                             float rx, float ry, float rz,
                             float wx, float wy, float wz) {
        if (!state) throw std::logic_error("native groom not initialized");
        if (!std::isfinite(seconds) || seconds <= 0.0f || seconds > 0.1f ||
            !std::isfinite(gravity) || !std::isfinite(damping) ||
            !std::isfinite(rx) || !std::isfinite(ry) || !std::isfinite(rz) ||
            !std::isfinite(wx) || !std::isfinite(wy) || !std::isfinite(wz))
            throw std::invalid_argument("nonfinite or unsupported native frame parameters");

        auto cmd = context.begin_diagnostic_commands();
        context.record_integration(cmd, *state, {
            .time_step = seconds,
            .gravity = gravity,
            .damping = damping,
            .root_translation_x = rx,
            .root_translation_y = ry,
            .root_translation_z = rz
        });
        context.record_strand_level(cmd, *state, {
            .velocity_shock_coefficient = 0.0f,
            .acceleration_threshold = 100.0f
        });
        context.record_velocity_shock(cmd, *state);
        context.record_local_shape(cmd, *state, 0.6f);
        LengthWindParameters wind{};
        wind.time_step = seconds;
        wind.length_iterations = 4;
        wind.winds[0] = {wx,wy,wz,0.0f};
        context.record_length_wind(cmd, *state, wind);
        context.record_followers(cmd, *state);
        // Diagnostic-only CPU synchronization: one completed stage batch,
        // then one explicit output snapshot; NOT the production game path.
        context.submit_diagnostic_and_wait();
        return context.diagnostic_read_positions(*state);
    }
};

NativeDiagnostic::NativeDiagnostic(std::filesystem::path shader,
                                    const PreparedGroom& groom, bool sw)
    : impl_(std::make_unique<Impl>(shader, groom, sw)) {}
NativeDiagnostic::~NativeDiagnostic() = default;
void NativeDiagnostic::reset(const PreparedGroom& groom) {
    impl_->reset(groom);
}
std::vector<Float4> NativeDiagnostic::step(float dt, float gravity, float damping,
                  float rx, float ry, float rz, float wx, float wy, float wz) {
    return impl_->step(dt,gravity,damping,rx,ry,rz,wx,wy,wz);
}
const char* NativeDiagnostic::backend_name() const noexcept {
#if defined(LOXFX_GODOT_NATIVE_VULKAN)
    return "Vulkan SDK-owned diagnostic";
#else
    return "D3D12 SDK-owned diagnostic";
#endif
}
bool NativeDiagnostic::software_device() const noexcept {
    return impl_->using_software;
}

} // namespace loxfx::godot_adapter
