// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#include "loxfx/groom.h"

#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace loxfx {
namespace {

constexpr std::size_t header_bytes = 160;

std::uint32_t read_u32(const std::uint8_t* bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}

float read_f32(const std::uint8_t* bytes, std::size_t offset) {
    const std::uint32_t bits = read_u32(bytes, offset);
    return std::bit_cast<float>(bits);
}

Float2 read_float2(const std::uint8_t* bytes, std::size_t offset) {
    return {read_f32(bytes, offset), read_f32(bytes, offset + 4)};
}

Float4 read_float4(const std::uint8_t* bytes, std::size_t offset) {
    return {read_f32(bytes, offset), read_f32(bytes, offset + 4),
            read_f32(bytes, offset + 8), read_f32(bytes, offset + 12)};
}

bool finite(const Float4& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

struct Section {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool present = false;
};

ImportResult fail(const char* message) {
    return std::unexpected(std::string{message});
}

} // namespace

ImportResult import_tfx(std::span<const std::uint8_t> input) {
    const std::uint8_t* bytes = input.data();
    const std::size_t byte_count = input.size();
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                  "TFX import requires IEEE-754 32-bit float");
    if (byte_count < header_bytes) {
        return fail("missing or truncated TFX header");
    }

    const float version = read_f32(bytes, 0);
    if (!(version >= 4.0f && version < 5.0f)) {
        return fail("unsupported TFX file version (expected 4.x)");
    }
    const std::uint32_t strands = read_u32(bytes, 4);
    const std::uint32_t vertices = read_u32(bytes, 8);
    if (strands == 0 || vertices < 4 || vertices > 64 ||
        (vertices & (vertices - 1u)) != 0) {
        return fail("invalid guide count or vertices-per-strand");
    }

    if (static_cast<std::size_t>(strands) >
        std::numeric_limits<std::size_t>::max() / vertices) {
        return fail("TFX guide count exceeds addressable memory");
    }
    const std::size_t vertex_count =
        static_cast<std::size_t>(strands) * static_cast<std::size_t>(vertices);
    if (vertex_count > std::numeric_limits<std::size_t>::max() / 16u) {
        return fail("TFX vertex count exceeds addressable memory");
    }

    // TressFXTFXFileHeader: five 32-bit offsets at bytes 12 through 28.
    // Optional attributes remain absent rather than being synthesized.
    const std::array<std::uint32_t, 5> offsets = {
        read_u32(bytes, 12), read_u32(bytes, 16), read_u32(bytes, 20),
        read_u32(bytes, 24), read_u32(bytes, 28)
    };
    const std::array<std::size_t, 5> counts = {
        vertex_count, strands, vertex_count, strands, vertex_count
    };
    const std::array<std::size_t, 5> strides = {16u, 8u, 8u, 4u, 16u};
    std::array<Section, 5> sections{};

    for (std::size_t i = 0; i < sections.size(); ++i) {
        const std::size_t offset = offsets[i];
        if (offset == 0) {
            if (i == 0) return fail("TFX guide positions are missing");
            continue;
        }
        if (counts[i] > std::numeric_limits<std::size_t>::max() / strides[i]) {
            return fail("TFX attribute length overflow");
        }
        const std::size_t length = counts[i] * strides[i];
        if ((offset & 7u) != 0 || offset < header_bytes || offset > byte_count ||
            length > byte_count - offset) {
            return fail("TFX attribute extends outside input buffer");
        }
        sections[i] = {offset, offset + length, true};
        for (std::size_t j = 0; j < i; ++j) {
            if (sections[j].present &&
                sections[i].begin < sections[j].end &&
                sections[j].begin < sections[i].end) {
                return fail("TFX attribute ranges overlap");
            }
        }
    }

    GroomAsset asset;
    asset.guide_strands = strands;
    asset.vertices_per_strand = vertices;
    asset.positions.reserve(vertex_count);
    for (std::size_t i = 0; i < vertex_count; ++i) {
        const Float4 value = read_float4(bytes, sections[0].begin + 16u * i);
        if (!finite(value)) return fail("TFX positions contain non-finite values");
        asset.positions.push_back(value);
    }

    if (sections[1].present) {
        asset.strand_uvs.reserve(strands);
        for (std::size_t i = 0; i < strands; ++i) {
            const Float2 value = read_float2(bytes, sections[1].begin + 8u * i);
            if (!std::isfinite(value.x) || !std::isfinite(value.y))
                return fail("TFX strand UVs contain non-finite values");
            asset.strand_uvs.push_back(value);
        }
    }
    if (sections[2].present) {
        asset.vertex_uvs.reserve(vertex_count);
        for (std::size_t i = 0; i < vertex_count; ++i) {
            const Float2 value = read_float2(bytes, sections[2].begin + 8u * i);
            if (!std::isfinite(value.x) || !std::isfinite(value.y))
                return fail("TFX vertex UVs contain non-finite values");
            asset.vertex_uvs.push_back(value);
        }
    }
    if (sections[3].present) {
        asset.strand_thickness.reserve(strands);
        for (std::size_t i = 0; i < strands; ++i) {
            const float value = read_f32(bytes, sections[3].begin + 4u * i);
            if (!std::isfinite(value))
                return fail("TFX strand thickness contains non-finite values");
            asset.strand_thickness.push_back(value);
        }
    }
    if (sections[4].present) {
        asset.vertex_colors.reserve(vertex_count);
        for (std::size_t i = 0; i < vertex_count; ++i) {
            const Float4 value = read_float4(bytes, sections[4].begin + 16u * i);
            if (!finite(value))
                return fail("TFX vertex colors contain non-finite values");
            asset.vertex_colors.push_back(value);
        }
    }
    return asset;
}

} // namespace loxfx
