<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Optimization backlog

Reviewed 15 October 2026. This is the **canonical, persistent record** of
deferred or active performance opportunities. Update it at discovery and before
every substantial handoff as specified in [AGENTS.md](../AGENTS.md).
`docs/ROADMAP.md` tracks milestones; this file tracks optimization leads.

Blocked research candidates need no separate GitHub issue until actionable; substantial active work has one linked issue. Promote candidates when prerequisites are met; avoid creating ten speculative issue threads.

Status: `candidate`, `blocked`, `ready`, `testing`,
`implemented-awaiting-measurement`, `done`, `rejected`.
A claim about launch counts, theoretical saved bytes, or another project's
benchmark is not a measured LoxFX performance result. Close items only with
reproducible GPU and correctness evidence, or documented rejection.

| ID | Priority | Status | Prerequisite | Opportunity |
| --- | --- | --- | --- | --- |
| OPT-001 | P0 | blocked | Persistent GPU buffer layouts | Eliminate redundant static follower buffers |
| OPT-002 | P0 | blocked | Dynamic GPU state/renderer handoff | Store simulation histories for guides, not followers, where possible |
| OPT-003 | P0 | blocked | Working six-stage GPU executor | Reduce host dispatch, descriptor and barrier overhead safely |
| OPT-004 | P1 | blocked | Hardware shader profiling | Tune wave/group size and shared-memory mapping |
| OPT-005 | P1 | blocked | Multi-character reference scene | Simulation/render LOD, culling and update cadence |
| OPT-006 | P1 | blocked | Dynamic colliders | SDF generation/update/reuse and multi-groom sharing |
| OPT-007 | P1 | blocked | Working constraints with tests | Optimize iteration strategy and inter-pass synchronization |
| OPT-008 | P2 | blocked | GPU correctness and numerical baseline | Compact and mixed-precision device buffers |
| OPT-009 | P0 | implemented-awaiting-measurement | Physical GPU baseline | Verify elimination of legacy master/follower over-dispatch |
| OPT-010 | P1 | blocked | Quality and GPU timing baseline | Higher-fidelity follower interpolation per simulated guide |
| OPT-011 | P1 | blocked | Multi-groom persistent GPU runtime | Suballocate buffers and manage temporary staging lifetime |
| OPT-012 | P1 | blocked | Native engine handoff and visual baseline | Avoid diagnostic per-frame CPU strand transfer and reconstruct/render on GPU |

## OPT-012 — Production engine output and diagnostic transfer overhead

**Observation / source:** the initial `adapters/godot/src/loxfx_groom_3d.cpp`
supports diagnostic packed CPU positions and Godot vertex-buffer updates.
The optional `LOXFX_GODOT_NATIVE_DIAGNOSTIC` mode now also records LoxFX's
actual six native compute stages and synchronously snapshots published
follower positions via its existing diagnostic readback before those line
vertices are uploaded. This proves a working consumer boundary without
claiming a production hardware path.
Its per-frame line-vertex expansion and CPU-to-GPU upload scale with every
rendered guide and follower. That is an intentional temporary inspection
path, **not** the eventual native renderer. The existing native Vulkan
and D3D12 GPU backends already produce resident positions/tangents.

**Suspected cost:** one host-visible position snapshot and an expanded
line-vertex upload per displayed frame; unmeasured, strongly dependent on
groom density, frame count and engine renderer.

**Experiment / prerequisite:** once the LoxFX GPU execution boundary and
Godot native rendering-device lifecycle are demonstrated, compare the
diagnostic copy path with host-owned GPU-resident strand consumption.
Measure simulation GPU time, producer/consumer synchronization, transfer
latency, render cost, frame pacing and visual equivalence on the same scene,
on both applicable native backends. Keep the standalone SDK baseline.

**Status:** blocked pending GPU interoperability and real visual rendering.
**Acceptance:** no forced frame-time GPU->CPU readback, accurate quality and
sync, and quantified transfer/whole-frame improvement on actual hardware.
No speedup or memory saving is claimed.

## OPT-001 — Duplicate static follower data

**Observation:** PR #3's `src/prepared_groom.cpp` constructs rest
positions, rest tangents and rest segment lengths for every visible follower.
The reference shaders simulate master guides and reconstruct follower
geometry in the final expansion stage. This is a useful compatibility layout
but may inflate upload and resident memory.

**Analytical opportunity:** Current prepared static sizes for the real AMD
Ratboy smoke tests are **10,465,280 bytes** (mohawk, three followers/guide)
and **22,958,080 bytes** (short hair, one follower/guide). Making the
36-byte-per-vertex rest-state arrays guide-only could save approximately
**7.74 MB** and **10.87 MB** respectively, *if* the actual GPU consumers permit
it. These are hypothetical byte reductions, **not** measured GPU savings.

