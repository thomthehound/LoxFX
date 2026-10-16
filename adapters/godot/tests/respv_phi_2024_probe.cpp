// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
// Standalone adapter for the 2024 re-SPIRV parse/optimize API.
#include <cstdint>
#include "re-spirv.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: old_probe input.spv output.spv\n";
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<char> input(std::istreambuf_iterator<char>{file}, {});
    if (input.empty() || input.size() % 4) {
        return 2;
    }
    respv::Shader shader;
    if (!shader.parse(input.data(), input.size())) {
        std::cerr << "PARSE_FAILED\n";
        return 3;
    }
    std::vector<uint8_t> out;
    if (!respv::Optimizer::run(shader, nullptr, 0, out)) {
        std::cerr << "OPTIMIZE_FAILED\n";
        return 4;
    }
    std::ofstream target(argv[2], std::ios::binary);
    target.write(reinterpret_cast<const char *>(out.data()),
                 static_cast<std::streamsize>(out.size()));
    return target ? 0 : 5;
}
