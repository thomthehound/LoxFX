<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Godot renderer-native handoff: source audit and acceptance boundary

**Status (15 October 2026): renderer visibility implemented; native device
borrowing, production command scheduling and GPU-resident consumption NOT
implemented.** This is a scoped engineering decision record, not a proposal
to create a new engine or GPU abstraction. Its purpose is to avoid mistaking
Godot's exposed native handles for a valid engine interop contract.

## What is already working

LoxFX's native Vulkan and D3D12 backends record the same six-stage simulation
and publish GPU-resident per-groom positions and tangents. Their internal
`GroomState::positions_buffer()` and `tangents_buffer()` expose borrowed
native handles, with GPU resource ownership retained by LoxFX. The optional
Godot GDExtension uses a separate SDK-owned diagnostic device, synchronously
reads back results, and renders diagnostic Godot line segments. Compatible
grooms share a device/context, but each has independent simulation state.
This is valid visual-validation infrastructure, **not** the shipping frame path.

## Godot interfaces, verified from 4.7.2 source

- `RenderingServer::get_rendering_device()` returns the engine's device where
  supported, or null where the active rendering method has no RenderingDevice.
  `RenderingServer::get_video_adapter_name()`,
  `get_video_adapter_vendor()` and `get_video_adapter_api_version()`
  report adapter metadata.
  [Source](https://github.com/godotengine/godot/blob/4.7.2-stable/servers/rendering/rendering_server.h).
- `RenderingDevice::get_driver_resource()` can expose native logical and
  physical device, queue, queue family, and resource identifiers.
  These are *borrowed native identifiers*, **not** licenses to submit commands
  on Godot's queue from an arbitrary scene callback, nor ownership of RIDs.
  [Source](https://github.com/godotengine/godot/blob/4.7.2-stable/servers/rendering/rendering_device.h).
- Godot offers `RenderingServer::call_on_render_thread()` and
  `CompositorEffect::_render_callback()` for execution inside the rendering
  lifecycle. These are possible integration points but do not automatically
  supply a native command buffer or synchronization barriers for arbitrary
  external work.
  [Render-thread API](https://github.com/godotengine/godot/blob/4.7.2-stable/servers/rendering/rendering_server.h);
  [Compositor documentation](https://docs.godotengine.org/en/stable/tutorials/rendering/compositor.html).
- The engine supports Vulkan and D3D12 backends. A Godot native device cannot
  be assumed to be the same API/GPU as the separate SDK-owned diagnostic
  device. Never compare handle bit patterns across different devices/APIs.

## Implemented conservative runtime observation

`LoxFXGroom3D.get_renderer_interop_status() -> Dictionary` reports Godot
renderer-device presence and display-adapter metadata. The report **always**
keeps these false until separately implemented and verified:
`native_handles_borrowed`, `device_shared_with_renderer`,
`native_queue_submission_permitted`, `synchronization_verified`, and
`gpu_resident_publication`. It also distinguishes SDK-owned diagnostic
compute activity from Godot renderer ownership.

The query intentionally does not call `RenderingDevice::get_driver_resource`
from the scene thread. It is safe as a UI/capture preflight and in a headless
smoke: no GPU handle crosses to Godot scripts.

## Next executable boundary: renderer-owned prototype, one platform first

1. In a render-thread callback, inspect the *actual* Godot graphics backend,
   device, queue family and queue. Reject unsupported methods explicitly.
   Negotiate capabilities and a clear borrow/lifetime contract with the SDK.
   Do not silently fall back to a different physical adapter.
2. Prove a **borrowed** backend context can prepare one groom and record
   source-ordered compute into a submission context owned by the renderer
   without allocating its own device. Establish queue legality, command-pool
   lifetime and feature requirements using the actual Godot backend source.
3. Expose published positions/tangents to a Godot **GPU-resident consumer**,
   either with a host-allocated shareable output buffer or a renderer-native
   draw path capable of reading the SDK-owned buffer on the same device.
   Do not fabricate a Godot `RID` from a raw VkBuffer/ID3D12Resource pointer.
4. Prove queue ownership, stage and graphics visibility, in-flight frame
   lifetime, resize/teardown, multi-groom use and D3D12/Vulkan feature parity.
   No forced per-frame GPU→CPU position readback in the qualifying path.
5. Compare diagnostic output to GPU-resident output and a standalone
   matched-workload baseline on actual consumer hardware, including shader
   execution, synchronization, data movement, drawing and full-frame cost.

A Godot headless/Compatibility run cannot prove this renderer-native path:
the existing headless smoke validates neutral API behavior and SDK-owned
compute only. A Forward+ / Vulkan and D3D12 hardware-backed visual test is
a separate acceptance gate.

## Explicit rejection

Do not borrow a native `VkDevice`/`VkQueue` or D3D12 queue and immediately
submit LoxFX commands from `_process`, `_physics_process`, or an unrelated
worker. Mere handle visibility cannot guarantee the engine's queue locks,
frame ordering, image/buffer resource state, descriptor lifetime, or visibility
between hair simulation and hair drawing. Likewise, do not call a CPU-mesh
upload path "GPU resident." The first real in-render-frame prototype should
be small, observable and source-backed before any public SDK commitment.
