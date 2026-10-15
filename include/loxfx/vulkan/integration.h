// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "loxfx/prepared_groom.h"
#include "loxfx/integration_parameters.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace loxfx::vulkan {

// The FIRST TressFX simulation stage only. This is an executable Vulkan
// integration milestone, not a complete hair simulator. Root transforms,
// bone skinning, constraints, collisions and follower publication follow.
class IntegrationRuntime {
public:
    IntegrationRuntime(const PreparedGroom& groom, const std::filesystem::path& spirv);
    ~IntegrationRuntime();

    IntegrationRuntime(const IntegrationRuntime&) = delete;
    IntegrationRuntime& operator=(const IntegrationRuntime&) = delete;

    // Each batch is one queue submission/fence wait, NOT one per-step readback.
    // Explicit compute->compute memory barriers protect cross-step history.
    // Only guide vertices advance; follower publication is not implemented.
    void advance(const IntegrationParameters& params, std::uint32_t steps = 1);

    // Explicit diagnostic readback, NEVER called from advance().
    [[nodiscard]] std::vector<Float4> read_positions();
    [[nodiscard]] const std::string& device_name() const noexcept;
    [[nodiscard]] bool is_software_device() const noexcept;
    // Timestamp-based duration of the most recent advance batch. Returns
    // -1 if the selected compute queue does not support timestamps.
    [[nodiscard]] double last_gpu_milliseconds() const noexcept;
    // Actual vkAllocateMemory allocation sizes, including persistent staging;
    // these are allocated bytes, not a device-driver residency measurement.
    [[nodiscard]] std::uint64_t allocated_buffer_bytes() const noexcept;
    [[nodiscard]] std::uint64_t staging_allocation_bytes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace loxfx::vulkan
