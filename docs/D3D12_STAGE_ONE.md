<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Native D3D12 / SM6.6 integration-stage smoke

**State:** internal/native working proof in [PR #14](https://github.com/thomthehound/LoxFX/pull/14).
The same source-derived HLSL stage executes through Direct3D 12 with native
DXIL, not just shader compilation. **Only stage 1 of six is implemented.**
WARP is a correctness oracle, not a performance target.

## What executes

- A standalone hardware-or-explicit-WARP D3D12 device context, or a
  borrowed application-selected D3D12 device and command queue. One
  root signature and compute pipeline per context.
- Independent groom state: three persistent committed resources
  (current/previous/rest positions). No graphics device/pipeline
  creation per groom.
- Root UAV descriptors for `u0` and `u1`, root SRV `t2`, and **12 DWORD
  root constants** at `b0` (48-byte payload). The first-stage kernel
  requires no descriptor heap and no constant-buffer upload per step.
- Record-only simulation into an **externally supplied
  `ID3D12GraphicsCommandList`**; barriers between Verlet steps and
  explicit host-controlled submission. The library never invokes a
  CPU fence wait while recording stage work.
- Explicit test-harness upload and readback, timestamp queries, query
  resolve and wait at the requested diagnostic submission boundary.
  No staging or readback occurs in `record_integration`.
- Windows CI **executes WARP**, checking two independently animated grooms,
  pinned roots, velocity history across submissions, rest-shape recovery,
  compute-queue and direct-queue borrowed contexts, and a host-owned
  command list/fence. Results use the same numerical fixtures as Vulkan.

## Build with Windows SDK, CMake 3.28+ and DXC

```powershell
cmake -S . -B build/d3d12 -A x64 -DLOXFX_BUILD_D3D12_SMOKE=ON -DBUILD_TESTING=ON
cmake --build build/d3d12 --config Release
ctest --test-dir build/d3d12 -C Release -R loxfx_d3d12_integration -V
```

The test chooses WARP explicitly for repeatability; application-selected
hardware devices are supported by the internal context. The D3D12 target,
headers and shader runtime are **not installed as public SDK APIs**.
`LoxFX::Core` remains graphics-independent.

## Architectural and performance boundaries

This is *not* a new physics pipeline. The simulation algorithm is shared
with Vulkan; D3D12 supplies its own device, resource, root signature,
barrier, command list, fence and timestamp implementation. Runtime
lifetime and queue ordering belong to the caller. The synchronous
`prepare_diagnostic` and diagnostic readback are harness operations,
not production per-frame behavior.

Committed resource allocation sizes use `GetResourceAllocationInfo`
and are not measured residency. WARP timestamps cannot establish
real GPU throughput. Important cost/quality targets remain:

- **Reference visual quality as an absolute floor**, preferably distinctly
  better hair movement, stability, collisions and style retention.
- **Lower full-frame time than reference at matched/better visible quality**,
  on physical consumer GPUs, counting copies, barriers, simulation and
  publication, not just individual shader time.
- No default reliance on specialty acceleration or vendor-specific SDKs.
- OPT-001/002 (guide-only vs follower storage), OPT-003 (dispatch and
  scheduling costs), OPT-009 (real GPU dispatch measurements) and
  OPT-011 (committed resource allocation and staging) remain open.

Next engineering target: restore source-faithful animated root and
strand-level data dependencies, then bring the complete six-stage
simulation **to both backends**, preserving host-owned command recording
and actual physical hardware speed/visual-quality acceptance.
