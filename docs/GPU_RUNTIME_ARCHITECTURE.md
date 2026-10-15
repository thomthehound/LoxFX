<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# GPU runtime architecture: ownership, scheduling, and backend parity

**Decision record (15 October 2026).** PR [#11](https://github.com/thomthehound/LoxFX/pull/11)
proved that one source-derived hair integration shader can execute via Vulkan
and compile from the same HLSL to DXIL/SM6.6. It also accumulated repeated
creation/lifetime/synchronization review findings because the current
`IntegrationRuntime::Impl` owns an entire graphics stack **per groom**.
**That runtime is an exploratory driver, not a shippable architecture.**
Do not merge it as a public SDK runtime without correcting these boundaries.
Tracked as [issue #13](https://github.com/thomthehound/LoxFX/issues/13).

## Current remediation status

**Implemented in the draft smoke harness:** the old device-owning
`IntegrationRuntime` code and header were deleted. An explicit
standalone/borrowed `DeviceContext` now shares a single logical Vulkan
device and pipeline across independent `GroomState` objects. A
`record_integration(VkCommandBuffer, ...)` method records GPU work
without submission/wait; diagnostic submission and readback are separate.
Initial uploads use temporary staging, and Vulkan creation handles are
committed only after successful calls. The Linux fixture exercises two
grooms on one device and an externally borrowed logical device.

**Since this decision:** the resource-ownership structure remains in place,
and [PR #14](https://github.com/thomthehound/LoxFX/pull/14) adds a
native executing D3D12 first-stage smoke test with independently recorded
host command lists (SM6.6/WARP). **Still unproven:** production frames in
flight, host-renderer publication, full six-stage source/visual equivalence,
or physical-hardware throughput. This historical audit documents the old
design's problems rather than describing the current runtime.
The findings below describe the **historical implementation that was
replaced**, not the code now compiled by the smoke target.

## Findings: this is a boundary problem, not a missing sequence of checks

- **[P0, historical/fixed in smoke harness] Per-groom graphics device and pipeline.** Every
  `IntegrationRuntime(PreparedGroom, shader_path)` creates its own
  `VkInstance`, adapter/device, queue, pipeline, descriptor pool, command
  pool and fence. For N grooms this can create N devices and N copies of
  invariant shader pipeline objects. In an engine, it can silently put hair
  on a different adapter from rendering. The code's automatic multi-GPU
  fallback is consequently at the **wrong layer**.
- **[P0, historical/fixed in smoke harness] Blocking, self-submitting simulator.** `advance()` records a
  command buffer, submits, waits for completion, then reads timestamp
  results. A shipping simulation pass must instead **record** into a
  host-controlled queue/command context. Synchronization at dependency
  edges is required; a forced CPU fence every advance is not.
- **[P0, replacement requires fault injection] Exception-unsafe Vulkan output handles.** The latest Codex P2
  observes `vkCreateBuffer(..., &result.buffer)` and
  `vkCreateDevice(..., &device)` writing into owned handles before
  checking success. Vulkan output values are not valid on failure; cleanup
  must not destroy them. Similar creation paths require one coherent
  RAII policy, not independent patches to each `catch`.
- **[P1, historical/fixed in smoke harness] Resource allocation is tied to adapter search.** Four dedicated
  `VkDeviceMemory` allocations per groom and a persistent staging buffer
  make the newly written candidate-device retry logic grow without
  solving resource pooling, external allocator ownership or device-loss
  behavior. Heap-size and allocation-limit checks are only eligibility
  filters, not reservation guarantees.
- **[P1] Public lifetime contract is premature.** Exporting
  `LoxFX::Vulkan` and installing `IntegrationRuntime` currently
  advertises a per-groom device-owning, blocking API. That is an
  architectural commitment the project does **not** want to make.
- **[P1, stage-one mitigated in PR #14] Cross-backend execution was absent.**
  DXC compiles DXIL and SPIR-V from shared HLSL. Native D3D12 stage-one
  resource binding, commands, and timestamps now execute under Windows
  WARP. Whole-simulation and hardware-performance parity remain incomplete.
- **[P1] Reference fidelity is still limited.** The shader is a
  deliberate static-root stage-one approximation; it lacks source skinning,
  previous-previous velocity history, constraints, collision and follower
  publication. Do not describe it as a complete port.
- **[P2] GPU allocation accounting is not heap residency.** Reporting
  actual four-buffer `vkAllocateMemory` sizes improved truthfulness,
  but driver memory budgets, shared heap allocations and multi-groom
  staging lifetime need a coherent context-level metric.

## Chosen ownership model — minimal, not a generic GPU framework

| Layer | Owns | Explicitly does **not** own |
| --- | --- | --- |
| Neutral SDK core | Authored and prepared groom intent, work-plan and stage/resource-use semantics, shared HLSL | Device selection, API-specific handles, submission |
| Backend device context | A **selected** Vulkan or D3D12 device, queue capabilities, per-device pipelines, allocator/staging access, device/queue lifetime contract | An individual groom, shader physics |
| Groom GPU instance | Per-groom dynamic buffers, immutable rest bindings, initialization/reset state, groom descriptor bindings | Physical adapter, logical graphics device, queue submission policy |
| Backend stage recorder | Bind prepared stage resources, record source-derived passes, barriers and publication transitions into supplied command context | Creating adapters, deciding when CPU blocks |
| Host scheduler / example | Command lists/buffers, submission, interop, frame completion, timestamps/readback on request | Reimplementing strand physics |

Two device modes must be supported without duplicating the simulator:

1. **Host-provided device/context:** the application supplies an already
   chosen device/queue and allocator or explicit allocation callbacks. LoxFX
   validates requirements, reports capacity errors against *that* device,
   and never switches GPU behind the host's back. Ownership of externally
   provided handles remains with the host.
2. **Standalone test/example context:** a small explicit factory creates
   one device and queue, then the **same** per-groom objects and pass recorder
   run against it. Test harness submission may wait on a fence for
   deterministic assertions. Standalone device creation is not part of a
   hair object constructor.

Do not invent a Vulkan/D3D12 virtual-object clone hierarchy, a generic
render graph, a shader compiler pipeline, or a bespoke global GPU allocator
for this one kernel. Keep **native, backend-local** implementations
responsible for command and resource details. Share only neutral stage
semantics, layouts and validation. Reuse host allocators where available;
build an SDK-owned suballocator only with evidence from a multi-groom case.

## Resource and failure rules

- Every owned Vulkan/D3D12 resource uses a local lifetime guard or RAII
  wrapper. Creation writes to a **temporary** initialized handle; only
  `SUCCESS` transfers it to the owner. A partially initialized groom can
  be discarded without scanning invalid handles.
- Device admission checks must be appropriate for the actual chosen
  context: descriptors, dispatch topology, shader workgroup requirements,
  buffer allocation requirements and compatible memory types. No
  promise that nominal heap capacity guarantees allocation success.
- A host-provided device that cannot allocate a groom returns a clear
  resource error; it does **not** implicitly migrate to another adapter.
- Device-scoped pipelines and shareable stage layouts are compiled/cached
  **once per context**, not once per groom.
- Immutable guide/rest data and dynamic history are separately owned.
  Avoid committing to redundant full-follower storage before auditing
  actual readers (OPT-001/002).
- Upload staging belongs to a device/transfer context or bounded upload
  operation, and is reclaimable **only after its completion fence**.
  Persistent per-groom staging is a provisional test shortcut (OPT-011).
- A stage records its hazard requirements; the caller/host owns ordering
  between recorded passes and external renderer work. Test both initial
  transfer->shader and simulation->renderer/publication transitions.
  Never drop barriers to optimize without correctness evidence.
- Per-stage timing probes live at the submission/diagnostic layer and
  report the exact batch/scene measured. Waiting for results is a
  test/telemetry choice, not an unconditional simulation operation.

## Cross-backend contract

**Vulkan 1.4 and native D3D12/SM6.6 are equal shipping targets.** Maintain
one canonical HLSL physics implementation and test identical fixture
parameters and numerical outcomes on both backends. Shader bindings,
memory models, root signatures, queue/fence primitives and allocation
strategies remain independently correct. D3D12 DXIL compilation is an
important gate but not proof of D3D12 runtime parity. PR #14 subsequently
demonstrated actual stage-one DXIL execution on WARP; this still does not
establish full six-stage or consumer GPU performance parity
([issue #12](https://github.com/thomthehound/LoxFX/issues/12)).

## Keep, discard, and sequence

**Keep:** authored-groom importer, preparation and work-plan tests; the
single-source integration HLSL and provenance; dual-compiler Windows
checks; Mesa Vulkan numerical fixture; actual source pipeline trace;
optimization backlog.

**Replace:** the publicly exported, device-owning, blocking
`IntegrationRuntime`; auto-switching adapters from a groom constructor;
ad hoc failed-resource cleanup; per-groom invariant pipelines and
unconditionally persistent upload staging.

**Sequence:**
1. **Implemented in draft PR:** retain shader/compute smoke tests while
   excluding the prototype backend library and headers from installed
   SDK exports. Do not merge a permanent per-groom runtime API.
2. **Implemented in the internal experimental runner:** an explicit
   standalone/borrowed backend device context, separate per-groom state,
   failure-safe owned Vulkan output handles, and *two grooms on one device*.
   Still needed: fault-injected resource-creation failures, external-device
   integration tests, concurrency/lifetime checks and production scheduler.
3. Separate stage command **recording** from queue **submission**.
   Use host-controlled dispatch and diagnostic readback, retaining
   source-derived synchronization. Test initial upload, two grooms,
   several frames, and buffer lifetime.
4. **Implemented in PR #14 internal smoke:** native D3D12 first-stage
   execution with equivalent numerical fixtures and the same HLSL. Qualify
   the full six-stage runtime and actual physical GPU behavior separately.
5. Continue stages 2–6 only after the source host DAG/resource map is
   satisfied and real-hardware GPU timing infrastructure exists.

## Merge gate

Do not declare the PR ready solely for green CI or disappearance of
Codex findings. It needs the correctly scoped ownership/execution
contract, two-groom shared-device test, exception-safe partial
initialization, host scheduling acceptance and explicit truthful
performance/limitations. Stage one remains just stage one.

**Optimization tracking:** execution/submission architecture is related
to OPT-003; per-device allocations and staging to OPT-011;
guide/follower memory to OPT-001/002; actual throughput to OPT-009.
No item may be marked `done` based on this architecture decision alone.
