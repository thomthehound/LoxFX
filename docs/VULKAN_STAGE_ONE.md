<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Vulkan integration stage — first executable GPU milestone

**Architectural status: EXPERIMENTAL HARNESS ONLY — NOT MERGE-READY AS AN
SDK RUNTIME.** The current implementation bundles Vulkan instance/device,
pipeline, per-groom buffers, queue submission and fence waits inside one
integration object. See [architectural audit](GPU_RUNTIME_ARCHITECTURE.md)
and [issue #13](https://github.com/thomthehound/LoxFX/issues/13)
for the required host-controlled, multi-groom correction.

Status: first-stage candidate. Issue [#6](https://github.com/thomthehound/LoxFX/issues/6)
remains open for the full six-stage GPU strand solver.

## What is implemented

- A real native Vulkan compute pipeline, compiled from one HLSL source using
  DXC to SPIR-V (SM6.6 target). Source-derived guide-only dispatch indexing.
- Persistent device-local current, previous and rest position buffers,
  prepared/uploaded once per groom. A **single-image** host-visible staging
  buffer is shared for initial uploads to all three buffers and **explicit
  diagnostic readback only**, rather than storing three redundant host copies.
- GPU Verlet integration, damping, gravity, fixed attachment roots and
  source-style global rest-shape stiffness, currently with **static roots**.
- Batched command submission with memory dependency between sequential
  physics steps and no per-step CPU history copy/readback. GPU timestamps
  for the batch when the selected queue supports them.
- A deterministic two-strand fixture (one guide + one follower), checked
  against analytical results on the Vulkan runtime in Linux CI.

## Source and differences

Based on the mathematics and `CalcIndicesInVertexLevelMaster` indexing in
the reference `TressFXSimulation.hlsl`, function
`IntegrationAndGlobalShapeConstraints`, and
`UpdateFinalVertexPositions`. The new HLSL is independently adapted:
https://github.com/thomthehound/TressFX/blob/master/src/Shaders/TressFXSimulation.hlsl

Not yet ported: the original's bone skinning and animated attachment matrices;
position-prev-prev history for velocity shock propagation; all five later
simulation stages; dynamic collision/SDF; wind; follower publication;
renderer/visual acceptance; compatibility with external command queues.

**Do not infer source parity, performance improvement, or a complete
working hair simulator from the one-stage smoke test.**

## Dual-backend parity

D3D12 SM6.6 remains a first-class production target. The same first-stage
HLSL compiles to DXIL and SPIR-V, with a backend-specific parameter binding
transport (D3D12 b0 CBV; Vulkan push constants). The native D3D12 runner is
**not implemented yet**. CPU-side parameter semantics are now independent of
the Vulkan namespace. See [BACKEND_PARITY.md](BACKEND_PARITY.md) for the
tracked cross-backend acceptance requirements.

## Runtime and performance boundaries

The first executor selects a capable hardware Vulkan compute device (discrete
before integrated, then other GPU adapters) before a compatible software
adapter. Selection checks the requested groom's buffer, dispatch and shader
limits and the independently specified `maxMemoryAllocationSize` once at
initialization. Each eligible adapter is then tested against **actual buffer
allocation requirements**: a failed partial allocation is cleaned up and
the next candidate is tried. Descriptor size and nominal heap sizes alone
do not guarantee available memory. This never occurs per frame. It implements
an API 1.2 Vulkan subset so software Vulkan CI
can execute it. Intended shipping qualification remains **Vulkan 1.4**;
runtime feature admission and compatibility are not complete. Shader compilation
is build-time through DXC; no runtime compiler is loaded by the SDK.

The diagnostic `advance(steps)` path records multiple steps into one
command buffer and waits for a final fence once per batch. This is useful
for source-grounded correctness/per-pass instrumentation, **not** a
production async frame scheduler. Frame-in-flight synchronization, external
queue use, animated roots and rendering handoff are the next work. The
diagnostic readback is explicitly user-requested and never happens during
advance(). Buffer allocations happen at initialization only.

Memory accounting reports **actual `vkAllocateMemory` allocation sizes**
for the three persistent device buffers and the persistent host staging
buffer, including Vulkan's allocation padding. The aggregate is not a
driver-observed VRAM residency metric; its staging portion is reported
separately. GPU memory savings from alternative representations have not
been measured.

Timestamp duration includes the batch of step-one dispatches and internal
compute barriers. Software device times are not consumer GPU performance
evidence. Record actual hardware/driver and scene settings before making
throughput claims.

## Unresolved optimization opportunities

Review canonical [optimization backlog](OPTIMIZATION_BACKLOG.md).
OPT-001: replace full-strand immutable rest upload with guide-only state.
OPT-002: avoid allocating previous position history for followers.
OPT-003: replace synchronous batch fence with production GPU frames-in-flight
and integrate the six-stage dependency graph.
OPT-009: measure actual guide-only dispatch performance.
These are **not** closed by this implementation.

## Next source-faithful step

Extend persistent buffers to prev-prev history, root/bone transforms and
strand-level data, then the CalculateStrandLevelData + VSP passes with
per-stage barriers and timestamps. Later add local/length constraints,
wind/contact and follower publication. Only then is the complete solver
available for hardware and fidelity qualification.
