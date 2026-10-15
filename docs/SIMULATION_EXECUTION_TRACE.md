<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# TressFX 4.1 guide-strand simulation execution and performance trace

Status: source audit and executable work-count contract, 15 October 2026.
The first four stages now execute in native Vulkan and D3D12 integration
harnesses. Complete simulation, visible quality and physical-GPU profiling
remain subsequent milestones.

## Source references

- [Simulation host](https://github.com/thomthehound/TressFX/blob/master/src/TressFX/TressFXSimulation.cpp):
  `Initialize`, `DispatchComputeShader`, `Simulate`.
- [Simulation shaders](https://github.com/thomthehound/TressFX/blob/master/src/Shaders/TressFXSimulation.hlsl):
  `CalcIndicesInVertexLevelMaster`, `CalcIndicesInStrandLevelMaster`,
  and six stage entry points.
- [Asset preparation](https://github.com/thomthehound/TressFX/blob/master/src/TressFX/TressFXAsset.cpp):
  `LoadHairData`, `GenerateFollowHairs`.
- [Hair object state](https://github.com/thomthehound/TressFX/blob/master/src/TressFX/TressFXHairObject.cpp):
  `UpdateSimulationParameters`, `TressFXHairObject` initialization.
- [Constants and workgroup packing](https://github.com/thomthehound/TressFX/blob/master/src/TressFX/TressFXConstantBuffers.h):
  `SetVerticesPerStrand`, `SetFollowHairsPerGuidHair`.

## Actual GPU stage order

| Stage | Shader entry point | Indexing | Writes and dependencies |
|---|---|---|---|
| 1 | `IntegrationAndGlobalShapeConstraints` | 1 thread/guide vertex | Writes positions/history; consumes skinned rest positions |
| 2 | `CalculateStrandLevelData` | 1 thread/guide strand | Reads current/old/old-old root positions; writes skinning/VSP transforms |
| 3 | `VelocityShockPropagation` | 1 thread/guide vertex | Reads stage 2 transforms; writes current/old positions |
| 4 | `LocalShapeConstraints` | 1 thread/guide strand per iteration | Reads transforms, rest and current positions; writes current positions |
| 5 | `LengthConstriantsWindAndCollision` | 1 thread/guide vertex | Shared-memory length iterations and barriers, wind, tangents, position history |
| 6 | `UpdateFollowHairVertices` | 1 thread/guide vertex | Expands followers based on guide positions and offsets, publishes renderable positions |

The reference host schedules **stage-major across hair objects**, with per-object
resource binding and a UAV barrier at the end of each hair-object stage.
Before the first stage, it updates all objects' simulation constant buffers;
after the last stage, it advances their simulation frame. The local-shape
stage is repeated by host dispatch if the strands have fewer than 64 vertices.

A correct GPU executor must preserve *all* observable ordering, ownership,
barrier and publication dependencies. Do not mistake six independently
compilable kernels for a complete simulation pipeline.

## Verified over-dispatch in original host

`DispatchComputeShader` derives its `DISPATCHLEVEL_VERTEX` groups from
`GetNumTotalHairVertices()` and strand groups from
`GetNumTotalHairStrands()`. Those include procedurally generated follower
strands. But **all six** shader entry points use `CalcIndicesIn*Master`,
which multiplies the selected guide index by `followers_per_guide + 1`.
Consequently the shader threads for each stage represent *guides*, while
the host launches using guides **plus followers**.

For three followers/guide and 64 prepared guides of 16 vertices:
- LoxFX packed resource capacity: 256 strands × 16 = 4,096 vertices.
- Correct master-index vertex dispatch: 1,024 lanes = 16 groups.
- Correct master-index strand dispatch: 64 lanes = 1 group.
- **Legacy loader** pads 64 authored guides to **128**, even though the
  count was already aligned. With three followers this becomes 512 strands
  and 8,192 vertices.
- Legacy vertex dispatch: 8,192 lanes = 128 groups.
- Legacy strand dispatch: 512 lanes = 8 groups.
- Four per-vertex stages, two strand stages, and two local-shape iterations
  lead to 4,288 intended master-guide lanes vs. 34,304 reference-launched
  lanes. In this particular exactly aligned case, the difference is **8×**
  in logical lanes due to both independent inefficiencies.

The Linux reference-asset smoke test also confirmed the real, AMD-authored
Ratboy groom metadata: the mohawk contains 2,228 guides × 32 vertices and the
short groom contains 37,747 guides × 8 vertices. With requested follower
multiplicities of three and one respectively, the guide-only planned launch
counts are 293,440 and 1,321,600 lanes, compared with 1,173,760 and
2,643,200 in the original host's total-strand launch formula. The assets
are downloaded only by CI and are not distributed in the LoxFX source tree.

With non-aligned authored guide counts, the extra padding-block effect
disappears, giving **4×** reference lane counts with three followers/guide.
These are analytical launch counts, **not evidence** of a 4× or 8×
performance slowdown, or a proportional runtime speedup after correction. The extra groups may also index
past valid master-strand data: their shader-side index calculation treats
already-expanded counts as guide counts, and does not show a guard for the
end of actual guide storage. This needs a real reference run to establish
actual behavior, risk and performance impact.

The C++23 `make_simulation_work_plan` function validates index ranges,
retains 64-guide padding required by the unguarded source shader, and
calculates all six guide-only group counts once during preparation/LOD changes.
It never queries GPU features or performs per-dispatch count division.
It is a necessary execution contract, **not** a simulation, driver interface
or automatic speedup.

## 64-vertex local-shape iterations and a source discrepancy

The reference host selects one local-shape dispatch for strands with 64
vertices and uses repeated host dispatches for shorter strands. It writes
the requested iteration count to the constant buffer on the 64-vertex path,
but the released `LocalShapeConstraints` shader does **not** read this
iteration count or loop its body: the requested additional local iterations
are therefore not implemented in the original 64-vertex shader.

LoxFX implements the intended semantics: one dispatch with a shader-internal
iteration loop for 64-vertex strands, and repeated dispatches with explicit
GPU memory dependencies for shorter strands. Matching Vulkan and D3D12
functional fixtures test both iteration modes. Physical-GPU throughput
and visible-shape convergence remain to be measured.

## Outstanding synchronization concern

The source host issues multiple local-shape dispatches inside the same
per-object loop and only emits its explicit UAV barrier *after* all local
iterations. Since iterations read and write the same position buffer, the
port must audit native D3D12/Vulkan dispatch semantics and engine-interface
implicit barriers before reproducing this sequence. Do not silently assume
every dispatch is ordered at the shader-memory visibility level.
Avoid unnecessary barriers, but do not trade synchronization correctness
for a speculative gain.

## Performance gates for GPU implementation

1. Upload and prepare the groom once; retain GPU buffers across steps.
   Account for actual simulated guides separately from rendered followers.
2. Preserve the six-stage dependency DAG, buffer layouts, skinning inputs,
   per-object constants and publish barrier. Ensure no host-to-CPU roundtrip
   between passes.
3. Minimize redundant dispatches and CPU work; batch or indirect-dispatch
   where correct. Do not reorganize dependent passes merely to reduce a
   timestamp counter.
4. Compare original 64-thread topology with capabilities of RDNA,
   Intel Arc and NVIDIA subgroups. Wave32 and wave64 strategies require
   actual device qualification; no mandatory cooperative matrix features.
5. Profile full simulation GPU time, each stage GPU time, memory bandwidth,
   occupancy, queue stalls, copies/synchronization and peak resident memory
   on qualifying physical hardware.
6. Treat WARP/llvmpipe CI as correctness harnesses only. Real hardware
   acceptance is a separate milestone.
7. Use realistic animated scalp, wind, collisions, guide/follower ratios
   and multi-groom scenes. Log numerical stability alongside time and memory.

Ongoing investigations (follower static/history storage, stage scheduling,
workgroup topology and launch savings) are recorded under OPT-001, OPT-002,
OPT-003, OPT-004 and OPT-009 in
[the optimization backlog](OPTIMIZATION_BACKLOG.md).

## Immediate next implementation

Continue the existing shared-source native Vulkan/D3D12 simulation sequence
with length/wind/collision and follower publication.
Preserve source stage ordering and inter-pass dependencies without introducing
a separate scheduling pipeline. Integrate a minimal visual consumer once
renderable followers can be published, then qualify full-frame costs and
visible quality on physical GPUs against the reference. Compare optional XPBD,
inter-strand contacts and advanced rendering only against that baseline.
