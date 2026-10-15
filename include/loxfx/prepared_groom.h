// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "loxfx/groom.h"
#include "loxfx/simulation_work.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace loxfx {

// Preparation happens at load time, not on the per-frame simulation path.
// Follow strands are optional; keep the original authored GroomAsset unchanged.
struct GroomPreparation {
    std::uint32_t followers_per_guide = 0;
    std::uint32_t local_shape_iterations = 2;
    float follower_root_radius = 0.0f;
    float follower_tip_separation = 0.0f;
    std::uint64_t seed = 1;
};

// The reference-compatible interleaved layout is:
//
// [guide 0 vertices][followers of guide 0][guide 1 vertices][followers...]
//
// Each guide's first vertex is at:
// guide_index * (followers_per_guide + 1) * vertices_per_strand.
//
// Static rest lengths and initial tangents are produced only once. Dynamic
// positions, previous positions, and previous-previous positions will be
// initialized from rest_positions by the GPU executor; they are not allocated
// or recopied by this type on every step.
struct PreparedGroom {
    SimulationWorkPlan work;
    std::uint32_t authored_guide_strands = 0;
    std::uint32_t vertices_per_strand = 0;
    std::uint32_t followers_per_guide = 0;
    float follower_tip_separation = 0.0f;

    // GPU-bound, strand-major arrays. All are sized to work.packed_total_vertices
    // except follower_root_offsets and source_guide_indices, which are
    // work.packed_total_strands long.
    std::vector<Float4> rest_positions;
    std::vector<Float4> rest_tangents;
    std::vector<float> rest_segment_lengths;
    std::vector<Float4> follower_root_offsets;

    // Source provenance / visibility information, NOT a per-step GPU buffer.
    // Padded guide strands map to the final authored guide; rendering and
    // export must exclude indices beyond authored_guide_strands.
    std::vector<std::uint32_t> source_guide_indices;

    [[nodiscard]] std::size_t static_buffer_bytes() const noexcept;
};

std::expected<PreparedGroom, std::string>
prepare_groom(const GroomAsset& asset, const GroomPreparation& options = {});

// Public PreparedGroom is mutable. Validate once at an execution/serialization
// boundary, rather than assuming that every caller used prepare_groom().
// Shared by all backends; no native GPU types or per-frame validation.
std::expected<void, std::string> validate_prepared_groom(const PreparedGroom& groom);

} // namespace loxfx
