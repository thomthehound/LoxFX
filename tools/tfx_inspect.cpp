// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#include "loxfx/groom.h"
#include "loxfx/simulation_work.h"
#include "loxfx/prepared_groom.h"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <string_view>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: loxfx_tfx_inspect <asset.tfx> [followers-per-guide] [--prepare]\n";
        return 2;
    }
    bool prepare = false;
    bool has_follower_count = false;
    std::uint32_t followers = 0;
    for (int arg = 2; arg < argc; ++arg) {
        const std::string_view input{argv[arg]};
        if (input == "--prepare") {
            if (prepare) {
                std::cerr << "Duplicate --prepare option\n";
                return 2;
            }
            prepare = true;
            continue;
        }
        if (has_follower_count) {
            std::cerr << "Only one follower count may be supplied\n";
            return 2;
        }
        const auto [end, ec] = std::from_chars(
            input.data(), input.data() + input.size(), followers);
        if (ec != std::errc{} || end != input.data() + input.size()) {
            std::cerr << "Follower count must be a nonnegative integer\n";
            return 2;
        }
        has_follower_count = true;
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
    const auto work = loxfx::make_simulation_work_plan(
        {groom.guide_strands, groom.vertices_per_strand, followers, 2});
    if (!work) {
        std::cerr << "Cannot prepare simulation work: " << work.error() << "\n";
        return 1;
    }
    std::cout << "Prepared guides: " << work->packed_guide_strands << "\n"
              << "Prepared total strands: " << work->packed_total_strands << "\n"
              << "Planned thread lanes: " << work->planned_lane_invocations << "\n"
              << "Reference-launched thread lanes: " << work->reference_lane_invocations << "\n";
    if (prepare) {
        const auto prepared = loxfx::prepare_groom(groom, {
            .followers_per_guide = followers,
            .local_shape_iterations = 2,
            .follower_root_radius = 0.005f,
            .follower_tip_separation = 0.25f
        });
        if (!prepared) {
            std::cerr << "Cannot prepare groom: " << prepared.error() << "\n";
            return 1;
        }
        std::cout << "Prepared position vertices: " << prepared->rest_positions.size() << "\n"
                  << "Prepared rest lengths: " << prepared->rest_segment_lengths.size() << "\n"
                  << "Static GPU upload bytes: " << prepared->static_buffer_bytes() << "\n";
    }
    return 0;
}
