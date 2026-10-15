// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace loxfx {

struct Float2 {
    float x;
    float y;
};

struct Float4 {
    float x;
    float y;
    float z;
    float w;
};

// Authored guide-strand data, without GPU workgroup padding or generated follow hairs.
// All vertex attributes are strand-major; optional arrays are empty when absent.
struct GroomAsset {
    std::uint32_t guide_strands = 0;
    std::uint32_t vertices_per_strand = 0;
    std::vector<Float4> positions;
    std::vector<Float2> strand_uvs;
    std::vector<Float2> vertex_uvs;
    std::vector<float> strand_thickness;
    std::vector<Float4> vertex_colors;
};

using ImportResult = std::expected<GroomAsset, std::string>;

// Import TressFX 4.x .tfx guide-strand data from a memory buffer.
// No dependency on TressFX, Cauldron, a graphics API or a physics engine.
ImportResult import_tfx(std::span<const std::uint8_t> bytes);

} // namespace loxfx
