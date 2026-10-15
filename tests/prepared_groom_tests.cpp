// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#include "loxfx/prepared_groom.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

loxfx::GroomAsset fixture(std::uint32_t guide_count = 2) {
    loxfx::GroomAsset g;
    g.guide_strands = guide_count;
    g.vertices_per_strand = 4;
    for (std::uint32_t s = 0; s < guide_count; ++s) {
        for (std::uint32_t v = 0; v < 4; ++v)
            g.positions.push_back({static_cast<float>(s), static_cast<float>(v),
                                   0.0f, v == 0 ? 0.0f : 1.0f});
    }
    return g;
}

void no_followers() {
    const auto source = fixture();
    auto prepared = loxfx::prepare_groom(source);
    check(prepared.has_value(), "source groom prepares");
    check(prepared->authored_guide_strands == 2, "authored count");
    check(prepared->work.packed_guide_strands == 64, "GPU padding only");
    check(prepared->work.packed_total_vertices == 256, "vertex storage");
    check(source.positions.size() == 8, "source unchanged");
    check(prepared->rest_positions[4].x == 1.0f, "second source guide");
    check(prepared->rest_positions[8].x == 1.0f, "padded guide duplicates last");
    check(prepared->rest_positions[0].w == 0.0f, "pinned root");
    check(prepared->rest_positions[1].w == 1.0f, "movable vertex");
    check(prepared->rest_segment_lengths[0] == 1.0f, "rest segment length");
    check(prepared->rest_segment_lengths[3] == 0.0f, "tip rest length");
    check(prepared->rest_tangents[3].y == 1.0f, "defined tip tangent");
    check(prepared->source_guide_indices[0] == 0, "first guide provenance");
    check(prepared->source_guide_indices[2] == 1, "padded provenance");
    check(prepared->static_buffer_bytes() ==
          256 * (2 * sizeof(loxfx::Float4) + sizeof(float)) +
          64 * sizeof(loxfx::Float4), "static GPU buffer accounting");
}

void followers_and_repeatability() {
    const auto source = fixture(1);
    const loxfx::GroomPreparation settings{3, 2, 0.08f, 0.4f, 123};
    auto prepared = loxfx::prepare_groom(source, settings);
    auto repeated = loxfx::prepare_groom(source, settings);
    check(prepared && repeated, "follower groom prepares");
    check(prepared->work.packed_total_strands == 256, "64 * 4");
    check(prepared->rest_positions.size() == 1024, "4 vertices/strand");
    check(prepared->rest_positions[0].x == 0.0f, "unmodified guide");
    check(prepared->rest_positions[4].w == 0.0f, "follower pinning retained");
    check(prepared->follower_root_offsets[0].x == 0.0f, "master has no offset");
    check(prepared->follower_root_offsets[1].x == repeated->follower_root_offsets[1].x,
          "offset x deterministic");
    check(prepared->follower_root_offsets[1].z == repeated->follower_root_offsets[1].z,
          "offset z deterministic");
    const auto& offset = prepared->follower_root_offsets[1];
    check(prepared->follower_root_offsets[0].w == 0.0f,
          "guide root offset has no tip expansion");
    check(offset.w == settings.follower_tip_separation,
          "follower tip expansion travels with compact offset metadata");
    check(std::abs(offset.x) <= settings.follower_root_radius + 0.0001f,
          "offset within square bounds");
    check(std::abs(offset.z) <= settings.follower_root_radius + 0.0001f,
          "offset within square bounds z");
    check(std::abs(prepared->rest_positions[4].x - offset.x) < 0.0001f,
          "follower root matches offset");
    const float final_factor = 1.0f + settings.follower_tip_separation * 3.0f / 4.0f;
    check(std::abs(prepared->rest_positions[7].x - offset.x * final_factor) < 0.0001f,
          "tip separation matches source formula");
    check(prepared->source_guide_indices[255] == 0, "padded source id");
}

void validate_mutated_buffers() {
    auto prepared = loxfx::prepare_groom(fixture());
    check(prepared.has_value(), "prepared fixture for validator");
    check(loxfx::validate_prepared_groom(*prepared).has_value(),
          "valid prepared groom accepted");

    auto changed = *prepared;
    changed.work.packed_guide_strands += 64;
    check(!loxfx::validate_prepared_groom(changed),
          "reject mismatched padded guide count");
    changed = *prepared;
    changed.work.planned_lane_invocations += 64;
    check(!loxfx::validate_prepared_groom(changed),
          "reject altered dispatch accounting");
    changed = *prepared;
    changed.rest_tangents.pop_back();
    check(!loxfx::validate_prepared_groom(changed),
          "reject missing rest tangents");
    changed = *prepared;
    changed.rest_segment_lengths[0] = -1.0f;
    check(!loxfx::validate_prepared_groom(changed),
          "reject invalid rest segment length");
    changed = *prepared;
    changed.follower_root_offsets[0].x =
        std::numeric_limits<float>::infinity();
    check(!loxfx::validate_prepared_groom(changed),
          "reject invalid follower offset");
    changed = *prepared;
    changed.source_guide_indices[0] = changed.authored_guide_strands;
    check(!loxfx::validate_prepared_groom(changed),
          "reject out-of-range source guide index");
}

void invalid_inputs() {
    auto source = fixture();
    source.positions.pop_back();
    check(!loxfx::prepare_groom(source), "incorrect authored vertex count");
    source = fixture();
    source.positions[0].x = std::numeric_limits<float>::quiet_NaN();
    check(!loxfx::prepare_groom(source), "reject nonfinite geometry");
    source = fixture();
    check(!loxfx::prepare_groom(source, {0, 2, -1.0f, 0.0f, 1}),
          "reject negative radius");
    check(!loxfx::prepare_groom(source, {0, 2, std::numeric_limits<float>::infinity(), 0.0f, 1}),
          "reject nonfinite radius");
    check(!loxfx::prepare_groom(source, {0, 2, 0.0f, std::numeric_limits<float>::quiet_NaN(), 1}),
          "reject nonfinite spread");
    source = fixture();
    source.vertices_per_strand = 32;
    check(!loxfx::prepare_groom(source), "topology must match positions");
}

} // namespace

int main() {
    try {
        no_followers();
        followers_and_repeatability();
        invalid_inputs();
        validate_mutated_buffers();
        std::cout << "Prepared groom tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
