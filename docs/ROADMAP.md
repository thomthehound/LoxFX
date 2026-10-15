<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# LoxFX engineering roadmap

Status: initial working plan, October 2026. Gates are evidence-driven.

See [modernization research and measured options](MODERNIZATION_RESEARCH.md)
for technique comparisons, performance criteria, and references.
Track all deferred performance opportunities in the canonical
[optimization backlog](OPTIMIZATION_BACKLOG.md).

## Objective

Provide a usable, portable, high-quality, real-time hair/fur SDK for ordinary
GPUs. Simulation and optional rendering must remain separable. A Godot
adapter is an early acceptance consumer, not the center of the architecture.

**Architecture is new; algorithms need not be.** Recover sound TressFX
components with their original license and source provenance when reusing
them is better than reimplementation. Keep the frozen reference build and
performance comparison independent of LoxFX.

## Target and performance policy

- C++23 independent core with CMake 3.28+; Windows and Linux first. No engine or GPU runtime
  dependency in asset import or CPU-only builds.
- **First-class shipping backends:** Vulkan 1.4 and D3D12 SM6.6, each with
  independently checked actual runtime capabilities, correctness and
  performance. Shader Model is not a Vulkan feature; DXIL compilation alone
  is not proof of native D3D12 execution.
- Fast native subgroup/wave arithmetic for relevant workloads; optional
  higher tiers may use larger feature sets, including matrix facilities only
  for genuinely matrix-shaped work. WMMA is not required for normal hair
  simulation.
- AMD RDNA1-class support is an architectural reference target; Radeon
  680M-class integrated graphics is an intended practical performance floor.
  Qualification depends on tested drivers, features and scene throughput.
- Probe and select device/kernel variants at initialization or explicit
  reconfiguration. No repeated capability enumeration on the dispatch path.
- Account for host scheduling, dispatch count, barriers, buffers, transfers,
  GPU occupancy, rendering latency and memory, not just shader arithmetic.
- **Quality floor:** full-system hair should look at least as good as the
  original TressFX reference at comparable configurations, preferably
  materially better (silhouette, rest-style retention, dynamics, contact
  and temporal stability). Simple stage-one numerical checks are not
  sufficient to claim this.
- **Performance requirement:** lower full-frame cost than reference on
  comparable hardware **at matched or better visible quality**, across
  representative single- and multi-groom scenes. Do not improve benchmark
  results by quietly reducing hair fidelity, density or collision workload.
- Research-only methods must earn inclusion through representative measured
  improvement over existing options.

## Boundaries

```text
Authoring formats / import -> authored groom data
                                  |
                         prepare / pack / LOD
                                  |
                          simulation contract
                            /       |       \
                           CPU    Vulkan    D3D12
                                  |
                         simulated strand data
                                  |
                  optional renderer / host adapter

Optional external integration: explicit collision and execution interfaces.
```

Authored guide geometry remains distinct from padded or compressed device
layouts. Preserve all supported imported attributes. Avoid exporting Godot,
external engine, Vulkan or D3D12 types through the central asset/simulation API.

## Godot adapter (initial reusable integration)

The optional `adapters/godot` GDExtension is being introduced as a
first-class engine adapter, not a dependency of the SDK or a Godauldron-only
experiment. Stage one imports TressFX bytes, prepares visible strands and
allows diagnostic packed position publication into a Godot line mesh. It
**does not yet run the native simulation** from a Godot scene and its CPU
mesh upload is not a production performance result. Its next gates are a
verified native Vulkan/D3D12 simulation handoff, render-queue ownership
and synchronization, higher-fidelity strand shading and validated
CPU versus GPU data transfer costs. See
[Godot adapter integration](../adapters/godot/README.md).

## Milestones

### M0 — Ground truth and ingest (initial)

- [x] Separate Apache-2.0 LoxFX history from TressFX reference history.
- [x] Establish buildable standalone C++ core, installable CMake package,
  and Windows/Linux CPU CI.
- [x] Import TressFX 4.x guide vertices and optional attributes with
  bounds checking and no implicit GPU-specific padding.
- [x] Exercise two real AMD .tfx groom assets in CI, with documented counts.
- [ ] Measure importer costs and examine .tfxbone/.tfxmesh, other
  legacy exporter assumptions and asset licenses.
- [ ] Build and profile the unmodified reference demo on intended hardware;
  record scene, quality, frame time, memory and per-pass GPU timings.
  Do not assert legacy performance without running it.

**Gate:** real TressFX asset round-trip checks and reproducible legacy baseline.

### M1 — First complete strand simulation

**GPU stages 1–4, plus length/wind:** Integration/global shape, strand-level
motion, velocity-shock propagation, local shape, and the length/wind portions
of stage five execute as internal native Vulkan and D3D12 compute passes. PRs #15 and #16 established translated attachments, compact proximal
history, and the per-guide motion data. Stage three propagates that motion
to distal guide vertices; stage four preserves local curvature and implements
the intended short/64-vertex iteration counts. Full bone-space attachments,
body/contact collision and complete animated skeletal inputs are still required.
GPU guide/follower positions and final tangents are now published by stage six.
See [follower publication](FOLLOWER_PUBLICATION_STAGE.md).
See [animated-root stage](ROOT_MOTION_STAGE.md),
[strand-level motion](STRAND_MOTION_STAGE.md),
[velocity-shock stage](VELOCITY_SHOCK_STAGE.md), and
[local-shape stage](LOCAL_SHAPE_STAGE.md) and
[length/wind stage](LENGTH_WIND_STAGE.md).

