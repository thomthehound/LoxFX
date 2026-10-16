// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0
// Reports whether a predecessor's terminator can be scheduled after the Phi
// to which its incoming block contributes. Deliberately makes no assumption
// about whether the predecessor is a loop back-edge.
#include "re-spirv.h"
#include "spirv/unified1/spirv.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: respv_phi_graph_probe input.spv\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        return 2;
    }
    const std::vector<char> data(std::istreambuf_iterator<char>{input}, {});
    if (data.empty() || (data.size() & 3)) {
        return 2;
    }
    respv::Shader shader;
    if (!shader.parse(data.data(), data.size(), false)) {
        return 1;
    }
    const auto *words = reinterpret_cast<const uint32_t *>(data.data());
    std::vector<uint32_t> rank(shader.instructions.size(), UINT32_MAX);
    for (uint32_t order = 0; order < shader.instructionOrder.size(); ++order) {
        rank[shader.instructionOrder[order]] = order;
    }
    uint32_t late = 0;
    uint32_t pairs = 0;
    for (const auto &phi : shader.phis) {
        const uint32_t instr = phi.instructionIndex;
        const auto &instruction = shader.instructions[instr];
        const uint32_t wi = instruction.wordIndex;
        const uint32_t wordCount = words[wi] >> 16U;
        const uint32_t headerInstruction = shader.blocks[instruction.blockIndex].labelInstructionIndex;
        const uint32_t targetLabel = words[shader.instructions[headerInstruction].wordIndex + 1];
        for (uint32_t k = 3; k + 1 < wordCount; k += 2) {
            uint32_t predecessorLabel = words[wi + k + 1];
            uint32_t predecessorInstr = shader.results[predecessorLabel].instructionIndex;
            uint32_t predecessorTerminator =
                shader.blocks[shader.instructions[predecessorInstr].blockIndex].terminatorInstructionIndex;
            ++pairs;
            if (rank[predecessorTerminator] > rank[instr]) {
                ++late;
                std::cout << "LATE_PHI_PREDECESSOR"
                          << " phi_id=" << words[wi + 2]
                          << " incoming_label=" << predecessorLabel
                          << " target_label=" << targetLabel
                          << " phi_order=" << rank[instr]
                          << " terminator_order=" << rank[predecessorTerminator]
                          << " predecessor_terminator_opcode="
                          << (words[shader.instructions[predecessorTerminator].wordIndex] & 0xFFFFU)
                          << "\n";
            }
        }
    }
    std::cout << "PHI_ORDER_SUMMARY pairs=" << pairs << " late=" << late << "\n";
}
