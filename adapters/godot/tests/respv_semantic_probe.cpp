// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
// Optimization CLI with optional function inlining and several spec constants.
#include "re-spirv.h"
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
int main(int argc, char **argv) {
    if (argc < 4 || (argc-4)%2) {
        std::cerr << "usage: semantic_probe input.spv output.spv inline:0|1 [id uint32]...\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>{input}, {}};
    if(!input || bytes.empty() || bytes.size()%4) return 2;
    respv::Shader shader;
    if(!shader.parse(bytes.data(), bytes.size(), std::stoi(argv[3])!=0)) {
        std::cerr << "parse failed\n";return 3;
    }
    std::vector<respv::SpecConstant> constants;
    for(int i=4;i<argc;i+=2) {
        constants.emplace_back(uint32_t(std::stoul(argv[i])),
                               std::vector<uint32_t>{uint32_t(std::stoul(argv[i+1]))});
    }
    std::vector<uint8_t> out;
    if(!respv::Optimizer::run(shader,constants.data(),uint32_t(constants.size()),out)) {
        std::cerr << "optimization failed\n";return 4;
    }
    std::ofstream output(argv[2],std::ios::binary);
    output.write(reinterpret_cast<const char*>(out.data()),std::streamsize(out.size()));
    return output?0:5;
}
