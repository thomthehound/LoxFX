<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# LoxFX

LoxFX is an independent, cross-platform hair and fur SDK under development.
It is intended to provide reusable real-time strand simulation, with optional
rendering integrations, without requiring a particular game engine.

The project is architecturally independent of AMD TressFX. The original
TressFX 4.1 implementation remains available in the
[reference repository](https://github.com/thomthehound/TressFX) for
source-grounded comparison, benchmarking and selective adoption.

## Current status

The initial CPU-only core imports TressFX 4.x `.tfx` guide-strand geometry and
optional attributes without changing authored counts or adding generated
follow strands. A standalone inspection tool, source-traced simulation dispatch planning,
one-time prepared strand storage and regression tests are included.
**First-stage GPU integration now executes in experimental Vulkan and
D3D12 harnesses**, but the full simulation, rendering and engine
integration remain incomplete and are not yet exported as SDK APIs.

## Build

Requires CMake 3.28+ and a C++23 compiler and standard library on Windows or Linux. No graphics
SDK, GPU runtime, reference implementation or game engine is needed.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Inspect a TressFX 4.x groom with `loxfx_tfx_inspect path/to/hair.tfx`.
To inspect the projected GPU work footprint, pass an optional follower count:
`loxfx_tfx_inspect path/to/hair.tfx 3`. Add `--prepare` to prepare the
persistent strand arrays and report their size. Preparation computes
tangents, segment rest lengths and deterministic follower geometry once,
not per frame. Printed lane counts are analytic, not GPU timings.

Install and consume LoxFX as a standalone CMake package:

```sh
cmake --install build --config Release --prefix /path/to/loxfx-install
```

Downstream projects can use `find_package(LoxFX CONFIG REQUIRED)` and
`target_link_libraries(app PRIVATE LoxFX::Core)`. The package exports its
C++23 requirement. The public ABI is not yet frozen.

## Optional Vulkan integration milestone

**Architecture review:** the stage-one Vulkan module is a working
experimental compute harness, **not a shipping runtime API**. The current
per-groom device ownership has now been replaced with **one shared Vulkan
context, separate per-groom buffers and caller-recorded command batches**.
Native D3D12 stage-one execution is now being validated alongside Vulkan;
production scheduler, quality and physical-GPU profiling must still pass
before either becomes an installed SDK interface. See
[the GPU runtime architecture decision](docs/GPU_RUNTIME_ARCHITECTURE.md)
and [issue #13](https://github.com/thomthehound/LoxFX/issues/13).


A native Vulkan module can compile and execute the **first** TressFX-inspired
simulation stage on persistent GPU buffers. It tests static-root Verlet
integration with explicit command submission, GPU memory barriers, diagnostic
readback and device timestamps. **It is not yet a six-stage hair simulator**;
bone skinning, full constraint solving, collisions and follower publication
remain pending.

Build with the Vulkan SDK and an installed DXC executable:

```sh
cmake -S . -B build/gpu -DLOXFX_BUILD_VULKAN_SMOKE=ON -DBUILD_TESTING=ON
cmake --build build/gpu --config Release
ctest --test-dir build/gpu -C Release -R loxfx_vulkan_integration -V
```

The smoke harness is opt-in and not installed. The SDK still exports
only its neutral `LoxFX::Core` target. CI executes the stage through
Mesa software Vulkan for functional validation, **not**
representative GPU performance. Hardware qualification remains required.
See [stage-one execution notes](docs/VULKAN_STAGE_ONE.md).

## Direct3D 12 status

Direct3D 12 / Shader Model 6.6 and Vulkan 1.4 are **equal intended production
backends**. The first stage's simulation input semantics and HLSL algorithm
are shared. Windows CI compiles the same stage to D3D12 DXIL and Vulkan SPIR-V.
**Native D3D12 now executes that DXIL through an actual D3D12 compute
pipeline** in Windows WARP CI, with independent grooms, root descriptors,
root constants, host-recorded command lists, fence synchronization and
timestamps. This is first-stage functionality, not a complete simulator.

Build the internal native D3D12 stage-one test with the Windows SDK and DXC:

```powershell
cmake -S . -B build/d3d12 -A x64 -DLOXFX_BUILD_D3D12_SMOKE=ON -DBUILD_TESTING=ON
cmake --build build/d3d12 --config Release
ctest --test-dir build/d3d12 -C Release -R loxfx_d3d12_integration -V
```

The test requests D3D12 **WARP** for repeatable functional verification.
WARP GPU timestamp measurements are **not** representative performance.
The experimental library/headers are not installed. Real hardware tests and
full reference visual/performance comparisons remain necessary.

See the [backend parity contract](docs/BACKEND_PARITY.md). Neither backend
should introduce incompatible physics behavior or constrain the other to a
slow fallback.

## Direction

- Vulkan 1.4 and D3D12 Shader Model 6.6 are the intended shipping GPU floors;
  feature admission and actual device support are not implemented yet.
- Preserve the original TressFX behavior and performance strategy where useful;
  improve constraints, collision, simulation LOD and rendering based on measured
  consumer-hardware results.
- Godot is an early consumer, not an SDK dependency.
- Optional integrations with external physics and rendering systems may be
  introduced through explicit, independently maintained interfaces.

See the [engineering roadmap](docs/ROADMAP.md),
[modernization research](docs/MODERNIZATION_RESEARCH.md),
[bootstrap audit](docs/BOOTSTRAP_AUDIT.md), and
[reference audit](docs/TRESSFX_REFERENCE.md), and
[simulation source/performance trace](docs/SIMULATION_EXECUTION_TRACE.md), and
[optimization backlog](docs/OPTIMIZATION_BACKLOG.md).

Contributors and coding agents should follow [AGENTS.md](AGENTS.md),
including its required record of performance findings and deferred
optimization work at each substantive handoff.

## Licensing

New LoxFX code is Apache-2.0 (see [LICENSE](LICENSE)). Adapted or imported
TressFX code must retain AMD's MIT license, copyright and attribution.
The reference repository is not a build dependency.
