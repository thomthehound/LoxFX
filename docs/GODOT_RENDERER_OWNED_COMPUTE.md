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

### Additional compatibility blocker verified in executable CI

After compiling a separate Godot-targeted `main` entry point, the *follower*
compute pipeline creates successfully. The *integration* compute pipeline
still fails in the normal Godot 4.7.2 Forward+ renderer, reporting
`VkResult -13`. Vulkan validation identifies the real error as
`VUID-VkShaderModuleCreateInfo-pCode-08737`: a forward-referenced SPIR-V
ID (`%123`) has no definition, followed by Mesa's
`spirv_to_nir failed (VK_ERROR_UNKNOWN)`.

**The supplied shader passes `spirv-val --target-env vulkan1.2`.**
Native LoxFX Vulkan executes its original entry-point variant successfully.
Godot's Vulkan implementation calls `respv::Optimizer::run` on imported
SPIR-V when shader debugging is disabled. Running the *same* Godot renderer
with `--generate-spirv-debug-info`, which disables this intermediate
optimization, **passes the original LoxFX integration and tangent numerical
acceptance** with a global Godot-owned device and the expected producer/
consumer dependency. This is a source-backed engine-internal transformation
failure, not evidence of an invalid LoxFX shader or a D3D12 problem.

We also tested DXC `-O0`, SPIRV-Tools optimization and HLSL legalization,
a DXC-SPIRV → SPIRV-Cross GLSL → glslang round-trip, an optional
specialization constant, and a mathematically equivalent predicated
control-flow variant. None survived the normal Godot re-SPIRV transform;
all experiments that introduced source variations were **reverted**. No
specialization/branchless shader fork or silent debug flag was added.

The renderer-owned proof now lives in its own **fast CI workflow**,
`.github/workflows/godot-renderer-proof.yml`, separate from the expensive
GDExtension build. It has a strict normal-mode gate. If the normal run
fails, a debug-mode run is retained **only as a diagnostic control**;
it cannot turn the check green.

**Current status: [PR #28](https://github.com/thomthehound/LoxFX/pull/28)
remains draft and this proof is blocked under stock Godot 4.7.2.**
Tracked as [LoxFX issue #29](https://github.com/thomthehound/LoxFX/issues/29)
with a standalone reproducible CI job and upstream source references. Correct remediation requires fixing or safely bypassing
the re-SPIRV transformation in a supported Godot renderer integration
surface, not inventing a device, altering the reference hair dynamics,
requiring a global debug mode, or marking the test passed.

The Godot-stage compile and numerical-readback gate must pass in normal
mode before we call the renderer-owned experiment functional.

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
