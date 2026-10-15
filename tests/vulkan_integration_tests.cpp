// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#include "loxfx/prepared_groom.h"
#include "loxfx/vulkan/integration.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

loxfx::GroomAsset test_groom() {
    loxfx::GroomAsset groom;
    groom.guide_strands = 1;
    groom.vertices_per_strand = 4;
    groom.positions = {{
        0.0f, 0.0f, 0.0f, 0.0f  // guide root immobile
    }, {0, 1, 0, 1}, {0, 2, 0, 1}, {0, 3, 0, 1}};
    return groom;
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        const auto groom = loxfx::prepare_groom(test_groom(), {.followers_per_guide = 1});
        check(groom.has_value(), "prepared fixture");
        loxfx::vulkan::IntegrationRuntime gpu(*groom, argv[1]);
        const auto before = gpu.read_positions();
        check(before.size() == groom->work.packed_total_vertices, "readback vertex count");
        check(before[0].y == 0.0f && before[1].y == 1.0f, "rest state");
        check(before[4].y == 0.0f && before[5].y == 1.0f, "follower initially unchanged");
        constexpr float dt = 1.0f / 60.0f;
        constexpr float g = 9.81f;
        gpu.advance({.time_step = dt, .gravity = g, .damping = 0}, 3);
        const auto after = gpu.read_positions();
        const float delta = g * dt * dt;
        // Verlet from rest with no damping: x_n = x_0 + (1+2+3)*(-g dt²).
        check(std::abs(after[1].y - (1.0f - 6 * delta)) < 0.0001f,
              "three physics steps occurred on the GPU");
        check(after[0].y == 0.0f, "root stayed pinned");
        check(after[5].y == 1.0f, "followers not yet published");

        // Confirm a second command submission observes prior GPU writes.
        gpu.advance({.time_step = dt, .gravity = g, .damping = 0}, 1);
        const auto fourth = gpu.read_positions();
        check(std::abs(fourth[1].y - (1.0f - 10 * delta)) < 0.0001f,
              "state survives a second GPU batch submission");
        // Source-style global rest-shape stiffness, selected over all vertices.
        gpu.advance({.time_step = dt, .gravity = g, .damping = 0,
                     .global_stiffness = 1.0f, .global_range = 1.0f}, 1);
        const auto restored = gpu.read_positions();
        check(std::abs(restored[1].y - 1.0f) < 0.0001f,
              "global shape constraint restores authored geometry");
        check(restored[0].y == 0.0f, "pinned root remains stable");

        check(gpu.resident_buffer_bytes() ==
              3 * groom->work.packed_total_vertices * sizeof(loxfx::Float4),
              "resident buffer accounting");
        std::cout << "Vulkan device: " << gpu.device_name()
                  << (gpu.is_software_device() ? " (software)" : " (hardware/unknown)")
                  << "\nPrepared vertices: " << groom->work.packed_total_vertices
                  << "\nPersistent device bytes: " << gpu.resident_buffer_bytes()
                  << "\nThree-stage-1-steps GPU milliseconds: "
                  << gpu.last_gpu_milliseconds() << "\n";
        if (gpu.last_gpu_milliseconds() < 0)
            std::cout << "Timestamp unsupported on selected queue\n";
        std::cout << "PASS: GPU-integration stage geometry/history/root tests\n";
        return 0;
    } catch (const std::exception& err) {
        std::cerr << "FAIL: " << err.what() << "\n";
        return 1;
    }
}
