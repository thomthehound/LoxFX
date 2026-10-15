// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#include "loxfx/prepared_groom.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>

namespace loxfx {
namespace {

struct Vec3 {
    float x, y, z;
};

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }

Vec3 xyz(Float4 a) { return {a.x, a.y, a.z}; }

float square_length(Vec3 a) { return a.x*a.x + a.y*a.y + a.z*a.z; }

Vec3 unit(Vec3 a, Vec3 fallback = {0.0f, 1.0f, 0.0f}) {
    const float length_squared = square_length(a);
    if (!(length_squared > 1.0e-20f) || !std::isfinite(length_squared))
        return fallback;
    const float inv_len = 1.0f / std::sqrt(length_squared);
    return a * inv_len;
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y*b.z - a.z*b.y,
            a.z*b.x - a.x*b.z,
            a.x*b.y - a.y*b.x};
}

Float4 packed(Vec3 p, float w) { return {p.x, p.y, p.z, w}; }

bool finite(Float4 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) &&
           std::isfinite(v.z) && std::isfinite(v.w);
}

// A tiny reproducible per-asset generator; never called during simulation.
std::uint64_t random_word(std::uint64_t& state) {
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t x = state;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

float signed_uniform(std::uint64_t& state) {
    constexpr float inv_24 = 1.0f / 16777216.0f;
    const auto fraction = static_cast<std::uint32_t>(random_word(state) >> 40);
    return 2.0f * (static_cast<float>(fraction) * inv_24) - 1.0f;
}

std::expected<PreparedGroom, std::string>
prepare_impl(const GroomAsset& asset, const GroomPreparation& options) {
    if (!std::isfinite(options.follower_root_radius) ||
        options.follower_root_radius < 0.0f ||
        !std::isfinite(options.follower_tip_separation)) {
        return std::unexpected("invalid follower radius or tip separation");
    }
    auto work = make_simulation_work_plan({
        asset.guide_strands, asset.vertices_per_strand,
        options.followers_per_guide, options.local_shape_iterations
    });
    if (!work) return std::unexpected(work.error());

    const std::size_t authored_vertices =
        static_cast<std::size_t>(asset.guide_strands) * asset.vertices_per_strand;
    if (asset.positions.size() != authored_vertices)
        return std::unexpected("authored position count does not match groom topology");
    for (const Float4 pos : asset.positions) {
        if (!finite(pos))
            return std::unexpected("authored positions contain non-finite components");
    }

    PreparedGroom prepared{
        .work = *work,
        .authored_guide_strands = asset.guide_strands,
        .vertices_per_strand = asset.vertices_per_strand,
        .followers_per_guide = options.followers_per_guide
    };

    const std::size_t count = work->packed_total_vertices;
    const std::size_t strands = work->packed_total_strands;
    prepared.rest_positions.resize(count);
    prepared.rest_tangents.resize(count);
    prepared.rest_segment_lengths.resize(count);
    prepared.follower_root_offsets.resize(strands);
    prepared.source_guide_indices.resize(strands);

    const std::size_t n = asset.vertices_per_strand;
    const std::size_t f = static_cast<std::size_t>(options.followers_per_guide);
    std::uint64_t random_state = options.seed;

    for (std::size_t guide = 0; guide < work->packed_guide_strands; ++guide) {
        const std::size_t source_guide =
            std::min(guide, static_cast<std::size_t>(asset.guide_strands - 1));
        const std::size_t source_base = source_guide * n;
        const std::size_t first_strand = guide * (f + 1);
        const Vec3 initial_direction = unit(
            xyz(asset.positions[source_base + 1]) - xyz(asset.positions[source_base]));
        // Tangent basis is built without trigonometry; its orientation is stable.
        const Vec3 t0 = std::abs(initial_direction.z) > 0.70710678f
            ? unit({0.0f, -initial_direction.z, initial_direction.y})
            : unit({-initial_direction.y, initial_direction.x, 0.0f});
        const Vec3 t1 = unit(cross(initial_direction, t0), {1.0f, 0.0f, 0.0f});

        for (std::size_t strand_offset = 0; strand_offset <= f; ++strand_offset) {
            const std::size_t packed_strand = first_strand + strand_offset;
            const std::size_t base = packed_strand * n;
            prepared.source_guide_indices[packed_strand] =
                static_cast<std::uint32_t>(source_guide);

            Vec3 offset{0, 0, 0};
            if (strand_offset != 0 && options.follower_root_radius != 0.0f) {
                const float u = signed_uniform(random_state);
                const float v = signed_uniform(random_state);
                offset = (t0 * u + t1 * v) * options.follower_root_radius;
            }
            prepared.follower_root_offsets[packed_strand] = packed(offset, 0);

            for (std::size_t vertex = 0; vertex < n; ++vertex) {
                const Float4 original = asset.positions[source_base + vertex];
                const float tip_factor = 1.0f +
                    options.follower_tip_separation *
                    (static_cast<float>(vertex) / static_cast<float>(n));
                const Vec3 p = xyz(original) + offset * tip_factor;
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                    return std::unexpected("follower geometry exceeds finite float range");
                prepared.rest_positions[base + vertex] = packed(p, original.w);
            }

            // Every strand, including padded and generated ones, gets a
            // defined tip tangent and final zero rest length.
            for (std::size_t vertex = 0; vertex < n; ++vertex) {
                const Vec3 here = xyz(prepared.rest_positions[base + vertex]);
                const Vec3 next = xyz(prepared.rest_positions[base + std::min(vertex + 1, n - 1)]);
                const Vec3 prior = xyz(prepared.rest_positions[base + (vertex == 0 ? 0 : vertex - 1)]);
                const Vec3 forward = unit(next - here);
                const Vec3 backward = unit(here - prior);
                const Vec3 tangent = vertex == 0 ? forward :
                    (vertex == n - 1 ? backward : unit(forward + backward, forward));
                prepared.rest_tangents[base + vertex] = packed(tangent, 0.0f);
                const Vec3 delta = next - here;
                const float length = vertex == n - 1 ? 0.0f : std::sqrt(square_length(delta));
                if (!std::isfinite(length))
                    return std::unexpected("rest segment length exceeds finite float range");
                prepared.rest_segment_lengths[base + vertex] = length;
            }
        }
    }

    return prepared;
}

} // namespace

std::size_t PreparedGroom::static_buffer_bytes() const noexcept {
    return rest_positions.size() * sizeof(Float4) +
           rest_tangents.size() * sizeof(Float4) +
           rest_segment_lengths.size() * sizeof(float) +
           follower_root_offsets.size() * sizeof(Float4);
}

std::expected<PreparedGroom, std::string>
prepare_groom(const GroomAsset& asset, const GroomPreparation& options) {
    try {
        return prepare_impl(asset, options);
    } catch (const std::bad_alloc&) {
        return std::unexpected("allocation failed preparing groom");
    } catch (const std::length_error&) {
        return std::unexpected("prepared groom exceeds array limits");
    }
}

} // namespace loxfx
