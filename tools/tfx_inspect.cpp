// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#include "loxfx/groom.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: loxfx_tfx_inspect <asset.tfx>\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "Cannot open TFX file\n";
        return 1;
    }
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()
    };
    if (!input.eof() && input.fail()) {
        std::cerr << "Cannot read TFX file\n";
        return 1;
    }
    const loxfx::ImportResult result = loxfx::import_tfx(bytes);
    if (!result) {
        std::cerr << "Invalid TFX file: " << result.error() << "\n";
        return 1;
    }
    const auto& groom = *result;
    std::cout << "Guide strands: " << groom.guide_strands << "\n"
              << "Vertices per strand: " << groom.vertices_per_strand << "\n"
              << "Authored vertices: " << groom.positions.size() << "\n"
              << "Strand UVs: " << groom.strand_uvs.size() << "\n"
              << "Vertex UVs: " << groom.vertex_uvs.size() << "\n"
              << "Strand thickness values: " << groom.strand_thickness.size() << "\n"
              << "Vertex colors: " << groom.vertex_colors.size() << "\n";
    return 0;
}