**Experiment:** At first GPU allocation, audit reads by simulation, initial
reset, collision and rendering. Compare guide-only immutable rest state +
follower offsets/output against the full-strand baseline. Measure GPU
resident bytes, upload/init time, full-frame performance, and geometry
equivalence on real assets. Keep any required follower render output.

**Issue:** [#4](https://github.com/thomthehound/LoxFX/issues/4).
**Prerequisite:** first GPU storage/binding implementation.
**Status:** implemented-awaiting-measurement for guide-only GPU rest-length
storage and compact stage-six follower publication metadata. Stage six no
longer reads follower rest vertices: immutable guide segment lengths and
16-byte-per-strand root offsets/tip coefficients share one GPU static
allocation and binding. The full follower rest-position buffer is still
uploaded at groom initialization, so the P0 reduction remains **blocked**
until all other consumers (including initialization and rendering) are
audited and matched on real hardware. The extra compact metadata footprint
is `16 * packed_total_strands` bytes analytically; no measured memory or
runtime saving is claimed.
**Source:** [PR #3](https://github.com/thomthehound/LoxFX/pull/3),
[length/wind stage](LENGTH_WIND_STAGE.md), and
[follower publication](FOLLOWER_PUBLICATION_STAGE.md).

**Evidence / source:** `src/prepared_groom.cpp`; six master-index shaders and follower expansion in the TressFX 4.1 reference; [PR #3](https://github.com/thomthehound/LoxFX/pull/3).
**Acceptance:** same authored/style/pinning behavior, collision and renderer compatibility, with measured lower static GPU residency or transfer cost and no unacceptable frame-time increase. Compare representative 0/1/3-follower assets.

## OPT-002 — Full-size dynamic follower history

**Observation:** The source integration and VSP shaders use only master
guide indices, but the legacy dynamic state allocates multiple position
history resources sized to the total strand population.

**Experiment:** Track every reader/writer, including reset, SDF contact and
renderer handoff; compare guide-only history plus full follower published
positions/tangents with full history. Validate behavior at 0/1/3+
followers, VRAM and frame time.

**Prerequisite:** dynamic resource ownership design. **Status:** blocked.

**Evidence / source:** TressFX 4.1 `TressFXHairObject.cpp` dynamic-state allocation and `TressFXSimulation.hlsl` master-index passes. **Suspected cost:** potentially large history buffers proportional to followers; unquantified until live buffer inventory is measured.
**Acceptance:** reset, motion/contact and publication correctness with lower measured VRAM/upload requirements or reduced bandwidth, no new readbacks and no unacceptable frame-time regression. [Issue #7](https://github.com/thomthehound/LoxFX/issues/7).

## OPT-003 — Stage-major scheduling and unnecessary synchronization

**Architecture prerequisite:** [issue #13](https://github.com/thomthehound/LoxFX/issues/13)
requires replacing per-groom Vulkan device, pipeline, submission and fence
ownership with a shared backend context and host-controlled pass recording.
Profiling fewer barriers on the existing synchronous per-groom object would
optimize the wrong ownership boundary.


**Observation:** Six stages dispatch across all grooms with per-object
bindings and barriers. Short strands can repeat local-shape dispatches; the
source may omit a memory visibility barrier between those iterations.

**Experiment:** First establish correct stage/resource dependencies. Measure
GPU timestamps, submission overhead, barriers, stalls and CPU driver cost
before testing batching, prepared descriptors or safe pass fusion. Never
delete a synchronization dependency based on a theoretical speedup.

**Prerequisite:** correct GPU executor. **Status:** blocked.
**Source:** [simulation execution trace](SIMULATION_EXECUTION_TRACE.md).

**Evidence / source:** TressFX 4.1 `TressFXSimulation.cpp::Simulate` and `DispatchComputeShader`; [source synchronization trace](SIMULATION_EXECUTION_TRACE.md). **Suspected cost:** per-object dispatch/bind/barrier overhead and stalls; not quantified.
**Acceptance:** validated cross-pass visibility, multi-groom parity and demonstrably lower measured CPU submission or GPU frame time with stable animation. [Issue #8](https://github.com/thomthehound/LoxFX/issues/8).

## OPT-004 — Wave/subgroup topology

**Observation:** Source uses 64-thread workgroups and strand layouts tied
to 4–64 vertices. Alternative wave32/wave64 arrangements may improve occupancy
but change shared-memory indexing and synchronization.

**Experiment:** Profile a small number of coherent variants on actual AMD,
Intel and NVIDIA hardware when available, monitoring occupancy, registers,
barriers and divergence. Select at device preparation, not per dispatch.

**Prerequisite:** running shaders and timings. **Status:** blocked.

**Evidence / source:** TressFX 4.1 `TressFXSimulation.hlsl` (`THREAD_GROUP_SIZE=64`, group-shared arrays and barrier sites), `TressFXConstantBuffers.h`. **Suspected cost:** subgroup underutilization, occupancy or shared-memory pressure; not measured and may be zero on many GPUs.
**Acceptance:** correct layout/synchronization and repeatably lower per-stage **and** whole-frame GPU time on a documented supported GPU; retain the baseline if alternatives lose. No issue while awaiting shader/hardware baseline.

## OPT-005 — GPU-driven LOD and visibility

**Observation:** Guides/followers receive simulation work regardless of
their projected screen-space benefit, visibility or importance to animation.

**Experiment:** Measure fidelity and latency while varying guide count,
follower count, update frequency, culling and indirect dispatch in a real
multi-groom scene; avoid perceptible popping and unstable contacts.

**Prerequisite:** scene and baseline renderer. **Status:** blocked.

**Evidence / source:** stage/guide/follower work-count model `src/simulation_work.cpp`, [execution trace](SIMULATION_EXECUTION_TRACE.md). **Suspected cost:** offscreen/distant groom updates scale with unchanged guide counts; scene-level cost not measured.
**Acceptance:** benchmarked frame-time reduction at controlled visual error, no visible temporal LOD popping or contact regressions, plus stable reentry after culling. [Issue #9](https://github.com/thomthehound/LoxFX/issues/9).

## OPT-006 — Reuse and update SDF collision efficiently

**Observation:** Legacy SDF generation and collision require additional
passes/transitions. Upstream reported incorrect shared per-groom collision
constants ([issue #46](https://github.com/GPUOpen-Effects/TressFX/issues/46)).

**Experiment:** Separate static from moving colliders, establish safe
per-collider ownership, reuse/rebuild only where valid, test multi-groom
penetration and measure dispatches, bandwidth and latency.

**Prerequisite:** working collision pipeline. **Status:** blocked.

**Evidence / source:** TressFX 4.1 `TressFXSDFCollision.cpp`, `TressFXSDFCollision.hlsl`, [upstream issue #46](https://github.com/GPUOpen-Effects/TressFX/issues/46). **Suspected cost:** repeated SDF compute/transitions for unchanged geometry and risk of constant-buffer cross-talk; unmeasured.
**Acceptance:** no multi-groom corruption, bounded penetration under animation, fewer measured unnecessary SDF builds/transfers and equal or lower full-frame GPU time. [Issue #10](https://github.com/thomthehound/LoxFX/issues/10).

## OPT-007 — Constraint iteration work

**Observation:** The 64-vertex source shader ignores its internal local
iteration uniform, while shorter strands use repeated host dispatches;
length constraints incur workgroup barriers.

**Experiment:** Reproduce the intended behavior first, compare convergence
and visual stability against host-loop and shader-loop strategies, then test
safe fusion and convergence-aware iteration counts with timestamps.

**Prerequisite:** complete solver and visual consumer for iteration-cost comparison.
**Status:** implemented-awaiting-measurement (correct local-shape iteration
semantics; length-constraint iteration optimization remains pending).
**Source:** [local-shape stage](LOCAL_SHAPE_STAGE.md) and
[simulation execution trace](SIMULATION_EXECUTION_TRACE.md).

**Evidence / source:** `LocalShapeConstraints` and `LengthConstriantsWindAndCollision` in TressFX 4.1 `TressFXSimulation.hlsl`; 64-vertex iteration discrepancy in [execution trace](SIMULATION_EXECUTION_TRACE.md). **Suspected cost:** host dispatches or synchronization per iteration; not quantified.
**Acceptance:** source-intended constraint convergence/length error and temporal stability with lower measured stage/full-frame cost, or explicit rejection. Open a dedicated issue when the correct solver is running.

## OPT-008 — Storage compression

**Observation:** float4 state and tangents may impose avoidable bandwidth.
FP16 or packed tangents could help on some devices but cause errors at
small hair scales, large world coordinates or collision boundaries.

**Experiment:** Compare mixed precision with full FP32 under fast animation
and tight contact; include numerical error, VRAM and full-frame GPU latency.
Never require reduced precision as the only baseline.

**Prerequisite:** real GPU correctness oracle. **Status:** blocked.

**Evidence / source:** source `float4` position/tangent history layouts in TressFX 4.1 and `include/loxfx/prepared_groom.h`. **Suspected cost:** FP32 bandwidth and memory traffic; format suitability unknown and presently unmeasured.
**Acceptance:** validated bounded positional/tangent error, collision correctness and repeatably lower device traffic and frame time on representative real GPUs. Keep FP32 if mixed precision loses. Open issue when a precision/bandwidth bottleneck is demonstrated.

## OPT-009 — Prove guide-only launch benefits

**Observation:** [PR #2](https://github.com/thomthehound/LoxFX/pull/2)
fixed the analytical work plan's follower over-dispatch and exact-multiple
64-guide over-padding. Real AMD hair assets confirm projected lane counts,
**not runtime speedups**.

**Experiment:** Measure actual GPU time at the same scenes/quality, using a
safe reference execution or trace without intentionally dispatching
out-of-bounds source groups. Record comparisons, driver and memory behavior.

**Prerequisite:** working GPU backend. **Status:** implemented-awaiting-measurement.

**Evidence / source:** `src/simulation_work.cpp`, real AMD Ratboy smoke tests, [PR #2](https://github.com/thomthehound/LoxFX/pull/2). **Suspected cost:** 2–4× excess *logical lanes* in reference workloads and an extra block for aligned guide counts; GPU time remains unmeasured.
**Acceptance:** a safe, genuinely GPU-executed reference comparison that quantifies per-pass/full-frame timing and keeps guide/follower quality and output equivalent. Runtime tracked by [issue #6](https://github.com/thomthehound/LoxFX/issues/6).

## OPT-010 — Better visible followers per simulated guide

**Observation:** Source root offsets are inexpensive but imperfect for
curl, contact and motion. Better interpolation may increase fidelity without
independently simulating every visible hair.

**Experiment:** Once both visuals and timings exist, compare guide-driven
schemes on straight/curly grooms across motion/contact, recording GPU work,
memory and perceptual error. See
[modernization research](MODERNIZATION_RESEARCH.md).

**Prerequisite:** visual and performance baseline. **Status:** blocked.

**Evidence / source:** TressFX 4.1 `UpdateFollowHairVertices` linear root-offset formula; physically guided interpolation approaches described in [modernization research](MODERNIZATION_RESEARCH.md). **Suspected cost:** additional computation could increase runtime; possible fidelity-per-guide gain unmeasured.
**Acceptance:** documented visual-motion improvement at controlled simulated guide count with practical full-frame performance on consumer GPU hardware. Open dedicated issue after a visual baseline and style fixtures exist.

## OPT-011 — Dedicated Vulkan allocation overhead and staging lifetime

**Architecture prerequisite:** [issue #13](https://github.com/thomthehound/LoxFX/issues/13)
moves device-level allocation policy and upload staging outside a groom;
this avoids per-groom device and pipeline duplication before any custom
suballocator work. No measured savings yet.


**Evidence / source:** the deleted monolithic
`src/vulkan/integration.cpp` created full devices and retained upload
staging per groom. Its replacement `src/vulkan/smoke_context.cpp`
has one explicit device context, independent groom GPU buffers and
temporary host staging freed after its initialization/diagnostic
submission completes. Three dynamic/rest buffer allocations per groom
are still dedicated and have no measured performance baseline.

**Suspected cost:** multiple dedicated Vulkan allocations can fragment
heaps and add allocation overhead at scene load. Shared staging and
suballocation may help, but no multi-groom hardware measurements exist.
`GroomState::allocated_buffer_bytes()` reports the three allocated
device buffers (not VRAM residency or transient staging).

**Experiment:** after multiple grooms and asynchronous command submission
work, compare shared staging suballocation/recycling and device-local
suballocation against dedicated allocations. Measure host/VRAM allocation
budgets, upload latency, GPU stalls, full-frame time and fragmentation on
consumer hardware. No early allocator framework before profiling.

**Acceptance:** reduced measured committed memory or allocation overhead
without broken transfer fences, ownership, correctness, or slower whole-frame
throughput. **Status:** blocked until multi-groom runtime and GPU profiling.

## Shared diagnostic devices and multi-groom scheduling (OPT-003/011)

**2026-10-15, PR #24 Codex P1 follow-up:** The first
`adapters/godot/src/native_diagnostic.cpp` implementation constructed a
standalone native `DeviceContext` inside every `LoxFXGroom3D` instance.
With N grooms this repeated device selection, invariant pipeline compilation
and GPU device/context allocation N times, violating the existing
device-versus-groom ownership contract.

**Current correction:** compatible native diagnostic grooms now reuse one
weakly cached device/queue and six-stage pipeline collection, keyed by the
absolute normalized shader path and D3D12 hardware/WARP preference.
Each groom retains its own GPU `GroomState`; no implicit adapter migration
occurs. The diagnostic recording, queue submission and readback are serialized
across users because the existing diagnostic command allocator/list is
shared. The last groom releases the device without a permanent GPU cache.
Two real grooms with different follower populations exercise state isolation,
independent step/reset and final-device teardown in the Godot runtime smoke.

**Remaining cost:** repeated CPU↔GPU diagnostic readbacks and
one wait+submission per groom are **not** fixed. Serialized multi-groom
diagnostics are deliberately non-production; a future host-owned
batched/asynchronous submission mode must preserve dependencies and
groom-local histories (OPT-003) and later evaluate allocation reuse
(OPT-011). No physical-GPU device-memory or timing reduction is claimed.

**Verification needed:** multi-groom Linux Vulkan/Godot smoke,
Windows D3D12 native build, then real hardware scene-to-frame profiling
comparing shared contexts with independent diagnostics and future batched
execution. Correctness is a prerequisite; `OPT-003` and `OPT-011`
remain blocked for production optimization.

## Evidence and handoff log

- **2026-10-15, PR #15:** Built a compact previous-previous GPU root
  history holding two `Float4` values per padded guide, because TressFX
  `CalculateStrandLevelData` reads only root vertices 0 and 1. This
  avoids a second full-vertex/follower history buffer. Other simulation
  history and authored follower data remain; OPT-001/002 are **not**
  completed and the memory choice has not been profiled on hardware.
  Added host-driven root translation without per-frame CPU groom uploads,
  preserving both native backends.


- **2026-10-15, PR #2:** Source-derived six-pass work plan and verified guide
  lane counts. Optimized launch savings await GPU measurement (OPT-009).
- **2026-10-15, PR #3:** Deterministic prepared interleaved follower data,
  real Ratboy smoke tests and static upload sizes. Retain current
  correctness path until GPU buffer consumers are audited (OPT-001/002).
- **2026-10-15, native D3D12 stage-one PR #14:** The shared HLSL now
  executes through actual D3D12 compute with three persistent committed
  per-groom buffers, root SRV/UAV descriptors and root constants, so this
  stage uses **no descriptor heap or per-dispatch constant-buffer upload**.
  Compare full-frame costs with Vulkan only on physical GPUs (OPT-003/009).
  Dedicated per-groom committed resources remain an OPT-011 profiling
  opportunity; all follower memory questions remain OPT-001/002.
- **2026-10-15, architecture remediation:** Replaced the per-groom
  device-owning runtime with an internal shared/borrowed Vulkan context,
  record-only integration, two-groom CI and temporary setup/readback staging.
  Frame submission and multi-groom allocation optimization remain OPT-003
  and OPT-011, unmeasured on consumer GPUs.
- **2026-10-15, subsequent Vulkan review:** Added separate allocation-size
  admission and transactional allocation retry across eligible adapters;
  this is correctness work during initialization, not a frame speedup.
  Multi-groom allocation reuse remains OPT-011.
- **2026-10-15, Vulkan stage-one review:** Validated mutable prepared
  topology before GPU access and selected a groom-compatible adapter.
  Removed duplicate CPU upload copies and included staging in actual
  allocation counts. Deferred multi-groom suballocation/reuse (OPT-011).
- **2026-10-15, Vulkan stage-one:** Persistent current/previous/rest device
  buffers and a single source-derived integration pass added as an opt-in
  native Vulkan runner. The synchronous batch fence is a correctness harness,
  not the production frame scheduler (OPT-003). Full-strand static/history
  allocations remain for now (OPT-001, OPT-002). Hardware timings pending.
- **2026-10-15:** Created this canonical list and required capture rule in
  `AGENTS.md`. No physical-GPU performance data exists yet.

Add evidence and decisions at each meaningful PR/handoff; never delete
unresolved leads merely because their implementation is postponed.

- **2026-10-15, PR #16 stage two:** Implemented the guide-only
  strand-level output in GPU memory (48 bytes per padded guide) and
  reused the two-root previous-previous state; no follower output or
  per-frame CPU readback. D3D12 reuses UAV states between the first
  two shader passes rather than inserting extra resource transitions.
  Structural savings only: OPT-001/002/003/009/011 remain open until
  actual consumer hardware timing and fidelity comparisons.
