<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Optimization backlog

Reviewed 15 October 2026. This is the **canonical, persistent record** of
deferred or active performance opportunities. Update it at discovery and before
every substantial handoff as specified in [AGENTS.md](../AGENTS.md).
`docs/ROADMAP.md` tracks milestones; this file tracks optimization leads.

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
**Status:** blocked. **Source:** [PR #3](https://github.com/thomthehound/LoxFX/pull/3).

## OPT-002 — Full-size dynamic follower history

**Observation:** The source integration and VSP shaders use only master
guide indices, but the legacy dynamic state allocates multiple position
history resources sized to the total strand population.

**Experiment:** Track every reader/writer, including reset, SDF contact and
renderer handoff; compare guide-only history plus full follower published
positions/tangents with full history. Validate behavior at 0/1/3+
followers, VRAM and frame time.

**Prerequisite:** dynamic resource ownership design. **Status:** blocked.

## OPT-003 — Stage-major scheduling and unnecessary synchronization

**Observation:** Six stages dispatch across all grooms with per-object
bindings and barriers. Short strands can repeat local-shape dispatches; the
source may omit a memory visibility barrier between those iterations.

**Experiment:** First establish correct stage/resource dependencies. Measure
GPU timestamps, submission overhead, barriers, stalls and CPU driver cost
before testing batching, prepared descriptors or safe pass fusion. Never
delete a synchronization dependency based on a theoretical speedup.

**Prerequisite:** correct GPU executor. **Status:** blocked.
**Source:** [simulation execution trace](SIMULATION_EXECUTION_TRACE.md).

## OPT-004 — Wave/subgroup topology

**Observation:** Source uses 64-thread workgroups and strand layouts tied
to 4–64 vertices. Alternative wave32/wave64 arrangements may improve occupancy
but change shared-memory indexing and synchronization.

**Experiment:** Profile a small number of coherent variants on actual AMD,
Intel and NVIDIA hardware when available, monitoring occupancy, registers,
barriers and divergence. Select at device preparation, not per dispatch.

**Prerequisite:** running shaders and timings. **Status:** blocked.

## OPT-005 — GPU-driven LOD and visibility

**Observation:** Guides/followers receive simulation work regardless of
their projected screen-space benefit, visibility or importance to animation.

**Experiment:** Measure fidelity and latency while varying guide count,
follower count, update frequency, culling and indirect dispatch in a real
multi-groom scene; avoid perceptible popping and unstable contacts.

**Prerequisite:** scene and baseline renderer. **Status:** blocked.

## OPT-006 — Reuse and update SDF collision efficiently

**Observation:** Legacy SDF generation and collision require additional
passes/transitions. Upstream reported incorrect shared per-groom collision
constants ([issue #46](https://github.com/GPUOpen-Effects/TressFX/issues/46)).

**Experiment:** Separate static from moving colliders, establish safe
per-collider ownership, reuse/rebuild only where valid, test multi-groom
penetration and measure dispatches, bandwidth and latency.

**Prerequisite:** working collision pipeline. **Status:** blocked.

## OPT-007 — Constraint iteration work

**Observation:** The 64-vertex source shader ignores its internal local
iteration uniform, while shorter strands use repeated host dispatches;
length constraints incur workgroup barriers.

**Experiment:** Reproduce the intended behavior first, compare convergence
and visual stability against host-loop and shader-loop strategies, then test
safe fusion and convergence-aware iteration counts with timestamps.

**Prerequisite:** functioning solver. **Status:** blocked.
**Source:** [simulation execution trace](SIMULATION_EXECUTION_TRACE.md).

## OPT-008 — Storage compression

**Observation:** float4 state and tangents may impose avoidable bandwidth.
FP16 or packed tangents could help on some devices but cause errors at
small hair scales, large world coordinates or collision boundaries.

**Experiment:** Compare mixed precision with full FP32 under fast animation
and tight contact; include numerical error, VRAM and full-frame GPU latency.
Never require reduced precision as the only baseline.

**Prerequisite:** real GPU correctness oracle. **Status:** blocked.

## OPT-009 — Prove guide-only launch benefits

**Observation:** [PR #2](https://github.com/thomthehound/LoxFX/pull/2)
fixed the analytical work plan's follower over-dispatch and exact-multiple
64-guide over-padding. Real AMD hair assets confirm projected lane counts,
**not runtime speedups**.

**Experiment:** Measure actual GPU time at the same scenes/quality, using a
safe reference execution or trace without intentionally dispatching
out-of-bounds source groups. Record comparisons, driver and memory behavior.

**Prerequisite:** working GPU backend. **Status:** implemented-awaiting-measurement.

## OPT-010 — Better visible followers per simulated guide

**Observation:** Source root offsets are inexpensive but imperfect for
curl, contact and motion. Better interpolation may increase fidelity without
independently simulating every visible hair.

**Experiment:** Once both visuals and timings exist, compare guide-driven
schemes on straight/curly grooms across motion/contact, recording GPU work,
memory and perceptual error. See
[modernization research](MODERNIZATION_RESEARCH.md).

**Prerequisite:** visual and performance baseline. **Status:** blocked.

## Evidence and handoff log

- **2026-10-15, PR #2:** Source-derived six-pass work plan and verified guide
  lane counts. Optimized launch savings await GPU measurement (OPT-009).
- **2026-10-15, PR #3:** Deterministic prepared interleaved follower data,
  real Ratboy smoke tests and static upload sizes. Retain current
  correctness path until GPU buffer consumers are audited (OPT-001/002).
- **2026-10-15:** Created this canonical list and required capture rule in
  `AGENTS.md`. No physical-GPU performance data exists yet.

Add evidence and decisions at each meaningful PR/handoff; never delete
unresolved leads merely because their implementation is postponed.
