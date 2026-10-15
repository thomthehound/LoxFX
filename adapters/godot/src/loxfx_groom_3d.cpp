// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
#include "loxfx_groom_3d.h"

#include "loxfx/groom.h"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>

using namespace godot;

namespace loxfx::godot_adapter {

void LoxFXGroom3D::_bind_methods() {
    ClassDB::bind_method(D_METHOD("load_tfx_bytes", "data", "followers_per_guide",
                    "follower_root_radius", "follower_tip_separation", "seed"),
                    &LoxFXGroom3D::load_tfx_bytes, DEFVAL(0), DEFVAL(0.0),
                    DEFVAL(0.0), DEFVAL(1));
    ClassDB::bind_method(D_METHOD("publish_positions", "packed_positions"),
                    &LoxFXGroom3D::publish_positions);
    ClassDB::bind_method(D_METHOD("get_initial_positions"),
                    &LoxFXGroom3D::get_initial_positions);
    ClassDB::bind_method(D_METHOD("get_packed_vertex_count"),
                    &LoxFXGroom3D::get_packed_vertex_count);
    ClassDB::bind_method(D_METHOD("get_authored_guide_count"),
                    &LoxFXGroom3D::get_authored_guide_count);
    ClassDB::bind_method(D_METHOD("get_vertices_per_strand"),
                    &LoxFXGroom3D::get_vertices_per_strand);
    ClassDB::bind_method(D_METHOD("get_followers_per_guide"),
                    &LoxFXGroom3D::get_followers_per_guide);
    ClassDB::bind_method(D_METHOD("is_loaded"),
                    &LoxFXGroom3D::is_loaded);
    ClassDB::bind_method(D_METHOD("get_last_error"),
                    &LoxFXGroom3D::get_last_error);
}

void LoxFXGroom3D::fail(const String& message) {
    error_ = message;
    UtilityFunctions::push_error(message);
}

bool LoxFXGroom3D::load_tfx_bytes(const PackedByteArray& tfx,
        int followers, double radius, double tip, int seed) {
    // Do not disturb a previously loaded groom on a failed import.
    if (followers < 0 ||
        static_cast<std::uint64_t>(followers) > std::numeric_limits<std::uint32_t>::max() ||
        !std::isfinite(radius) || radius < 0.0 ||
        !std::isfinite(tip)) {
        fail("LoxFX: invalid follower preparation options");
        return false;
    }
    const std::span<const std::uint8_t> data(
        tfx.ptr(), static_cast<std::size_t>(tfx.size()));
    auto asset = import_tfx(data);
    if (!asset) {
        fail(String("LoxFX TFX import: ") + String(asset.error().c_str()));
        return false;
    }
    auto prepared = prepare_groom(*asset, {
        .followers_per_guide = static_cast<std::uint32_t>(followers),
        .follower_root_radius = static_cast<float>(radius),
        .follower_tip_separation = static_cast<float>(tip),
        .seed = static_cast<std::uint64_t>(seed)
    });
    if (!prepared) {
        fail(String("LoxFX groom preparation: ") +
             String(prepared.error().c_str()));
        return false;
    }
    // Godot's packed arrays and surface byte regions are indexed with a
    // signed 32-bit count. Reject out-of-range prepared geometries before
    // converting or resizing, even though LoxFX's native 32-bit work index
    // permits a larger range.
    const std::uint64_t points_count = prepared->work.packed_total_vertices;
    const std::uint64_t visible_strands =
        static_cast<std::uint64_t>(prepared->authored_guide_strands) *
        (prepared->followers_per_guide + 1u);
    const std::uint64_t line_vertices =
        visible_strands * (prepared->vertices_per_strand - 1u) * 2u;
    if (points_count > static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max()) ||
        // The update API uses a PackedByteArray of 32-bit XYZ triplets:
        // its byte count, not just the number of vectors, must fit.
        line_vertices > static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max() / (3 * sizeof(float)))) {
        fail("LoxFX: prepared groom exceeds Godot diagnostic geometry limits");
        return false;
    }
    // Replacing the scene geometry is intentionally separate from preparing
    // the neutral asset. No native Vulkan/D3D12 handles enter this node.
    PackedVector3Array initial;
    initial.resize(static_cast<std::int64_t>(prepared->rest_positions.size()));
    if (initial.size() != static_cast<std::int64_t>(
            prepared->rest_positions.size())) {
        fail("LoxFX: unable to allocate prepared diagnostic points");
        return false;
    }
    for (std::int64_t i = 0; i < initial.size(); ++i) {
        const auto& p = prepared->rest_positions[static_cast<std::size_t>(i)];
        initial.set(i, Vector3(p.x, p.y, p.z));
    }
    auto previous = std::move(groom_);
    const auto previous_lines = lines_;
    groom_ = std::move(*prepared);
    lines_.instantiate();
    if (!update_lines(initial, true)) {
        groom_ = std::move(previous);
        lines_ = previous_lines;
        return false;
    }
    set_mesh(lines_);
    error_ = String();
    return true;
}

