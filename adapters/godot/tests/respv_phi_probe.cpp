// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
// Investigation-only CLI: apply a single boolean specialization ID.
#include "re-spirv.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 3 && argc != 5) {
        std::cerr << "usage: respv_phi_probe input.spv output.spv [spec-id 0|1]\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "Unable to open input\n";
        return 2;
    }
    std::vector<char> bytes(std::istreambuf_iterator<char>{input}, {});
    if (bytes.empty() || bytes.size() % 4 != 0) {
        std::cerr << "Invalid input size\n";
        return 2;
    }
    respv::Shader shader;
    if (!shader.parse(bytes.data(), bytes.size(), false)) {
        std::cerr << "Unable to parse input\n";
        return 1;
    }
    std::vector<respv::SpecConstant> constants;
    if (argc == 5) {
        constants.emplace_back(static_cast<uint32_t>(std::stoul(argv[3])),
                               std::vector<uint32_t>{static_cast<uint32_t>(std::stoul(argv[4]))});
    }
    std::vector<uint8_t> output;
    if (!respv::Optimizer::run(shader, constants.data(),
                                static_cast<uint32_t>(constants.size()), output)) {
        std::cerr << "Optimization failed\n";
        return 1;
    }
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char *>(output.data()),
              static_cast<std::streamsize>(output.size()));
    return out ? 0 : 2;
}
