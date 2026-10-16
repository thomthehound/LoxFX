// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
#include "loxfx_groom_3d.h"

#include "loxfx/groom.h"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <filesystem>
#include <span>
#include <exception>
#include <string>

using namespace godot;

namespace loxfx::godot_adapter {

LoxFXGroom3D::~LoxFXGroom3D() = default;

void LoxFXGroom3D::_bind_methods() {
    ClassDB::bind_method(D_METHOD("start_native_diagnostic",
                    "shader_file", "software_device"),
                    &LoxFXGroom3D::start_native_diagnostic, DEFVAL(false));
    ClassDB::bind_method(D_METHOD("step_native_diagnostic",
                    "delta_seconds", "gravity", "damping",
                    "root_translation", "wind"),
                    &LoxFXGroom3D::step_native_diagnostic,
                    DEFVAL(9.81), DEFVAL(0.08),
                    DEFVAL(Vector3()), DEFVAL(Vector3()));
    ClassDB::bind_method(D_METHOD("reset_native_diagnostic"),
                    &LoxFXGroom3D::reset_native_diagnostic);
    ClassDB::bind_method(D_METHOD("stop_native_diagnostic"),
                    &LoxFXGroom3D::stop_native_diagnostic);
    ClassDB::bind_method(D_METHOD("is_native_diagnostic_active"),
                    &LoxFXGroom3D::is_native_diagnostic_active);
    ClassDB::bind_method(D_METHOD("get_native_diagnostic_steps"),
                    &LoxFXGroom3D::get_native_diagnostic_steps);
    ClassDB::bind_method(D_METHOD("get_native_diagnostic_context_count"),
                    &LoxFXGroom3D::get_native_diagnostic_context_count);
    ClassDB::bind_method(D_METHOD("get_native_diagnostic_root_position"),
                    &LoxFXGroom3D::get_native_diagnostic_root_position);
    ClassDB::bind_method(D_METHOD("get_native_diagnostic_backend"),
                    &LoxFXGroom3D::get_native_diagnostic_backend);
    ClassDB::bind_method(D_METHOD("get_renderer_interop_status"),
                    &LoxFXGroom3D::get_renderer_interop_status);
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
    // A newly loaded groom invalidates the previous GPU state, but a failed
    // load above leaves the previous state and visualization intact.
    stop_native_diagnostic();
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


bool LoxFXGroom3D::start_native_diagnostic(const String& shader_file,
                                            bool software_device) {
#if defined(LOXFX_GODOT_NATIVE_DIAGNOSTIC)
    if (!groom_) {
        fail("LoxFX: load a groom before starting native simulation");
        return false;
    }
    if (shader_file.is_empty()) {
        fail("LoxFX: native shader filename is required");
        return false;
    }
    try {
        const String resolved = shader_file.begins_with("res://") ||
                                shader_file.begins_with("user://")
            ? ProjectSettings::get_singleton()->globalize_path(shader_file)
            : shader_file;
        const CharString utf8 = resolved.utf8();
        const auto* bytes = reinterpret_cast<const char8_t*>(utf8.get_data());
        std::filesystem::path shader{std::u8string(bytes)};
        // Construct fully before replacing previous state. A failure never
        // destroys the preceding valid simulation or its diagnostic mesh.
        auto replacement = std::make_unique<NativeDiagnostic>(
            shader, *groom_, software_device);
        native_ = std::move(replacement);
        native_frames_ = 0;
        native_last_root_ = Vector3(groom_->rest_positions[0].x,
                                    groom_->rest_positions[0].y,
                                    groom_->rest_positions[0].z);
        native_root_valid_ = true;
        error_ = String();
        return true;
    } catch (const std::exception& error) {
        fail(String("LoxFX native diagnostic startup: ") +
             String::utf8(error.what()));
        return false;
    }
#else
    (void)shader_file;
    (void)software_device;
    fail("LoxFX: extension was built without native diagnostic GPU support");
    return false;
#endif
}

bool LoxFXGroom3D::step_native_diagnostic(
        double delta_seconds, double gravity, double damping,
        const Vector3& root_translation, const Vector3& wind) {
#if defined(LOXFX_GODOT_NATIVE_DIAGNOSTIC)
    if (!native_ || !groom_) {
        fail("LoxFX: native diagnostic simulation is not started");
        return false;
    }
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0 ||
        delta_seconds > 0.1 || !std::isfinite(gravity) ||
        !std::isfinite(damping) || damping < 0.0 || damping > 1.0 ||
        !root_translation.is_finite() || !wind.is_finite()) {
        fail("LoxFX: invalid native frame parameters");
        return false;
    }
    try {
        const auto snapshot = native_->step(
            static_cast<float>(delta_seconds),static_cast<float>(gravity),
            static_cast<float>(damping),
            static_cast<float>(root_translation.x),
            static_cast<float>(root_translation.y),
            static_cast<float>(root_translation.z),
            static_cast<float>(wind.x),static_cast<float>(wind.y),
            static_cast<float>(wind.z));
        if (snapshot.size() != groom_->work.packed_total_vertices) {
            fail("LoxFX: native output did not match the prepared groom");
            return false;
        }
        PackedVector3Array points;
        points.resize(static_cast<std::int64_t>(snapshot.size()));
        if (points.size() != static_cast<std::int64_t>(snapshot.size())) {
            fail("LoxFX: cannot allocate diagnostic frame positions");
            return false;
        }
        for (std::int64_t i=0; i<points.size(); ++i) {
            const auto& v=snapshot[static_cast<std::size_t>(i)];
            points.set(i, Vector3(v.x,v.y,v.z));
        }
        if (!publish_positions(points)) return false;
        native_last_root_ = Vector3(snapshot[0].x,snapshot[0].y,snapshot[0].z);
        native_root_valid_ = true;
        ++native_frames_;
        return true;
    } catch(const std::exception& error) {
        fail(String("LoxFX native diagnostic step: ") +
             String::utf8(error.what()));
        return false;
    }
#else
    (void)delta_seconds;(void)gravity;(void)damping;
    (void)root_translation;(void)wind;
    fail("LoxFX: extension was built without native diagnostic GPU support");
    return false;
#endif
}

bool LoxFXGroom3D::reset_native_diagnostic() {
#if defined(LOXFX_GODOT_NATIVE_DIAGNOSTIC)
    if (!native_ || !groom_) {
        fail("LoxFX: native diagnostic simulation is not started");
        return false;
    }
    try {
        native_->reset(*groom_);
        native_frames_=0;
        const bool published = publish_positions(get_initial_positions());
        if (published) {
            native_last_root_ = Vector3(groom_->rest_positions[0].x,
                                       groom_->rest_positions[0].y,
                                       groom_->rest_positions[0].z);
            native_root_valid_ = true;
        }
        return published;
    } catch (const std::exception& error) {
        fail(String("LoxFX native diagnostic reset: ") +
             String::utf8(error.what()));
        return false;
    }
#else
    fail("LoxFX: extension was built without native diagnostic GPU support");
    return false;
#endif
}

void LoxFXGroom3D::stop_native_diagnostic() {
#if defined(LOXFX_GODOT_NATIVE_DIAGNOSTIC)
    native_.reset();
    native_frames_ = 0;
    native_root_valid_ = false;
#endif
}

bool LoxFXGroom3D::is_native_diagnostic_active() const {
#if defined(LOXFX_GODOT_NATIVE_DIAGNOSTIC)
    return native_ != nullptr;
#else
    return false;
#endif
}

int LoxFXGroom3D::get_native_diagnostic_steps() const {
#if defined(LOXFX_GODOT_NATIVE_DIAGNOSTIC)
    return static_cast<int>(std::min(
        native_frames_, static_cast<std::uint64_t>(
            std::numeric_limits<int>::max())));
#else
    return 0;
#endif
}

int LoxFXGroom3D::get_native_diagnostic_context_count() const {
#if defined(LOXFX_GODOT_NATIVE_DIAGNOSTIC)
    return static_cast<int>(NativeDiagnostic::active_context_count());
#else
    return 0;
#endif
}

Vector3 LoxFXGroom3D::get_native_diagnostic_root_position() const {
#if defined(LOXFX_GODOT_NATIVE_DIAGNOSTIC)
    if (native_ && native_root_valid_) return native_last_root_;
#endif
    return {};
}

Dictionary LoxFXGroom3D::get_renderer_interop_status() const {
    Dictionary report;
    // No API-specific Vulkan or D3D12 handles cross this boundary.
    // get_rendering_device() can be null on Compatibility/headless hosts.
    // This method intentionally does not touch RenderingDevice directly or
    // call get_driver_resource from the Godot scene thread. Native resource
    // access requires a reviewed render-thread/command ownership contract.
    const auto* rendering = RenderingServer::get_singleton();
    const bool present = rendering && rendering->get_rendering_device();
    report["renderer_device_available"] = present;
    report["renderer_adapter_name"] = rendering
        ? rendering->get_video_adapter_name() : String();
    report["renderer_api_version"] = rendering
        ? rendering->get_video_adapter_api_version() : String();
    report["renderer_vendor"] = rendering
        ? rendering->get_video_adapter_vendor() : String();
    report["native_handles_borrowed"] = false;
    report["device_shared_with_renderer"] = false;
    report["gpu_resident_publication"] = false;
    report["native_queue_submission_permitted"] = false;
    report["synchronization_verified"] = false;
    report["diagnostic_gpu_active"] = is_native_diagnostic_active();
    report["diagnostic_backend"] = get_native_diagnostic_backend();
    report["status"] = present
        ? String("renderer_present_not_negotiated")
        : String("renderer_unavailable");
    report["reason"] = present
        ? String("Native renderer handles may be discoverable, but command "
                 "scheduling, buffer ownership and GPU synchronization "
                 "are not yet negotiated.")
        : String("The active Godot mode has no RenderingDevice; "
                 "native renderer interoperability cannot be attempted.");
    return report;
}

String LoxFXGroom3D::get_native_diagnostic_backend() const {
#if defined(LOXFX_GODOT_NATIVE_DIAGNOSTIC)
    if (native_)
        return String::utf8(native_->backend_name()) +
            (native_->software_device() ? " (software)" : " (hardware)");
#endif
    return "none";
}

} // namespace loxfx::godot_adapter
