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
   ordering, and cross-frame GPU-in-flight hazards. Exercise realistic faults,
   then prove that a subsequent valid operation still works.
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
