// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#include "re-spirv.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 4 || (std::string(argv[3]) != "default" &&
                       std::string(argv[3]) != "no-dce")) {
        std::cerr << "usage: respv_probe <input.spv> <output.spv> default|no-dce\n";
        return 2;
    }

    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "Cannot open input: " << argv[1] << '\n';
        return 2;
    }
    const std::vector<uint8_t> bytes{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (bytes.empty() || bytes.size() % sizeof(uint32_t) != 0) {
        std::cerr << "Input is not a SPIR-V word stream\n";
        return 2;
    }

    // Match Godot's no-specialization path, including disabled inlining.
    respv::Shader shader(bytes.data(), bytes.size(), false);
    if (shader.empty()) {
        std::cerr << "re-SPIRV could not parse input\n";
        return 1;
    }
    respv::Options options;
    options.removeDeadCode = std::string(argv[3]) == "default";
    std::vector<uint8_t> output;
    if (!respv::Optimizer::run(shader, nullptr, 0, output, options)) {
        std::cerr << "re-SPIRV optimization failed\n";
        return 1;
    }
    std::ofstream file(argv[2], std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char *>(output.data()),
                             static_cast<std::streamsize>(output.size()))) {
        std::cerr << "Cannot write output: " << argv[2] << '\n';
        return 2;
    }
    std::cout << "re-SPIRV input=" << bytes.size() << " bytes output="
              << output.size() << " bytes mode=" << argv[3] << '\n';
    return 0;
}
