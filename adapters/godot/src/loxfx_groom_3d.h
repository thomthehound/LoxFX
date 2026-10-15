// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "loxfx/prepared_groom.h"
#if defined(LOXFX_GODOT_NATIVE_DIAGNOSTIC)
#include "native_diagnostic.h"
#endif
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <optional>
#include <cstdint>
#include <memory>

namespace loxfx::godot_adapter {

// Minimal diagnostic consumer. The LoxFX SDK remains unaware of Godot.
// This node does not simulate, own a graphics device or claim performance
// equivalence with the eventual GPU-resident renderer.
class LoxFXGroom3D final : public ::godot::MeshInstance3D {
    GDCLASS(LoxFXGroom3D, ::godot::MeshInstance3D)

    std::optional<PreparedGroom> groom_;
    ::godot::Ref<::godot::ArrayMesh> lines_;
    ::godot::String error_;
#if defined(LOXFX_GODOT_NATIVE_DIAGNOSTIC)
    std::unique_ptr<NativeDiagnostic> native_;
    std::uint64_t native_frames_ = 0;
    ::godot::Vector3 native_last_root_{};
    bool native_root_valid_ = false;
#endif

    bool update_lines(const ::godot::PackedVector3Array& packed_positions,
                      bool first_upload);
    void fail(const ::godot::String& message);

protected:
    static void _bind_methods();

public:
    ~LoxFXGroom3D() override;
    // Standalone LoxFX native compute mode for visual diagnostics only.
    // The owning game or Godauldron drives step/reset on the scene thread.
    // Shader file: initial .spv (Linux Vulkan) or .dxil (Windows D3D12),
    // with all six compiled stages in the same directory.
    bool start_native_diagnostic(const ::godot::String& shader_file,
                                 bool software_device = false);
    bool step_native_diagnostic(double delta_seconds, double gravity = 9.81,
                                double damping = 0.08,
                                const ::godot::Vector3& root_translation = {},
                                const ::godot::Vector3& wind = {});
    bool reset_native_diagnostic();
    void stop_native_diagnostic();
    bool is_native_diagnostic_active() const;
    int get_native_diagnostic_steps() const;
    ::godot::Vector3 get_native_diagnostic_root_position() const;
    ::godot::String get_native_diagnostic_backend() const;

    bool load_tfx_bytes(const ::godot::PackedByteArray& tfx,
                        int followers_per_guide = 0,
                        double follower_root_radius = 0.0,
                        double follower_tip_separation = 0.0,
                        int seed = 1);
    bool publish_positions(const ::godot::PackedVector3Array& packed_positions);
    ::godot::PackedVector3Array get_initial_positions() const;
    int get_packed_vertex_count() const;
    int get_authored_guide_count() const;
    int get_vertices_per_strand() const;
    int get_followers_per_guide() const;
    bool is_loaded() const;
    ::godot::String get_last_error() const;
};

} // namespace loxfx::godot_adapter
