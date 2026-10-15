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
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace loxfx::godot_adapter {
namespace {

// This is an adapter-level *standalone diagnostic* device owner, not part of
// any groom or engine-neutral SDK API. A future Godot renderer-owned backend
// must obtain its device/queue from the host instead of using this pool.
// Invariant pipelines, diagnostic command allocator and queue live here.
struct DiagnosticDevice {
    native::DeviceContext context;
    std::mutex commands; // serialize entire record / submit / snapshot transaction
    bool using_software = false;

    DiagnosticDevice(const std::filesystem::path& shader, bool software)
#if defined(LOXFX_GODOT_NATIVE_VULKAN)
      : context(shader)
#else
      : context(shader, software
                    ? native::AdapterChoice::warp
                    : native::AdapterChoice::hardware)
#endif
    {
#if defined(LOXFX_GODOT_NATIVE_VULKAN)
        // Vulkan standalone selection is controlled by the Vulkan loader and
        // available physical devices, not this Godot diagnostic flag.
        using_software = context.software_device();
#else
        using_software = context.software_adapter();
#endif
    }
};

// An extension may have several LoxFXGroom3D nodes. Share one context for
// each compatible shader asset and adapter choice. Only weak references are
// retained, so the GPU device is destroyed with the final groom user.
// Do not hold locks across Godot callbacks: acquisition is the only operation
// protected by this registry lock.
using DeviceKey = std::pair<std::filesystem::path, bool>;
std::mutex& registry_mutex() {
    static std::mutex value;
    return value;
}
std::map<DeviceKey, std::weak_ptr<DiagnosticDevice>>& registry() {
    static std::map<DeviceKey, std::weak_ptr<DiagnosticDevice>> value;
    return value;
}

std::shared_ptr<DiagnosticDevice> acquire_device(
    const std::filesystem::path& shader, bool software) {
#if defined(LOXFX_GODOT_NATIVE_VULKAN)
    if (software)
        throw std::invalid_argument(
            "Vulkan diagnostic device selection follows the Vulkan loader; "
            "the software_device flag is supported only on D3D12");
#endif
    const DeviceKey key{std::filesystem::absolute(shader).lexically_normal(), software};
    std::lock_guard lock(registry_mutex());
    auto& contexts = registry();
    // Reap old names without extending any device's lifetime.
    for (auto it = contexts.begin(); it != contexts.end(); ) {
        if (it->second.expired()) it = contexts.erase(it);
        else ++it;
    }
    const auto found = contexts.find(key);
    if (found != contexts.end()) {
        if (auto existing = found->second.lock()) return existing;
    }
    // Construct under registry lock: simultaneous groom initialization must
    // not race and accidentally compile two independent device pipelines.
    auto device = std::make_shared<DiagnosticDevice>(key.first, software);
    contexts.insert_or_assign(key, device);
    return device;
}

} // namespace

struct NativeDiagnostic::Impl {
    std::shared_ptr<DiagnosticDevice> device;
    std::unique_ptr<native::GroomState> state;

    Impl(const std::filesystem::path& shader, const PreparedGroom& groom,
         bool software_device)
      : device(acquire_device(shader, software_device)) {
        reset(groom);
    }

    void reset(const PreparedGroom& groom) {
        // Backend diagnostic initialization/uploads may use the same shared
        // command allocator and queue used to step the other grooms.
        std::lock_guard lock(device->commands);
#if defined(LOXFX_GODOT_NATIVE_VULKAN)
        auto prepared = device->context.prepare(groom);
#else
        auto prepared = device->context.prepare_diagnostic(groom);
#endif
        // Keep existing valid state when preparation fails.
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

        // The prototype backend reuses one command buffer/list/allocator
        // and diagnostic queue per DeviceContext. It is NOT independently
        // thread-safe even though groom GPU resources are distinct.
        // Hold this lock through readback, which records and submits a second
        // diagnostic command batch on the same context.
        std::lock_guard lock(device->commands);
        auto& context = device->context;
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
        wind.winds[0] = {wx, wy, wz, 0.0f};
        context.record_length_wind(cmd, *state, wind);
        context.record_followers(cmd, *state);
        // Diagnostic-only CPU synchronization: one completed six-stage
        // compute batch and one readback. Never claim production throughput.
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
    return impl_->step(dt, gravity, damping, rx, ry, rz, wx, wy, wz);
}
const char* NativeDiagnostic::backend_name() const noexcept {
#if defined(LOXFX_GODOT_NATIVE_VULKAN)
    return "Vulkan SDK-owned diagnostic";
#else
    return "D3D12 SDK-owned diagnostic";
#endif
}
bool NativeDiagnostic::software_device() const noexcept {
    return impl_->device->using_software;
}
std::uint32_t NativeDiagnostic::active_context_count() noexcept {
    std::lock_guard lock(registry_mutex());
    std::uint32_t total = 0;
    for (const auto& entry : registry())
        if (!entry.second.expired()) ++total;
    return total;
}

} // namespace loxfx::godot_adapter
