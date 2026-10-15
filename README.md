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
follow strands. A standalone inspection tool, source-traced simulation dispatch planning and
regression tests are included. **Hair simulation, GPU execution, rendering and engine
integration are not implemented in this initial milestone.**

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
`loxfx_tfx_inspect path/to/hair.tfx 3`. The printed thread counts are analytic,
not measured GPU execution times.

Install and consume LoxFX as a standalone CMake package:

```sh
cmake --install build --config Release --prefix /path/to/loxfx-install
```

Downstream projects can use `find_package(LoxFX CONFIG REQUIRED)` and
`target_link_libraries(app PRIVATE LoxFX::Core)`. The package exports its
C++23 requirement. The public ABI is not yet frozen.

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
[simulation source/performance trace](docs/SIMULATION_EXECUTION_TRACE.md).

## Licensing

New LoxFX code is Apache-2.0 (see [LICENSE](LICENSE)). Adapted or imported
TressFX code must retain AMD's MIT license, copyright and attribution.
The reference repository is not a build dependency.
