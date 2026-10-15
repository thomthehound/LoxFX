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
- Preserve source-grounded simulation behavior, data ownership, dispatch
  dependencies, synchronization and publication when porting TressFX.
  Deliberate departures require reasons and correctness comparisons.
- Favor high throughput and practical fidelity on consumer GPUs. CI passing
  does not prove hardware performance, and reductions in theoretical
  thread counts or buffer bytes are **not** measured speedups.
- Keep authored data separate from GPU packing and backend-specific handles;
  do not add mandatory engine or physics dependencies.
- Do not mandate research-grade techniques or exotic accelerator features
  without demonstrated quality/performance improvements.

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