**Architecture milestone:** PR #11 merged the shared-device and command-
recording proof. PR #14 adds native D3D12 stage-one execution against
the same shader and host-owned submission contract. Both test harnesses
remain internal, not shippable SDK APIs; full six-stage simulation,
visual parity and actual consumer-GPU performance remain outstanding. See
[GPU runtime architecture](GPU_RUNTIME_ARCHITECTURE.md) and
[issue #13](https://github.com/thomthehound/LoxFX/issues/13).


- [x] Trace source stage order, master strand indexing, dispatch counts, and
  unused follower work; implement checked source-aligned guide-work planning.
- [x] Prepare interleaved persistent guide/follower buffers, rest lengths,
  tangents and provenance without modifying imported authoring data.
- [x] Demonstrate an **experimental** native Vulkan/DXC integration-stage
  smoke test with persistent buffers, synchronization, readback assertions
  and timestamps (first stage only; not a production backend API).
- [x] Replace the experimental per-groom Vulkan runtime with a shared
  standalone/borrowed device context, per-groom RAII buffers, and record-only
  compute methods. Test two independent grooms using one logical device.
- [ ] Validate host-supplied command buffer interoperability, fault-injected
  resource creation, frames in flight and production scheduling.
- [x] Compile identical first-stage HLSL to Vulkan SPIR-V and D3D12 SM6.6
  DXIL in Windows CI, with backend-neutral parameter semantics.
- [x] Execute the same first-stage HLSL natively through D3D12 DXIL on WARP,
  with root descriptors/constants, host-controlled command recording,
  explicit fences/timestamps and numerical parity fixtures (PR #14).
- [ ] Implement full animated bone-space attachments, then validate
  source-faithful stage dependencies and publication on both native APIs.
- [ ] After follower publication and a minimal visual consumer, qualify
  full-frame and per-pass GPU timings on representative physical devices.
- [ ] Build a native GPU executor for the whole source stage chain; the
  work-plan alone does not simulate hair or validate a shader port.

- Port an identifiable complete TressFX guide-strand simulation pipeline,
  preserving integration, global/local shape constraints, velocity shock
  propagation, wind, length iterations, follow strands and dispatch ordering.
- Account for original engine-interface resource ownership, barriers,
  scheduling and state publication; no orphan compute-kernel collection.
- Vulkan and D3D12 share the canonical shader mathematics and neutral
  simulation inputs, with backend-specific binding and execution. Build and
  test both backends at each meaningful solver milestone; preserve a
  deterministic cross-backend fixture and source-reference comparisons.
- Distinguish authored data from derived per-device representations.

**Gate:** a complete moving-groom simulation produces testable renderable
strands on both native backends. Physical-GPU timing and visual-quality
qualification follow with a meaningful visual consumer.

### M2 — Robust contact, authoring, integration

- Reconcile multi-hair constant-buffer ownership and mixed-length constraints.
- Validate wind behavior, moving-body collision, SDF generation/update and
  predictable synchronization with animated attachment geometry.
- Expose a minimal Godot adapter plus an independent native sample, both
  using the same LoxFX engine and simulation state.
- Add Blender import/export and authoring-preserving conversions where useful.

**Gate:** dynamic characters with multiple grooms and colliders behave
correctly and run at interactive rates without a per-frame readback loop.

### M3 — Measured modernization

- GPU-driven guide/follow strand LOD and indirect dispatch/compaction.
- Compare legacy constraint solver with XPBD-style compliance, not a forced
  rewrite. Test convergence, hair style retention, stretching, curling,
  damping, wind response, contacts and temporal stability.
- Evaluate performant hybrid hair-hair interaction approximations.
- Offer separate low/medium/high-cost rendering paths and quality metrics.
- Test on real integrated and discrete GPUs; maintain low-overhead defaults.

**Gate:** reproducible quality/performance gains on representative grooms,
including multi-character scenes.

### M4 — Production SDK and optional interoperability

- Stable library/asset ABI decisions, packaging, diagnostics, scheduling API,
  device-lost behavior, lifetime and synchronization documentation.
- Independently test GPU backends and integrations on AMD, NVIDIA and Intel
  as qualifying devices become available.
- Evaluate optional integration with external simulation systems only after
  the standalone runtime is functional and profiled.

## Do not undertake by default

No whole-engine fork or mandatory external physics, GPU-vendor SDK, game-engine or sample-framework dependency.
No early architectural rewrite around ray-traced hair, full rod dynamics,
cooperative matrices, ML inference, or Work Graphs. No slow software-like
fallback becoming the shipping baseline. Each advanced option requires a
useful scene, a correctness oracle and a measured performance argument.

## Required benchmark record

Track strands and vertices (simulated and rendered separately), strand length
distribution, constraints/iterations, dynamic colliders, dt, physics quality
(especially penetration, jitter and stretching), GPU milliseconds per pass,
dispatch/synchronization costs, transfers, peak memory and frame latency.
Include at least one fixed camera-quality comparison at each LOD. Report
actual physical hardware and backend, and separate measurements from estimates.
