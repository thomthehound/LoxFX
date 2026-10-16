// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
// Print the parsed shader's conservative final-Phi-pass scheduling requirement.
#include "re-spirv.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>{input}, {}};
    if (!input || bytes.empty() || bytes.size() % 4 != 0) return 2;
    respv::Shader shader;
    if (!shader.parse(bytes.data(), bytes.size(), false)) return 3;
    std::cout << (shader.requiresFinalPhiCleanup ? "1" : "0") << '\n';
}
