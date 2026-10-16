// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
// Repeatable re-SPIRV benchmark with specialization and optional function inlining.
#include "re-spirv.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 8 && argc != 9) {
        std::cerr << "usage: respv_switch_bench input.spv iterations inline:0|1 A B MODE COUNT [parse-run]\n";
        return 2;
    }
    const uint32_t iterations = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
    if (iterations == 0) return 2;
    const bool inlineFunctions = std::strtoul(argv[3], nullptr, 10) != 0;
    const bool parseEveryTime = argc == 9 && std::string(argv[8]) == "parse-run";
    if (argc == 9 && !parseEveryTime) return 2;

    std::ifstream source(argv[1], std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>{source}, {}};
    if (!source || bytes.empty() || bytes.size() % sizeof(uint32_t) != 0) return 2;

    std::vector<respv::SpecConstant> constants;
    for (uint32_t id = 0; id < 4; ++id) {
        constants.emplace_back(id, std::vector<uint32_t>{
            static_cast<uint32_t>(std::strtoul(argv[id + 4], nullptr, 10))
        });
    }

    respv::Shader shader;
    if (!shader.parse(bytes.data(), bytes.size(), inlineFunctions)) return 3;
    std::vector<uint8_t> output;
    auto once = [&]() {
        if (parseEveryTime && !shader.parse(bytes.data(), bytes.size(), inlineFunctions)) return false;
        return respv::Optimizer::run(shader, constants.data(), uint32_t(constants.size()), output);
    };

    for (int i = 0; i < 32; ++i) if (!once()) return 4;

    using Clock = std::chrono::steady_clock;
    std::vector<double> samples;
    for (int batch = 0; batch < 7; ++batch) {
        const auto start = Clock::now();
        for (uint32_t i = 0; i < iterations; ++i) {
            if (!once()) return 4;
        }
        const auto elapsed = Clock::now() - start;
        samples.push_back(
            std::chrono::duration<double, std::micro>(elapsed).count() / iterations
        );
    }
    std::sort(samples.begin(), samples.end());
    std::cout << std::fixed << std::setprecision(4) << samples[samples.size() / 2] << '\n';
    return 0;
}
