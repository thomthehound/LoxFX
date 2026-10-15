// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#include "loxfx/groom.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8u * i));
}

void put_f32(std::vector<std::uint8_t>& bytes, std::size_t offset, float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    put_u32(bytes, offset, bits);
}

// Two guide strands, four vertices each, with all optional 4.x attributes.
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> bytes(504, 0);
    put_f32(bytes, 0, 4.0f);
    put_u32(bytes, 4, 2);
    put_u32(bytes, 8, 4);
    put_u32(bytes, 12, 160); // position float4[8]
    put_u32(bytes, 16, 288); // strand uv float2[2]
    put_u32(bytes, 20, 304); // vertex uv float2[8]
    put_u32(bytes, 24, 368); // thickness float[2]
    put_u32(bytes, 28, 376); // color float4[8]
    for (unsigned i = 0; i < 8; ++i) {
        put_f32(bytes, 160 + 16 * i, static_cast<float>(i));
        put_f32(bytes, 160 + 16 * i + 4, 0.25f);
        put_f32(bytes, 160 + 16 * i + 8, -1.0f);
        put_f32(bytes, 160 + 16 * i + 12, 1.0f);
        put_f32(bytes, 304 + 8 * i, static_cast<float>(i) / 8.0f);
        put_f32(bytes, 304 + 8 * i + 4, 0.5f);
        put_f32(bytes, 376 + 16 * i, 0.8f);
        put_f32(bytes, 376 + 16 * i + 4, 0.7f);
        put_f32(bytes, 376 + 16 * i + 8, 0.6f);
        put_f32(bytes, 376 + 16 * i + 12, 1.0f);
    }
    put_f32(bytes, 288, 0.125f);
    put_f32(bytes, 292, 0.25f);
    put_f32(bytes, 296, 0.5f);
    put_f32(bytes, 300, 0.75f);
    put_f32(bytes, 368, 0.1f);
    put_f32(bytes, 372, 0.2f);
    return bytes;
}

void test_complete() {
    const auto bytes = fixture();
    auto result = loxfx::import_tfx(bytes);
    require(static_cast<bool>(result), "complete fixture must import");
    require(result->guide_strands == 2, "authored strands must not be padded");
    require(result->vertices_per_strand == 4, "vertices per strand");
    require(result->positions.size() == 8, "position count");
    require(result->positions[7].x == 7.0f, "position value");
    require(result->positions[0].w == 1.0f, "fourth component preserved");
    require(result->strand_uvs.size() == 2, "strand uv count");
    require(result->vertex_uvs.size() == 8, "vertex uv count");
    require(result->strand_thickness.size() == 2, "thickness count");
    require(result->vertex_colors.size() == 8, "color count");
    require(result->strand_uvs[1].y == 0.75f, "strand uv value");
    require(result->strand_thickness[1] == 0.2f, "thickness value");
}

void test_optional_absent() {
    auto bytes = fixture();
    for (std::size_t offset : {16u, 20u, 24u, 28u}) put_u32(bytes, offset, 0);
    auto result = loxfx::import_tfx(bytes);
    require(static_cast<bool>(result), "optional attributes may be absent");
    require(result->positions.size() == 8, "positions remain");
    require(result->strand_uvs.empty() && result->vertex_uvs.empty() &&
            result->strand_thickness.empty() && result->vertex_colors.empty(),
            "missing attributes stay missing");
}

void test_rejection() {
    auto bytes = fixture();
    require(!loxfx::import_tfx({}), "null data");
    require(!loxfx::import_tfx(std::span<const std::uint8_t>{bytes.data(), 159}), "truncated header");
    bytes.pop_back();
    require(!loxfx::import_tfx(bytes), "truncated color section");
    bytes = fixture();
    put_f32(bytes, 0, 3.0f);
    require(!loxfx::import_tfx(bytes), "unsupported major version");
    bytes = fixture();
    put_u32(bytes, 4, 0);
    require(!loxfx::import_tfx(bytes), "zero strands");
    bytes = fixture();
    put_u32(bytes, 8, 3);
    require(!loxfx::import_tfx(bytes), "unsupported strand topology");
    bytes = fixture();
    put_u32(bytes, 12, 0);
    require(!loxfx::import_tfx(bytes), "missing position array");
    bytes = fixture();
    put_u32(bytes, 16, 164);
    require(!loxfx::import_tfx(bytes), "overlapping attribute ranges");
    bytes = fixture();
    put_f32(bytes, 160, std::numeric_limits<float>::quiet_NaN());
    require(!loxfx::import_tfx(bytes), "nonfinite guide vertex");
    bytes = fixture();
    put_u32(bytes, 4, std::numeric_limits<std::uint32_t>::max());
    require(!loxfx::import_tfx(bytes), "malicious strand count");
}

} // namespace

int main() {
    try {
        test_complete();
        test_optional_absent();
        test_rejection();
        std::cout << "TFX importer tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
