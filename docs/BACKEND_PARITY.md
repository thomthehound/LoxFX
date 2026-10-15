<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# D3D12 / Vulkan backend parity contract

**Status: both Vulkan SPIR-V and native D3D12 SM6.6 DXIL now execute the
same first-stage HLSL** in separate GPU API runtimes. Linux software Vulkan
and Windows D3D12 WARP are functional tests, not physical GPU performance
measurements. Both runtimes remain internal, not installed SDK APIs.
Source code, timing and dispatch semantics must ultimately agree across
both APIs. Native full-six-stage execution is still incomplete.

Before building more Vulkan-only stages, apply the explicit
[device/context/recording contract](GPU_RUNTIME_ARCHITECTURE.md).
Native D3D12 integration must **not** duplicate the current test-harness
choice of a private graphics device and blocking submission per groom.

## Hard requirements

- **Both production backends are first class:** Direct3D 12 with Shader Model
  6.6 and Vulkan 1.4 (qualified on actual feature/driver support).
- Shared source HLSL and uniform simulation/asset semantics; do not fork the
  physical solver per backend just to accommodate a descriptor API.
- Native D3D12 and Vulkan command/resource schedulers may differ (heap
  allocators, root signatures, descriptors, Vulkan sets, fences/timelines)
  while preserving **the same data dependencies and published hair state**.
- Runtime backends must be usable independently, without mandatory third
  party engine or specific-GPU acceleration features. Backend choice and
  capabilities are established at initialization, not queried each dispatch.
  **Descriptor/dispatch limits do not imply memory allocation capacity**:
  backends must validate and attempt required allocations on a suitable
  device, and either retry other compatible adapters or report the failure
  through explicit externally selected device ownership.
- Keep shader variants to justified device features; do not conflate
  subgroup/wave primitives with hardware matrix accelerators.
- Require independent correctness checks and *hardware* performance
  measurements for each backend, with the same fixtures, quality criteria,
  guide/follower count, time step and collision workload.

## Shared first-stage shader contract

| Concept | Vulkan (SPIR-V) | D3D12 (DXIL) |
| --- | --- | --- |
| HLSL | `shaders/IntegrationAndGlobalShapeConstraints.hlsl` | **Same file**, SM6.6 |
| Parameters | 48-byte push constants | `b0, space0` mapped to 12 root DWORD constants (no CBV buffer) |
| Position buffer | set 0, binding 0, storage read/write | `u0, space0` root UAV descriptor |
| Previous position buffer | set 0, binding 1, storage read/write | `u1, space0` root UAV descriptor |
| Rest position buffer | set 0, binding 2, storage read-only | `t2, space0` root SRV descriptor |
| Workgroups | 64 threads | 64 threads |
| Work scheduling | Vulkan compute commands | D3D12 compute command lists |

PR #15 retains the same 48-byte constants but interprets the last
three floats as host-driven translation; both backends add a root-history
UAV at shader `u3` / Vulkan set 0 binding 3. This stores two entries
per guide and is uploaded only at creation and updated in the integration
shader. See [root motion stage](ROOT_MOTION_STAGE.md).

On both APIs: guide-only indexing, source-derived group counts, pinned roots,
gravity, damping and rest-shape relaxation must produce equivalent results.
Cross-API layouts are **contracts**, not native descriptor handles in the
authored `GroomAsset` or `PreparedGroom`.

## Rollout and acceptance gates

1. **Shader parity (done):** Windows CI compiles the canonical stage into
   Vulkan SPIR-V and D3D12 SM6.6 DXIL.
2. **Native D3D12 first-stage execution (PR #14):** Windows WARP executes
   actual D3D12 compute with persistent groom resources, root UAV/SRV
   descriptors, root constants, compute/direct command lists, UAV barriers,
   fences and timestamps. Two independent grooms reproduce the Vulkan
   fixture's numerical behavior, including externally submitted work.
   **The native D3D12 runtime remains a non-exported experimental harness.**
   WARP is not a physical GPU performance baseline.
3. **Six-stage parity:** Port each subsequent solver stage to shared HLSL
   first; update Vulkan/D3D12 execution contracts and tests together.
   Audit full source task/queue/dataflow ownership before making changes.
4. **Hardware performance:** For comparable device generations, report
   full-frame GPU time, per-stage timings, copies, barriers, peak memory,
   queue scheduling cost, and qualitative/numerical hair correctness.
   Choose the better viable approach per backend without sacrificing
   the other platform.

Track allocation/dispatch/memory performance opportunities in
[OPTIMIZATION_BACKLOG.md](OPTIMIZATION_BACKLOG.md). The issue for the
complete simulator is [#6](https://github.com/thomthehound/LoxFX/issues/6).
