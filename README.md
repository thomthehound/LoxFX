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
follow strands. A standalone inspection tool and synthetic importer regression
tests are included. **Hair simulation, GPU execution, rendering and engine
integration are not implemented in this initial milestone.**

## Build

Requires CMake 3.21+ and a C++23 compiler and standard library on Windows or Linux. No graphics
SDK, CUDA runtime, PhysX, Cauldron or Godot installation is needed.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Inspect a TressFX 4.x groom with `loxfx_tfx_inspect path/to/hair.tfx`.

## Direction

- Vulkan 1.4 and D3D12 Shader Model 6.6 are the intended shipping GPU floors;
  feature admission and actual device support are not implemented yet.
- Preserve the original TressFX behavior and performance strategy where useful;
  improve constraints, collision, simulation LOD and rendering based on measured
  consumer-hardware results.
- Godot is an early consumer, not an SDK dependency.
- Optional integrations with external physics and rendering systems may be
  introduced through explicit, independently maintained interfaces.

See [engineering roadmap](docs/ROADMAP.md) and
[reference audit](docs/TRESSFX_REFERENCE.md).

## Licensing

New LoxFX code is Apache-2.0 (see [LICENSE](LICENSE)). Adapted or imported
TressFX code must retain AMD's MIT license, copyright and attribution.
The reference repository is not a build dependency.