bool LoxFXGroom3D::update_lines(
        const PackedVector3Array& points, bool first_upload) {
    if (!groom_) return false;
    const auto& g = *groom_;
    if (points.size() != static_cast<std::int64_t>(g.work.packed_total_vertices)) {
        fail("LoxFX: packed position count does not match groom topology");
        return false;
    }
    const std::uint64_t n = g.vertices_per_strand;
    const std::uint64_t visible_strands =
        static_cast<std::uint64_t>(g.authored_guide_strands) *
        (g.followers_per_guide + 1u);
    const std::uint64_t line_vertices = visible_strands * (n - 1u) * 2u;
    if (line_vertices > static_cast<std::uint64_t>(
                std::numeric_limits<std::int32_t>::max() / (3 * sizeof(float)))) {
        fail("LoxFX: groom exceeds diagnostic mesh index limit");
        return false;
    }
    PackedVector3Array vertices;
    vertices.resize(static_cast<std::int64_t>(line_vertices));
    if (vertices.size() != static_cast<std::int64_t>(line_vertices)) {
        fail("LoxFX: unable to allocate diagnostic line vertices");
        return false;
    }
    std::int64_t cursor = 0;
    Vector3 minimum;
    Vector3 maximum;
    bool first = true;
    // Deliberately omit padded guide groups. GPU buffer addresses remain in
    // the source's interleaved guide/follower order, with no reindexing.
    for (std::uint64_t i = 0; i < visible_strands; ++i) {
        const std::uint64_t base = i * n;
        for (std::uint64_t v = 0; v + 1u < n; ++v) {
            const Vector3 a = points[static_cast<std::int64_t>(base + v)];
            const Vector3 b = points[static_cast<std::int64_t>(base + v + 1u)];
            if (!a.is_finite() || !b.is_finite()) {
                fail("LoxFX: nonfinite strand position");
                return false;
            }
            vertices.set(cursor++, a);
            vertices.set(cursor++, b);
            if (first) {
                minimum = a;
                maximum = a;
                first = false;
            }
            minimum = minimum.min(a).min(b);
            maximum = maximum.max(a).max(b);
        }
    }
    if (first_upload) {
        Array arrays;
        arrays.resize(Mesh::ARRAY_MAX);
        arrays[Mesh::ARRAY_VERTEX] = vertices;
        lines_->add_surface_from_arrays(Mesh::PRIMITIVE_LINES, arrays,
                Array(), Dictionary(), Mesh::ARRAY_FLAG_USE_DYNAMIC_UPDATE);
        if (lines_->get_surface_count() != 1) {
            fail("LoxFX: Godot refused to create the diagnostic line surface");
            return false;
        }
    } else {
        // This CPU->Godot update path is for visual diagnostics, not a
        // production renderer or a trustworthy full-frame benchmark.
        PackedByteArray encoded = vertices.to_byte_array();
        const auto expected_bytes =
            static_cast<std::int64_t>(line_vertices * 3 * sizeof(float));
        if (encoded.size() != expected_bytes) {
            // Godot's GPU vertex buffer is float32 XYZ even in double-real
            // engine builds. PackedVector3Array's raw bytes may instead
            // reflect its 64-bit CPU Vector3 precision.
            encoded.resize(expected_bytes);
            if (encoded.size() != expected_bytes) {
                fail("LoxFX: unable to allocate the diagnostic vertex update");
                return false;
            }
            auto* destination = encoded.ptrw();
            for (std::int64_t i = 0; i < vertices.size(); ++i) {
                const Vector3 p = vertices[i];
                const float xyz[3] = {
                    static_cast<float>(p.x),
                    static_cast<float>(p.y),
                    static_cast<float>(p.z)
                };
                std::memcpy(destination + i * sizeof(xyz), xyz, sizeof(xyz));
            }
        }
        lines_->surface_update_vertex_region(0, 0, encoded);
    }
    // Updating vertex buffer bytes does not implicitly update mesh bounds.
    set_custom_aabb(AABB(minimum, maximum - minimum).grow(0.01));
    return true;
}

bool LoxFXGroom3D::publish_positions(
        const PackedVector3Array& packed_positions) {
    if (!groom_ || !lines_.is_valid()) {
        fail("LoxFX: load a groom before publishing positions");
        return false;
    }
    const bool result = update_lines(packed_positions, false);
    if (result) error_ = String();
    return result;
}

PackedVector3Array LoxFXGroom3D::get_initial_positions() const {
    PackedVector3Array positions;
    if (!groom_) return positions;
    positions.resize(static_cast<std::int64_t>(groom_->rest_positions.size()));
    if (positions.size() != static_cast<std::int64_t>(
            groom_->rest_positions.size())) return {};
    for (std::int64_t i = 0; i < positions.size(); ++i) {
        const auto& p = groom_->rest_positions[static_cast<std::size_t>(i)];
        positions.set(i, Vector3(p.x, p.y, p.z));
    }
    return positions;
}
int LoxFXGroom3D::get_packed_vertex_count() const {
    return groom_ ? static_cast<int>(groom_->work.packed_total_vertices) : 0;
}
int LoxFXGroom3D::get_authored_guide_count() const {
    return groom_ ? static_cast<int>(groom_->authored_guide_strands) : 0;
}
int LoxFXGroom3D::get_vertices_per_strand() const {
    return groom_ ? static_cast<int>(groom_->vertices_per_strand) : 0;
}
int LoxFXGroom3D::get_followers_per_guide() const {
    return groom_ ? static_cast<int>(groom_->followers_per_guide) : 0;
}
bool LoxFXGroom3D::is_loaded() const {
    return groom_.has_value();
}
String LoxFXGroom3D::get_last_error() const {
    return error_;
}

} // namespace loxfx::godot_adapter
