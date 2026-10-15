<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# D3D12 / Vulkan backend parity contract

**Status: Vulkan stage-one execution demonstrated; same HLSL compiled to
D3D12 SM6.6 DXIL but no native D3D12 executor yet.** Source code, timing
and dispatch semantics must ultimately agree across both APIs.

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
- Keep shader variants to justified device features; do not conflate
  subgroup/wave primitives with hardware matrix accelerators.
- Require independent correctness checks and *hardware* performance
  measurements for each backend, with the same fixtures, quality criteria,
  guide/follower count, time step and collision workload.

## Shared first-stage shader contract

| Concept | Vulkan (SPIR-V) | D3D12 (DXIL) |
| --- | --- | --- |
| HLSL | `shaders/IntegrationAndGlobalShapeConstraints.hlsl` | **Same file**, SM6.6 |
| Parameters | 48-byte push constants | `b0, space0` constant buffer |
| Position buffer | set 0, binding 0, storage read/write | `u0, space0` UAV |
| Previous position buffer | set 0, binding 1, storage read/write | `u1, space0` UAV |
| Rest position buffer | set 0, binding 2, storage read-only | `t2, space0` SRV |
| Workgroups | 64 threads | 64 threads |
| Work scheduling | Vulkan compute commands | D3D12 compute command lists |

On both APIs: guide-only indexing, source-derived group counts, pinned roots,
gravity, damping and rest-shape relaxation must produce equivalent results.
Cross-API layouts are **contracts**, not native descriptor handles in the
authored `GroomAsset` or `PreparedGroom`.

## Rollout and acceptance gates

1. **Shader parity (current PR):** Windows CI compiles the one canonical
   stage to both SPIR-V and DXIL, and executes SPIR-V via Mesa on Linux.
   This is compile-time compatibility, not a D3D12 execution proof.
2. **Native D3D12 first-stage execution (next):** Implement C++23 D3D12
   device/resource creation, upload, UAV/SRV/CBV binding, root signature,
   dispatch, UAV barriers, fence completion and GPU timestamps. Validate
   identical multi-step rooted guide motion with a native D3D12 test.
   Add `windows-latest` D3D12 execution CI if WARP supports the required
   feature set; otherwise document the capability and use physical GPU
   qualification. Do not incorrectly claim WARP performance equivalence.
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
