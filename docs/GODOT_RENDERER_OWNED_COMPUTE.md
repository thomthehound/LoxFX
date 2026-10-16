<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Godot 4.7.2 renderer-owned original-shader execution proof

**Status: experimental Forward+/Vulkan integration test; not the production
engine adapter, not complete LoxFX solver and not strand drawing.**

This test deliberately uses **Godot's global RenderingDevice on its render
thread**, rather than opening a LoxFX-owned Vulkan device or submitting
outside the engine scheduler. See
`adapters/godot/tests/renderer_owned_compute.gd` and the dedicated
software-Vulkan Xvfb CI step.

## SPIR-V entry-point compatibility

The initial Forward+ test successfully selected Mesa Vulkan and launched
Godot 4.7.2, but both compute pipelines failed creation (Vulkan error -13).
Source inspection isolated an entry-point ABI mismatch: Godot's
`RenderingDeviceDriverVulkan` sets the Vulkan shader-stage `pName` to
`"main"` during shader creation, while LoxFX's independent native
`DeviceContext` deliberately names the HLSL entry functions.

The optional Godot adapter build now compiles separate
`shaders/godot/loxfx_integration.spv` and
`shaders/godot/loxfx_follower_publish.spv` from the **same HLSL sources**,
using DXC `-fspv-entrypoint-name=main`. Native Vulkan SPIR-V binaries
continue to export their original names and are built unmodified.
The Godot runtime test consumes only the `main` variants; native Vulkan
CI continues testing the standard artifacts. No shader-physics fork or
byte-patching of SPIR-V is necessary.

The Godot-stage compile and numerical-readback gate must pass before we
call the renderer-owned experiment functional.

The GPU producer is LoxFX's **unmodified**
`IntegrationAndGlobalShapeConstraints.hlsl`, compiled by our existing
native shader build to `loxfx_integration.spv`. The GPU consumer is our
**unmodified** `UpdateFollowHairVertices.hlsl`, compiled to
`loxfx_follower_publish.spv`. Both shaders run from Godot's
`RenderingDevice` with Godot-created storage-buffer RIDs, reflected
descriptor sets, compute pipelines and push constants. A guide's pinned
root translates by 0.25 units; its free vertex integrates gravity.
The follower/tangent pass consumes the *same GPU current-position buffer*,
writing a tangent buffer without an intermediate CPU transfer or a manually
submitted `VkCommandBuffer`.

The test uses a minimal 16-guide/4-vertex fixture to prove the GPU ABI,
descriptor bindings, source shader execution and read-after-write graph
relationship, not to judge artistic hair quality. It performs a **single
asynchronous test-only readback after the compute stages** to verify a
translated pinned root, gravity integration and the dependent tangent.
There is no full-frame CPU geometry transfer inside its production-shaped
compute recording path.

## Ownership and synchronization

- The render-thread entrypoint uses
  `RenderingServer.call_on_render_thread`, checks
  `is_on_render_thread()`, and retrieves
  `RenderingServer.get_rendering_device()`. It rejects missing RD.
- All buffers, shaders, descriptor sets and compute pipelines are
  **Godot-created RIDs**. No raw `VkDevice`, `VkQueue`, `VkBuffer`,
  Vulkan descriptor or queue submission enters the proof. Godot's compute
  graph is responsible for recording, submission and dependency ordering.
- RIDs live until the asynchronous output probe reports GPU completion.
  Final teardown occurs on the render thread in reverse dependency order.
- Software-Vulkan CI verifies API integration and numerical semantics,
  **not GPU hardware performance**. A real Forward+ desktop visual pass
  and AMD hardware profiling remain required.

## What this does *not* solve

This test only demonstrates the **first and final** stages of the original
source-derived six-stage pipeline, with zero follower strands for the
initial ABI proof; it does not silently rename two stages a complete solver.
It does not replace the existing six-stage Vulkan/D3D12 execution, bypass
their state-management contract, provide real scalp attachment or collision,
or yield a Godot mesh/RID that can draw the full groom. Proper Godot-owned
buffer preparation for all seven solver resources, all six passes,
multi-groom history and actual GPU-resident geometry consumption are
subsequent integration gates. D3D12 parity remains independent and must
receive its own Godot renderer run rather than inheriting Vulkan claims.

## Next implementation

Extend the renderer-owned buffer bindings and the existing neutral frame
parameters to all six unmodified compute shaders, using the *same prepared
groom* and numerical fixture as the native backend. Then establish
GPU-resident drawing from Godot-owned positions/tangents without fabricating
RIDs from native handles, and validate complete frame ordering and lifetime
with multi-groom loads. Only after that do we replace the diagnostic node's
position-readback presentation path and claim render-frame GPU residency.
