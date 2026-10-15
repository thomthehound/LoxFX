<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Velocity-shock propagation — stage three

## Source and execution contract

This stage follows AMD TressFX 4.1's `VelocityShockPropagation` in
`src/Shaders/TressFXSimulation.hlsl`, consuming the per-guide motion records
from `CalculateStrandLevelData`. It runs after integration and strand-level
motion, before local-shape constraints.

One invocation handles a **guide vertex**, not a follower vertex. For each
guide vertex from index 2 onward, the solver reads its source-derived
rotation quaternion, translation, and propagation coefficient:

```text
new = (1 - coefficient) * old + coefficient * (rotate(quaternion, old) + translation)
```

The transformation is applied to **both current and previous positions**.
Vertices 0 and 1 are left unchanged, preserving proximal attachment motion.
Generated follower positions are not simulated by this stage.

## Implementation

- One canonical `shaders/VelocityShockPropagation.hlsl` compiled to
  SPIR-V and SM6.6 DXIL.
- Native Vulkan and D3D12 execution through the existing shared device
  contexts and per-groom persistent buffers.
- The strand-level pipeline bindings are reused; there are no additional
  GPU allocations or descriptor sets.
- The caller records integration → strand-level motion → VSP into a
  command buffer/list. GPU memory dependencies precede the consuming stage.
  Recording does not submit, wait, or read back.
- Workgroup sizes use existing source-derived guide-vertex dispatch
  quantities, including padded-guide bounds.

## Numerical regression coverage

Both backends exercise the same fixtures:

1. Root translation with acceleration-triggered full propagation.
2. Partial propagation below the acceleration threshold.
3. Unchanged proximal vertices, pin flags, and generated follower storage.
4. A subsequent integration pass with no unintended velocity spike,
   verifying that both current and previous positions were propagated.
5. A nonidentity proximal rotation, compared with the expected
   quaternion-plus-translation blend for distal guide vertices.

Software Vulkan and D3D12 WARP are functional tests. Hardware throughput
and complete visible quality are evaluated with a later coherent
simulation-and-rendering baseline.

## Next steps

Stage four implements local-shape constraints, accounting for reference
iteration ordering and stable curvature. Per-guide skeletal skinning
inputs and nonidentity skinning quaternions remain necessary for animated
characters. Stages five and six add length/wind/collision and follower
publication, enabling a meaningful visual and full-frame GPU evaluation.

See [the roadmap](ROADMAP.md), [simulation execution trace](SIMULATION_EXECUTION_TRACE.md)
and [optimization backlog](OPTIMIZATION_BACKLOG.md).
