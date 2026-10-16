<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Host-owned groom upload — native Vulkan and D3D12

**Scope:** An internal, provisional native GPU runtime milestone. LoxFX's
existing core import/preparation and all six physics stages are unchanged.
There is no Godot dependency, public installed API, or performance claim.

Previously, Vulkan `DeviceContext::prepare` and D3D12
`DeviceContext::prepare_diagnostic` allocated the groom and staging resources,
recorded copies into the backend's private diagnostic command buffer/list,
submitted them and blocked on a completion fence. A game renderer cannot
control or batch that hidden submission, even though PR #26 made complete
simulation frame *recording* host-owned.

Both backends now offer the same three-part internal native workflow:

1. `auto pending = context.prepare_upload(prepared_groom);` validates the
   prepared topology, allocates GPU buffers/descriptors and packs mapped CPU
   staging memory. **No native GPU command submission or fence wait.** The
   move-only `PendingGroomUpload` owns *both* staging and destination buffers.
2. `context.record_initial_upload(host_command, pending);` appends the
   existing destination copies and initial transfer→compute barriers/state
   transitions into a **caller-created and caller-controlled** Vulkan
   `VkCommandBuffer` or D3D12 `ID3D12GraphicsCommandList`. Recording twice
   or recording a transaction into a different `DeviceContext` is rejected.
   The caller may record several independent uploads in one command batch.
3. After its **own** queue submission and completion fence has signaled, the
   host calls `auto gpu_groom =
   std::move(pending).finish_after_completion();`.
   This does not query, submit or wait on GPU work: it transfers the finished
   groom and releases upload staging **only at this explicit completion point**.

Do **not** finalize or destroy a pending upload while GPU commands reference
its memory. The host owns the fence/completion guarantee; the SDK cannot
infer completion from the recording or from a native GPU handle alone. A
partially recorded command buffer must not be submitted if an error occurs.
As with ordinary submitted GPU resources, the host must defer destruction
until GPU work is complete. This primitive is deliberately explicit, not a
fictional fully automatic asynchronous lifetime manager.

The old `prepare()` and `prepare_diagnostic()` remain for standalone
functional tests, built on these same allocation and recording primitives
followed by an **explicit diagnostic-only** submit/wait/finalization.
Existing Godot diagnostic tests retain the blocking path. The new path is
for a future renderer-owned scheduler and enables multi-groom upload
recording without hidden SDK submissions.

Native Vulkan software and D3D12 WARP CI upload two independently prepared
grooms in one **host-owned** command buffer/list, submit with their own
queue/fence, verify that diagnostic submission counters do not change,
finalize only after completion and check resulting rest positions. Previous
six-stage native execution and compatibility tests remain active.

## Still outstanding

- Godot Forward+/D3D12 renderer queue and device ownership negotiation,
  render-thread scheduling, and safe buffer exposure to a GPU-only draw path;
  no native Godot RID or raw-handle adapter is fabricated here.
- Cross-frame asynchronous lifetime/fence integration in a real renderer,
  including teardown with frames in flight, CPU/GPU overlap, multi-queue
  barriers, error paths and resource reuse under load.
- Hardware-level performance measurement and comparison to the separate
  TressFX reference at matched visual quality.
- A separately reviewed production-facing API: the types here remain in
  **uninstalled** experimental backend headers.

See [native frame recording](NATIVE_FRAME_RECORDING.md),
[GPU runtime ownership](GPU_RUNTIME_ARCHITECTURE.md) and
[Godot renderer interoperability](GODOT_RENDERER_INTEROP.md).
