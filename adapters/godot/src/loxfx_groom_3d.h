// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "loxfx/prepared_groom.h"
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <optional>

namespace loxfx::godot_adapter {

// Minimal diagnostic consumer. The LoxFX SDK remains unaware of Godot.
// This node does not simulate, own a graphics device or claim performance
// equivalence with the eventual GPU-resident renderer.
class LoxFXGroom3D final : public ::godot::MeshInstance3D {
    GDCLASS(LoxFXGroom3D, ::godot::MeshInstance3D)

    std::optional<PreparedGroom> groom_;
    ::godot::Ref<::godot::ArrayMesh> lines_;
    ::godot::String error_;
    std::uint64_t displayed_guide_count_ = 0;

    bool update_lines(const ::godot::PackedVector3Array& packed_positions,
                      bool first_upload);
    void fail(const ::godot::String& message);

protected:
    static void _bind_methods();

public:
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
