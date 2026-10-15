// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
//
// INTERNAL EXPERIMENTAL INTERFACE: not installed as SDK API.
#pragma once
#include "loxfx/integration_parameters.h"
#include "loxfx/prepared_groom.h"
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
    void record_integration(VkCommandBuffer command_buffer,
                            const GroomState& groom,
                            const IntegrationParameters& params,
                            std::uint32_t steps = 1) const;

    // Diagnostic-only orchestration below. Actual host applications submit
    // the VkCommandBuffer passed to record_integration themselves.
    VkCommandBuffer begin_diagnostic_commands();
    void submit_diagnostic_and_wait();
    [[nodiscard]] std::vector<Float4> diagnostic_read_positions(const GroomState& groom);
    [[nodiscard]] std::vector<Float4> diagnostic_read_root_history(const GroomState& groom);
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
