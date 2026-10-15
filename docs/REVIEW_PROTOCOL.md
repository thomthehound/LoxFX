<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Independent PR self-review protocol

**Purpose:** prevent the next architecture mistake from being hidden behind
incremental P2 fixes or passing smoke tests. A code-review bot can supplement
this review, but is neither required nor sufficient.

Keep this review **proportional to the change**. No ritual checklists, extra
hashes, or fake hardware benchmarks. A concrete question and evidence matter
more than the number of checks performed.

## Review order, before declaring a substantial PR mergeable

1. **Architecture and ownership first.** Read the entire diff and its callers.
   Identify who owns each device/allocator/buffer/queue, and who records,
   submits, synchronizes, and publishes the results. For every resource,
   identify construction, normal destruction and failure destruction.
   Reject a wrongly placed responsibility even if each local method passes.
2. **Negative-case review.** Identify invalid public inputs, mutable asset
   metadata, integer overflow, GPU device limits, failed resource creation,
   allocation failure, partial initialization, context/groom destruction
   ordering, and cross-frame GPU-in-flight hazards. Exercise representative
   failures to prove ordinary cleanup; avoid exhaustive driver-return-code
   recovery or speculative fault-injection without an actual product need.
3. **Parity and API boundary.** Check the same shader and neutral physics
   contracts on Vulkan and D3D12. A compiler output is not runtime parity.
   Confirm experimental code is not inadvertently exported or installed.
   Confirm no unrelated project coupling has entered the repository.
4. **Performance consequences.** Check hot-path allocations, CPU readback,
   queue waits, data copies, descriptor/pipeline creation, staging lifetime,
   workgroup over-dispatch and redundant follower storage. Capture deferred
   opportunities in the canonical optimization backlog, without claiming
   analytical projections are GPU speedups.
5. **Verification versus claims.** Inspect final-head Windows/Linux tests,
   sanitizers, software Vulkan validation where available, actual artifact
   packaging and code that is not covered by CI. Distinguish software
   execution, device-specific hardware profiling and visual source parity.
   Summarize uncovered risks as blockers or linked follow-up items.

For every substantive PR, report: **findings / corrected items /
remaining gates / exact head and CI result / optimization backlog
changes**. Rerun checks after last meaningful code change. If a review
uncovers repeated local remedies for the same responsibility, stop and
reconsider the owning abstraction rather than adding another guard.

## Applied review — PR #11 (15 October 2026)

**Scope checked:** public core/import/preparation and work-plan interfaces;
experimental Vulkan context, buffers, recording/submission, temporary staging,
shader compilation, CMake installed targets and D3D12 parity contract.

