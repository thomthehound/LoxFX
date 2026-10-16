// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
// A diagnostic-only batch benchmark for re-SPIRV transformations.
#include "re-spirv.h"
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: respv_bench shader.spv iterations [parse-run]\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> bytes(std::istreambuf_iterator<char>{input}, {});
    if (bytes.empty() || bytes.size() % 4 != 0) {
        return 2;
    }
    const uint32_t iterations = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
    const bool parseEveryTime = argc == 4 && std::string(argv[3]) == "parse-run";
    respv::Shader shader;
    if (!shader.parse(bytes.data(), bytes.size(), false)) {
        return 1;
    }
    std::vector<uint8_t> output;
    for (int i = 0; i < 30; ++i) {
        if (parseEveryTime && !shader.parse(bytes.data(), bytes.size(), false)) return 1;
        if (!respv::Optimizer::run(shader, nullptr, 0, output)) {
            return 1;
        }
    }
    using Clock = std::chrono::steady_clock;
    std::vector<double> samples;
    for (int batch = 0; batch < 7; ++batch) {
        auto before = Clock::now();
        for (uint32_t i = 0; i < iterations; ++i) {
            if (parseEveryTime && !shader.parse(bytes.data(), bytes.size(), false)) return 1;
            if (!respv::Optimizer::run(shader, nullptr, 0, output)) {
                return 1;
            }
        }
        auto after = Clock::now();
        samples.push_back(std::chrono::duration<double, std::micro>(after-before).count()/iterations);
    }
    std::sort(samples.begin(), samples.end());
    std::cout << samples[samples.size()/2] << '\n';
    return 0;
}
