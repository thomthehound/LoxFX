<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Strand-level motion stage: GPU execution and scope (PR #16)

## Source-driven contract

TressFX 4.1 `src/Shaders/TressFXSimulation.hlsl`,
`CalculateStrandLevelData` (lines 678–728) reads current, previous, and
previous-previous positions of vertices **0 and 1** on each guide, calculates
a quaternion from the prior first segment to the new segment, and derives
the corresponding translation from the first root. It promotes the
VSP propagation coefficient to 1 when the second finite difference of
proximal vertex 1 exceeds a configured acceleration threshold. It writes
three `float4` values per guide: `skinningQuat`, `vspQuat`,
`vspTranslation` (xyz movement, w coefficient).

## Implemented in this PR

- Shared `CalculateStrandLevelData.hlsl` compiles to both Vulkan SPIR-V
  and D3D12 SM6.6 DXIL, and is **executed** on Mesa and WARP respectively.
- One invocation per padded guide; each outputs a 48-byte
  `StrandMotionData` record. No strand-level output per follower.
- Current/previous guide positions and *two proximal previous-previous
  positions* are read on GPU. The previous-previous buffer is the compact
  state added in PR #15. No extra full-size history copy is introduced.
- The host records integration and then stage two into the same command
  buffer/list with an explicit GPU memory dependency. No submission,
  blocking fence, CPU history upload or readback in stage recording.
- In D3D12, the second stage reads compatible UAV buffers to avoid
  pointless UAV → SRV → UAV resource transitions. A root descriptor
  and 32-byte constants bind the stage; no descriptor heap is required.
  Vulkan uses a second descriptor set but shares the per-context layout
  and native shader pipeline ownership.
- Quaternion computation guards degenerate and anti-parallel root
  segments so an imported strand cannot trivially introduce NaNs.

**Important limit:** the reference `skinningQuat` comes from per-strand
bone skinning. Our current attachment is translation only, and its
skin quaternion is therefore **identity**. This is explicit, not a
claim that a full skeletal hair simulation has been ported. Rest length
and animated skeletal matrices are inputs to later stages, not silently
simulated on the CPU.

## Verification and product acceptance

The two GPU CI fixtures compare the same translated-root reference
values, expected quaternion identity, propagation coefficients above and
below an acceleration threshold, and size of the padded per-guide
output. These are correctness tests, **not** reference-quality visuals
or GPU performance benchmarks.

Next: stage-three `VelocityShockPropagation` reads these data and updates
guide vertices 2+, including both current and previous states, then
local shape, length/wind/collision and follower publication. Restore
full bone-space attachments in a source-grounded slice before claiming
skeletal-reference equivalence. Keep OPT-001/002/003/009/011 open.

**Hard goals:** visual quality at least matching reference, preferably
substantially better; and lower total GPU frame time on physical GPUs
at matched-or-higher visual quality, not favorable isolated kernels.

## Near-antiparallel rotation regression (Codex P2, PR #16)

The first implementation snapped any proximal-segment rotation with
`dot(a,b) < -0.9999` (roughly 179.2–180 degrees) to an *exact*
180-degree quaternion. That discarded the actual motion axis/angle.
The shared HLSL now computes the near-opposite half-angle from
`|a+b|/2` and the axis from `cross(a,b)`, retaining rotations
near 180 even when the scalar dot product rounds to -1. An arbitrary
orthogonal axis is used only when the vectors are numerically
indistinguishable from exact opposites (vanishing cross product).

Matched Vulkan and D3D12 GPU regressions test turns on both sides of
the former threshold, positive/negative axis directions, an almost-exact
reversal, and an exact 180-degree case. They verify that applying the
stored quaternion maps the *previous* guide-root segment to the actual
*new* segment, not just that a quaternion value looks plausible.
