<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Host-owned full-frame recording (internal native backend milestone)

This milestone joins the already implemented six native hair stages into one
**record-only** operation. Both `vulkan::smoke::DeviceContext::record_frame`
and `d3d12::smoke::DeviceContext::record_frame` consume a
`NativeFrameParameters` and a groom's independent `GroomState`.

The source-ordered stages remain:
1. Integration and global shape
2. Strand-level motion
3. Velocity-shock propagation
4. Local shape
5. Length constraints and wind
6. Follower positions and tangent publication

**Single-clock contract:** `NativeFrameParameters::time_step` is the sole
frame time step. The backend-neutral `native_frame_stage_parameters()`
lowers it into both the integration and length/wind pass structures.
It is impossible to independently configure integration and wind time
steps through this composite API. Each standalone per-stage recorder keeps
its established parameter interface. Both native test suites exercise an
active wind at 30 Hz and compare the complete-frame result against explicit
stage recording with a common 30 Hz clock; core CI also checks the lowering.

The implementation delegates to the **existing** individually verified
native recorders; their resource bindings, shader mathematics and stage
barriers are not duplicated or modified. The command buffer/list belongs to
the caller. **Recording neither allocates a new device, nor closes or submits
commands, waits on a fence, reads GPU data, or manufactures renderer objects.**

Both native integration tests now prepare independent grooms (including
different follower populations), record two entire simulations into **one
host-created command buffer/list**, and submit that batch with a host-owned
fence. They assert no SDK diagnostic submission occurred while recording and
check distinct guide/follower output via diagnostic readback **after**
submission for numerical validation. The Godot diagnostic adapter uses the
same record-only operation before performing its explicitly non-production
submission and CPU transfer.

## Important ownership boundaries

- `DeviceContext` owns device-invariant pipelines and context resources;
  `GroomState` owns groom-specific GPU resources and retains the device's
  lifetime as required by the native backend.
- Native `record_frame` calls are ordered and may be interleaved between
  different grooms using the same compatible context. The host controls queue
  ordering, submission, frame fences, presentation and lifetime.
- Groom initialization now supports `prepare_upload()` resource/staging
  creation, `record_initial_upload()` into a caller-owned command buffer/list,
  and explicit pending-transaction completion **after the host GPU fence**.
  Legacy `prepare()`/`prepare_diagnostic()` remain blocking test helpers.
  See [host-owned groom upload](HOST_OWNED_GROOM_UPLOAD.md). Godot's actual
  render-thread device and queue ownership/synchronization is still an
  independent unfinished step.
- If input validation fails after commands have been appended, abandon and
  reset the host command recording rather than submitting a partial frame.
  The stage-specific validation and failure contracts remain unchanged.
- GPU-resident outputs are already exposed by borrowed Vulkan/D3D12 buffer
  handles, but the host must provide the correct cross-pass and
  compute-to-graphics barriers and a compatible GPU consumer. A raw buffer
  handle is **not** a Godot RID or permission to use a graphics queue.
- No physical-hardware speedup, frame latency, zero-readback Godot rendering,
  skeletal contact, or production API stability is claimed.

## Next acceptance gate

Separate groom preparation/upload from test-owned queue submission, then
exercise a borrowed renderer device and render-thread command owner.
Only after command lifecycle, GPU consumer visibility and lifetimes are
proved should the Godot adapter advertise renderer-resident strands.
Preserve native Vulkan/D3D12 parity; test the same moving multi-groom input
under the standalone baseline and any engine-hosted path.
