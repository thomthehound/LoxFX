// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
// INTERNAL experimental interface; not part of installed LoxFX::Core.
#pragma once

#include "loxfx/integration_parameters.h"
#include "loxfx/length_wind.h"
#include "loxfx/native_frame_parameters.h"
#include "loxfx/prepared_groom.h"
#include "loxfx/strand_motion.h"
#include <d3d12.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace loxfx::d3d12::smoke {

enum class AdapterChoice { hardware, warp };

struct ExternalDevice {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* compute_queue = nullptr;
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
    [[nodiscard]] ID3D12Resource* positions_buffer() const noexcept;
    [[nodiscard]] ID3D12Resource* tangents_buffer() const noexcept;
private:
    friend class DeviceContext;
    struct Impl;
    explicit GroomState(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};

// An upload transaction that owns both pending groom state and upload
// heap until an externally submitted command list finishes. The host must
// keep the object alive during GPU execution and call finish ONLY after its
// completion fence; doing so never submits or waits for GPU work.
class PendingGroomUpload {
public:
    ~PendingGroomUpload();
    PendingGroomUpload(PendingGroomUpload&&) noexcept;
    PendingGroomUpload& operator=(PendingGroomUpload&&) noexcept;
    PendingGroomUpload(const PendingGroomUpload&) = delete;
    PendingGroomUpload& operator=(const PendingGroomUpload&) = delete;
    [[nodiscard]] GroomState finish_after_completion() &&;
private:
    friend class DeviceContext;
    struct Impl;
    explicit PendingGroomUpload(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};

class DeviceContext {
public:
    // Standalone chooses a device ONCE. WARP is for functional CI only.
    DeviceContext(const std::filesystem::path& dxil,
                  AdapterChoice adapter = AdapterChoice::hardware);
    // Host controls its chosen device and queue; COM references maintain
    // handle lifetimes, but the host must synchronize use of its queue.
    DeviceContext(ExternalDevice external, const std::filesystem::path& dxil);
    ~DeviceContext();
    DeviceContext(DeviceContext&&) noexcept;
    DeviceContext& operator=(DeviceContext&&) noexcept;
    DeviceContext(const DeviceContext&) = delete;
    DeviceContext& operator=(const DeviceContext&) = delete;

    // Diagnostic upload waits explicitly once; NO per-stage CPU readback.
    [[nodiscard]] PendingGroomUpload prepare_upload(const PreparedGroom& groom);
    void record_initial_upload(ID3D12GraphicsCommandList* command_list,
                               PendingGroomUpload& upload) const;
    // Explicitly blocking convenience for diagnostic users only.
    [[nodiscard]] GroomState prepare_diagnostic(const PreparedGroom& groom);
    // Record the six LoxFX passes on a caller-owned command list.
    // No submission, waits, GPU readback or command-list ownership.
    // The host must discard a partially recorded list on validation error.
    void record_frame(ID3D12GraphicsCommandList* command_list,
                      const GroomState& groom,
                      const NativeFrameParameters& parameters) const;
    // Backend-native work recording: no queue submission, fence or CPU wait.
    // Applies strand-level VSP transforms to guide vertices 2+.
    // Caller records integration and strand-level calculation first.
    // Guide-local shape correction, following strand motion and VSP.
    // Stiffness is [0,1]; iterations come from the prepared work plan.
    // Stage-five length/wind pass; collision is handled separately.
    // Final pass publishes follower positions and per-vertex tangents.
    void record_followers(ID3D12GraphicsCommandList* command_list,
                          const GroomState& groom) const;
    void record_length_wind(ID3D12GraphicsCommandList* command_list,
                            const GroomState& groom,
                            const LengthWindParameters& parameters) const;
    void record_local_shape(ID3D12GraphicsCommandList* command_list,
                            const GroomState& groom, float stiffness) const;
    void record_velocity_shock(ID3D12GraphicsCommandList* command_list,
                               const GroomState& groom) const;
    void record_strand_level(ID3D12GraphicsCommandList* command_list,
                             const GroomState& groom,
                             const StrandMotionParameters& parameters) const;
    void record_integration(ID3D12GraphicsCommandList* command_list,
                            const GroomState& groom,
                            const IntegrationParameters& parameters,
                            std::uint32_t steps = 1) const;

    ID3D12GraphicsCommandList* begin_diagnostic_commands();
    void submit_diagnostic_and_wait();
    [[nodiscard]] std::vector<Float4> diagnostic_read_positions(const GroomState& groom);
    [[nodiscard]] std::vector<Float4> diagnostic_read_tangents(const GroomState& groom);
    [[nodiscard]] std::vector<Float4> diagnostic_read_root_history(const GroomState& groom);
    [[nodiscard]] std::vector<StrandMotionData> diagnostic_read_strand_data(const GroomState& groom);
    [[nodiscard]] double last_diagnostic_gpu_milliseconds() const noexcept;
    [[nodiscard]] std::uint64_t diagnostic_submission_count() const noexcept;
    [[nodiscard]] ID3D12Device* native_device() const noexcept;
    [[nodiscard]] ExternalDevice borrowed_handles() const noexcept;
    [[nodiscard]] const std::string& adapter_name() const noexcept;
    [[nodiscard]] bool software_adapter() const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace loxfx::d3d12::smoke
