<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# TressFX 4.1 reference and migration map

For candidate modern algorithms, costs and benchmarking priorities, see
[modernization research](MODERNIZATION_RESEARCH.md).

Reference source: https://github.com/thomthehound/TressFX (AMD TressFX 4.1,
January 2020). Original work remains under AMD's MIT license. LoxFX does
not currently incorporate AMD source files.

| Reference responsibility | Original source | LoxFX disposition |
| --- | --- | --- |
| Binary groom format | src/TressFX/TressFXFileFormat.h | Decode 4.x guide attributes independently, with format provenance |
| Asset preparation | src/TressFX/TressFXAsset.cpp | Audit derived tangents/rest lengths/follow strands; do not pad on import |
| Simulation ordering | src/TressFX/TressFXSimulation.cpp | Preserve whole ordered pipeline, not isolated shader entry points |
| Simulation constraints | src/Shaders/TressFXSimulation.hlsl | Baseline first; compare measured XPBD variants later |
| GPU memory and state | src/TressFX/TressFXHairObject.cpp | Audit lifetime, frame state, barriers and resource ownership |
| Dynamic SDF contact | src/TressFX/TressFXSDFCollision.cpp | Audit updates, sharing and per-groom state |
| Rendering / transparency | src/TressFX/TressFXPPLL.cpp, TressFXShortCut.cpp | Optional renderers with explicit resource budgets |
| DX12 and Vulkan execution | src/DX12, src/VK, src/Common | Use as reference; no Cauldron runtime requirement |

## Verified legacy concerns requiring reproduction

1. TressFX 4.1 `LoadHairData` pads guide counts to simulation workgroup
   sizes and synthesizes strands. LoxFX import retains only the authored
   guide count; GPU packing and procedural follow strands belong to separate
   preparation stages.
2. The original simulation host submits ordered
   `IntegrationAndGlobalShapeConstraints`,
   `CalculateStrandLevelData`, `VelocityShockPropagation`,
   `LocalShapeConstraints`, `LengthConstriantsWindAndCollision`
   and `UpdateFollowHairVertices` operations, with resource barriers.
   Port their interdependencies and source ownership along with the shaders.
3. Upstream issue #46 reports a shared SDF collision constant-buffer
   problem when multiple hair objects have different vertex counts.
   Upstream issue #47 reports wind calculation problems and an overly
   coupled wind/constraint pass. Both need independent reproduction.
4. ShortCut and PPLL are different quality/performance tradeoffs;
   neither automatically dictates the LoxFX rendering API.

## Port decision record

For each substantial subsystem migration, record: original function and
shader dispatch, original inputs/outputs, buffer ownership and lifetimes,
ordering/synchronization, reference complexity and performance strategy,
modernized change and justification, correctness comparison, target hardware,
measurement and license/provenance.

A standalone CPU importer and tests are the initial LoxFX code milestone.
They do not establish functioning GPU simulation or claim equivalence with
the original reference renderer.