| Boundary | Evidence and conclusion |
| --- | --- |
| Core asset ownership | `GroomAsset` remains authored-only. No Vulkan/D3D12 instance or queue is created by the importer, planner or preparation code |
| Public mutable prepared state | Identified a duplicated, incomplete validation path in Vulkan. Centralized `validate_prepared_groom` in neutral core; rejects malformed counts, stages, arrays and nonfinite GPU inputs once before resource preparation |
| Device/groom ownership | Deleted prior `IntegrationRuntime`. `DeviceContext` holds a device/pipeline, `GroomState` holds per-groom buffers. Borrowed device lifetime remains a host obligation |
| Frame submission | `record_integration` records into host-provided `VkCommandBuffer`; Linux CI exercises independent host pool, submission and fence. No implicit queue submit in stage recording |
| Failure cleanup | Resource creation uses temporary success-committed handles and RAII. Added groom fault-injection checkpoints and context-construction fault tests. These simulate failures *before calls*; they do not replace actual driver-failure testing |
| GPU memory | Groom owns dedicated persistent buffers; host staging lasts only for upload/readback. OPT-001/002 and OPT-011 remain open, including guide-only state and pooling |
| Shader parity | Same stage HLSL compiles to Vulkan SPIR-V and D3D12 SM6.6 DXIL; **native D3D12 execution remains absent** (issue #12) |
| Hardware and fidelity | Mesa Vulkan functional tests are not real GPU throughput, multi-frame renderer interop or six-stage TressFX visual equivalence |

**Remaining limitations:** the experimental API must stay unexported. GPU
work still requires caller-enforced in-flight resource lifetime and correct
synchronization with rendering. Multi-queue, device-lost and actual-driver
allocation failures are not yet demonstrated. No physical GPU performance
measurements exist. Native D3D12 stage one and full six-stage execution
remain milestones, not completed claims.

**Backlog:** OPT-001, OPT-002, OPT-003, OPT-009 and OPT-011 remain open;
nothing marked `done` without measurements.

This record is a review of the **draft internal smoke harness**, not a
declaration that a production simulation runtime or D3D12 backend is ready.

## Applied review — PR #14 (native D3D12 integration stage)

**Architecture:** D3D12 is a distinct native backend using the same
neutral `PreparedGroom`, `IntegrationParameters`, and HLSL physics as
Vulkan. One context owns the selected device, root signature and pipeline;
groom states share its lifetime and own only committed buffers. Optional
borrowed devices/queues use COM references. No per-groom device creation
or CPU wait inside `record_integration`.

**Correctness:** The Windows WARP test executes actual DXIL compute and
checks the same static-root Verlet, pinned root, independent follower
state, global-rest-shape and cross-submission numerical fixtures as Vulkan.
Host command list/queue/fence ownership and borrowed **COMPUTE and DIRECT**
queues have separate execution checks.

**Performance discipline:** The integration stage uses root SRV/UAV
descriptors and 48-byte root constants without a descriptor heap,
per-step upload, or per-step CPU readback. These are design properties,
**not measured speedups**. Device feature checks occur once at context
creation, not per-step. Committed buffer sizes are reported with
`GetResourceAllocationInfo`. OPT-001/002/003/009/011 remain open.

**Remaining gates:** physical D3D12 and Vulkan performance, full six-stage
and moving-root fidelity, frame-to-renderer publication, plus matched
reference visual-quality and whole-scene throughput. WARP execution
validates functionality only. The new implementation stays non-exported
until product integration contracts mature.

## Applied independent review — PR #15 (moving roots/history, 15 October 2026)

**Architecture and source audit:** Compared the shared first-stage HLSL
against TressFX 4.1 `IntegrationAndGlobalShapeConstraints` and
`UpdateFinalVertexPositions`, and traced `CalculateStrandLevelData`
consumers. That next stage reads *current, previous and previous-previous*
positions of **only the first two guide vertices**. The new compact
`2 * padded_guides` history buffer preserves the source's order
(`old previous` stored before updating `previous`). The current
translation primitive is intentionally not claimed as full bone
skinning or complete strand-motion support.

**Cross-backend bindings and memory:** Vulkan declares the fourth storage
buffer at set 0/binding 3, updates the standalone and borrowed device
descriptor-limit admission, and uploads/reads the compact buffer with
existing diagnostic synchronization. D3D12 declares a matching root UAV
at `u3`, uses the shared 48-byte root constants, initializes and
transitions the committed history buffer, and records UAV barriers between
dependent steps. Neither path adds CPU readback or resource setup during
the stage's per-frame record operation.

**Regression from merged PR #14:** Borrowed D3D12 queues now query their
owning device and compare canonical COM `IUnknown` identities with the
host's device before retaining the handles. Tests verify ordinary borrowed
compute/direct queues. An attempted cross-device negative test is **only
executed when two distinct device identities exist**: WARP often returns
the same cached device, so CI must never falsely claim the mismatch test
ran on a single-device runner.

**Additional review coverage:** Tests already exercised dynamic vertex-1
motion history and cross-groom isolation. This review added a paired
Vulkan/D3D12 fixture with **both proximal guide vertices pinned**,
moving together under three successive attachment translations; the
previous-previous history then retains both correctly translated
attachments. This supplies a rigid-motion baseline for upcoming strand
rotation/translation work.

**Still deferred, not defects to disguise:** per-strand skinning,
rotation, strand-data output and VSP, reset/teleport behavior,
rendering/publication, frames in flight, physical-GPU performance and
reference-quality imagery. OPT-001/002/003/009/011 remain tracked.
No claimed throughput or visual improvement is inferred from software CI.

## PR #16 — independent review checkpoints

Trace quaternion orientation/translation/acceleration against reference
stage two; check numeric degenerate-root handling, exact three-float4
layout and per-guide indexing. Require **actual** Vulkan and D3D12
execution with matching data and threshold fixtures, not shader compilation
alone. Verify GPU barrier visibility of integration results, no CPU
submission from stage recorder and no per-follower output allocation.
Skinning remains identity until full skeletal attachment is present.
No driver-recovery scope creep or unmeasured speedup claims.
