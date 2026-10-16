// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
//
// INTERNAL EXPERIMENTAL INTERFACE: not installed as SDK API.
#pragma once
#include "loxfx/integration_parameters.h"
#include "loxfx/length_wind.h"
#include "loxfx/native_frame_parameters.h"
#include "loxfx/prepared_groom.h"
#include "loxfx/strand_motion.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace loxfx::vulkan::smoke {

// Internal smoke-test hook. Failure is injected just before an initialization
// resource operation; -1 disables. Never active in frame-time recording.
void diagnostic_fail_creation_after(int checkpoints) noexcept;

// Borrowed handles: the host must keep its device/queue alive until all
// dependent contexts and grooms have been destroyed. No implicit migration.
struct ExternalDevice {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice logical = VK_NULL_HANDLE;
    VkQueue compute_queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
};

class GroomState {
public:
    ~GroomState();
    GroomState(GroomState&&) noexcept;
    GroomState& operator=(GroomState&&) noexcept;
    GroomState(const GroomState&) = delete;
    GroomState& operator=(const GroomState&) = delete;
    [[nodiscard]] std::uint64_t allocated_buffer_bytes() const noexcept;
    // Borrowed resource handles for a host renderer; no ownership transfer.
    // Synchronization and compute-to-graphics visibility remain host-owned.
    [[nodiscard]] VkBuffer positions_buffer() const noexcept;
    [[nodiscard]] VkBuffer tangents_buffer() const noexcept;
private:
    friend class DeviceContext;
    struct Impl;
    explicit GroomState(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// The device context owns invariant pipeline/descriptor infrastructure.
// Several grooms share ONE context. record_integration() only records
// commands; queue submission is explicitly owned by caller/test harness.
class DeviceContext {
public:
    // Standalone smoke path: selects a compatible device once.
    explicit DeviceContext(const std::filesystem::path& spirv);
    // Engine-hosted path: borrow a caller-selected device and queue.
    DeviceContext(ExternalDevice external, const std::filesystem::path& spirv);
    ~DeviceContext();
    DeviceContext(DeviceContext&&) noexcept;
    DeviceContext& operator=(DeviceContext&&) noexcept;
    DeviceContext(const DeviceContext&) = delete;
    DeviceContext& operator=(const DeviceContext&) = delete;

    [[nodiscard]] GroomState prepare(const PreparedGroom& source);
    // Records all six source-ordered stages into a CALLER-owned command buffer.
    // Does not begin/end, submit, wait, read back or allocate frame commands.
    // Caller must discard its partially recorded command buffer on error,
    // and provide correct completion/graphics-visibility synchronization.
    void record_frame(VkCommandBuffer command_buffer,
                      const GroomState& groom,
                      const NativeFrameParameters& parameters) const;
    // Stage 2 consumes integration outputs; records barrier and one
    // guide-strand dispatch, with no submit/wait/readback.
    // Applies strand-level VSP transforms to guide vertices 2+.
    // Caller records integration and strand-level calculation first.
    // Guide-local shape correction, following strand motion and VSP.
    // Stiffness is [0,1]; iterations come from the prepared work plan.
    // Stage-five length/wind pass; collision is handled separately.
    // Final pass publishes follower positions and per-vertex tangents.
    void record_followers(VkCommandBuffer command_buffer,
                          const GroomState& groom) const;
    void record_length_wind(VkCommandBuffer command_buffer,
                            const GroomState& groom,
                            const LengthWindParameters& parameters) const;
    void record_local_shape(VkCommandBuffer command_buffer,
                            const GroomState& groom, float stiffness) const;
    void record_velocity_shock(VkCommandBuffer command_buffer,
                               const GroomState& groom) const;
    void record_strand_level(VkCommandBuffer command_buffer,
                             const GroomState& groom,
                             const StrandMotionParameters& parameters) const;
    void record_integration(VkCommandBuffer command_buffer,
                            const GroomState& groom,
                            const IntegrationParameters& params,
                            std::uint32_t steps = 1) const;

    // Diagnostic-only orchestration below. Actual host applications submit
    // the VkCommandBuffer passed to record_integration themselves.
    VkCommandBuffer begin_diagnostic_commands();
    void submit_diagnostic_and_wait();
    [[nodiscard]] std::vector<Float4> diagnostic_read_positions(const GroomState& groom);
    [[nodiscard]] std::vector<Float4> diagnostic_read_tangents(const GroomState& groom);
    [[nodiscard]] std::vector<Float4> diagnostic_read_root_history(const GroomState& groom);
    [[nodiscard]] std::vector<StrandMotionData> diagnostic_read_strand_data(const GroomState& groom);
    [[nodiscard]] double last_diagnostic_gpu_milliseconds() const noexcept;
    [[nodiscard]] const std::string& device_name() const noexcept;
    [[nodiscard]] bool software_device() const noexcept;
    [[nodiscard]] VkDevice native_device() const noexcept;
    // For testing explicit borrowed-device construction. Caller keeps
    // originating device/queue alive and serializes access to its queue.
    [[nodiscard]] ExternalDevice borrowed_handles() const noexcept;
    [[nodiscard]] std::uint64_t gpu_submission_count() const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace loxfx::vulkan::smoke
