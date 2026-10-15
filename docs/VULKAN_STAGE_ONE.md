<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Experimental native Vulkan integration-stage proof

PR [#11](https://github.com/thomthehound/LoxFX/pull/11) is draft. This
is an **internal test harness**, not a complete simulator or installed SDK API.
See [ownership design](GPU_RUNTIME_ARCHITECTURE.md) and
[issue #13](https://github.com/thomthehound/LoxFX/issues/13).

## Demonstrated

- Source-derived stage-one HLSL compiles through DXC to Vulkan SPIR-V and
  D3D12 SM6.6 DXIL. This document originally covered the Vulkan-only
  milestone; [PR #14](https://github.com/thomthehound/LoxFX/pull/14)
  subsequently executes the same HLSL natively on D3D12 WARP.
- Explicit standalone or borrowed-host `DeviceContext`: one logical
  Vulkan device and one compute pipeline shared across grooms. No per-groom
  adapter selection, device creation or implicit migration.
- Independent `GroomState` objects own GPU rest/current/previous position
  buffers and descriptors, with RAII lifetimes tied to their context.
- `record_integration(VkCommandBuffer, GroomState, params, steps)` records
  the work and its inter-step dependencies, **without submitting or
  waiting**. A diagnostic harness explicitly submits when requested.
- CI executes two independently animated grooms on one device, verifies
  pinned roots, Verlet evolution, independent history, rest-shape recovery,
  and a separate borrowed-device context.
- Temporary staging is freed after the one-time upload or explicit
  diagnostic readback completes. No CPU readback is performed by
  `record_integration`. Validation occurs when preparing resources, not
  on the frame dispatch hot path.
- Failure-sensitive creation calls use temporary handles that become owned
  only after success. Injected allocation/creation failures have not
  yet been exercised; functional CI alone does not establish exhaustive
  failure safety.

The old `IntegrationRuntime` header/source were deleted. The new internal
harness is `include/loxfx/vulkan/smoke_context.h` and
`src/vulkan/smoke_context.cpp`. Only neutral `LoxFX::Core` is installed.

## Boundaries

The integration shader remains a simplified static-root, first-of-six
stage. It does **not** implement animated root transforms, velocity-shock
propagation, constraints, collision, wind, follower publication or rendering.
D3D12 first-stage execution is implemented in
[PR #14](https://github.com/thomthehound/LoxFX/pull/14);
[issue #12](https://github.com/thomthehound/LoxFX/issues/12)
retains native-hardware performance qualification.
The Vulkan test uses an API 1.2 subset for software-driver CI;
production target is Vulkan 1.4.

The internal test helpers `submit_diagnostic_and_wait` and
`diagnostic_read_positions` are explicitly **not** a production scheduler.
Host applications must own queue submission, renderer interop,
frames-in-flight synchronization and lifetime of externally supplied devices.
Physical GPU per-pass/full-scene timing and visual quality remain unmeasured.

Tracked optimization opportunities:
[OPT-001/002](OPTIMIZATION_BACKLOG.md) for guide/follower static/history
buffers, OPT-003 for real frame orchestration, OPT-009 for measured dispatch
benefits and OPT-011 for device-allocation/suballocation strategies.
No optimization has been declared complete on the strength of software CI.

## Next acceptance gates

Test externally allocated command buffers and creation failure injection,
establish host-safe frame scheduling and scene/renderer resource transitions,
continue source-derived six-stage simulation on BOTH native Vulkan and
D3D12, after the stage-one D3D12 proof in PR #14. Require consumer-hardware performance tests.
