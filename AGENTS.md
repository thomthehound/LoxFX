<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# LoxFX contributor and coding-agent instructions

Read the project roadmap and [optimization backlog](docs/OPTIMIZATION_BACKLOG.md)
before substantial implementation or performance work. These files carry
important decisions across sessions.

## Non-negotiable project rules

- LoxFX is independent and engine-neutral. Do not reference unrelated
  experimental projects in its source, docs, issues, PRs or release notes.
- New code requires **C++23**, CMake 3.28+, Apache-2.0 SPDX headers, and
  `windows-latest` / Ubuntu CI. Preserve MIT notices on imported AMD source.
- **D3D12 SM6.6 and Vulkan 1.4 are first-class production targets.** Any
  common shader stage must compile to both DXIL and SPIR-V on Windows CI.
  Do not deepen one backend without tracking the other to parity; keep
  backend-specific binding, scheduling and resource code in separate modules.
  Native D3D12 runtime parity is required, not replaced by DXIL compilation.
- **Device/context ownership is outside per-groom simulation.** A groom must
  never create its own graphics device, select another adapter implicitly,
  compile device-invariant pipelines, or submit/fence-wait on every stage.
  Host-owned and explicit standalone device contexts share the same recorder.
  GPU resources must be failure-safe RAII with owned/borrowed handles clear.
  See [GPU runtime architecture](docs/GPU_RUNTIME_ARCHITECTURE.md).
- Preserve source-grounded simulation behavior, data ownership, dispatch
  dependencies, synchronization and publication when porting TressFX.
  Deliberate departures require reasons and correctness comparisons.
- **Primary product gates:** hair dynamics/rendering must match or
  exceed the reference's visual quality, preferably materially improve it,
  while delivering lower full-frame cost at matched-or-better quality on
  real consumer GPUs. Prioritize this over engineering novelty and
  speculative robustness work.
- Favor high throughput and practical fidelity on consumer GPUs. CI passing
  does not prove hardware performance, and reductions in theoretical
  thread counts or buffer bytes are **not** measured speedups.
- Handle ordinary resource errors predictably and release owned resources.
  **Do not build exhaustive driver-error recovery machinery or demand a
  test for every possible driver return code without a demonstrated need.**
  Apply proportionate review effort, focusing on source fidelity,
  frame-time costs, memory traffic, GPU orchestration and visual results.
- Keep authored data separate from GPU packing and backend-specific handles;
  do not add mandatory engine or physics dependencies.
- Do not mandate research-grade techniques or exotic accelerator features
  without demonstrated quality/performance improvements.

## Pre-merge self-review (required even without Codex)

Do not treat automated review or green CI as a substitute for examining the
current PR *head* and the changed code in context. Use the
[review protocol](docs/REVIEW_PROTOCOL.md), recording a short evidence table
for every substantial PR. Review architecture before individual defects,
then correctness/failure paths, cross-backend compatibility, performance
hot paths and installed-SDK boundaries. Verify the latest CI SHA and any
unresolved review comments; intentionally leave work draft if a
merge-blocking concern remains. Code review is a responsibility of the
maintainer/agent, not dependent on external reviewer credits.

## Required optimization-discovery and handoff practice

1. **Capture at discovery.** Whenever you notice avoidable copies, follower
   duplication, excess buffers, dispatches, barriers, synchronization,
   allocations, divergence, unneeded shading work, missed LOD or credible
   opportunities for a cheaper algorithm, add or update an `OPT-###` entry
   in `docs/OPTIMIZATION_BACKLOG.md` **before concluding the task**.
   Do this even when the fix should be postponed.
2. Each entry states the evidence/source path, suspected cost (clearly mark
   analytical estimates), prerequisite stage, next experiment, priority,
   status and testable acceptance criteria. Reference source commits,
   issues and measurements where possible.
3. Open/link a GitHub Issue for major actionable work. Blocked research
   candidates can remain backlog-only until their prerequisite is met;
   avoid premature or duplicate issues for speculative experiments.
4. Before each performance PR, read the backlog and update any relevant
   entry. Mark `done` only after a correctness check and meaningful
   physical-GPU measurements; otherwise use `implemented-awaiting-measurement`.
   Rejected ideas must retain their rationale, not disappear.
5. Each substantive PR description and session handoff states
   **Optimization backlog:** added / revised / addressed items, or
   `None discovered`. Keep unresolved work in the repo, never solely in chat.
6. At milestone boundaries, reconcile the backlog with `docs/ROADMAP.md`
   and `docs/SIMULATION_EXECUTION_TRACE.md`.

**Performance acceptance requires** full-frame GPU time and pass times,
peak memory, transfers, dispatch/barrier costs, visual/numerical fidelity,
scene and hardware/driver identification. Use actual hardware for throughput
claims. Avoid unnecessary checksums or verification bureaucracy.
