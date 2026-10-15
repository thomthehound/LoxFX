// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#include <loxfx/groom.h>

#include <array>
#include <cstdint>
#include <span>

int main() {
    constexpr std::array<std::uint8_t, 0> empty{};
    const auto result = loxfx::import_tfx(std::span{empty});
    const loxfx::GroomAsset asset{};
    return !result && asset.guide_strands == 0 ? 0 : 1;
}
