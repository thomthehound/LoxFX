<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# LoxFX

LoxFX develops an engine-independent, cross-platform SDK for real-time
hair and fur. Its C++23 core processes and prepares strand-based assets,
while native Vulkan and Direct3D 12 backends execute initial simulation
stages from shared HLSL. The design keeps simulation, rendering, and
engine integration decoupled.

LoxFX takes a source-grounded approach to AMD TressFX 4.1: retain useful
algorithms and asset compatibility, improve data layout and execution,
and evaluate changes against visual quality and full-frame GPU cost. The
[TressFX reference repository](https://github.com/thomthehound/TressFX)
provides an independent comparison baseline.

## Implementation

| Component | Current capability |
| --- | --- |
| Portable core | C++23 asset model, validated TressFX 4.x `.tfx` import, optional attributes, and an installable CMake package |
| Groom preparation | Persistent strand data, rest geometry, tangents, segment lengths, deterministic followers, and source-aligned GPU work plans |
| Vulkan | Native compute execution through follower publication, with persistent positions/tangents and caller-recorded commands; collision remains outstanding |
| Direct3D 12 | The same shared HLSL stages executed as SM6.6 DXIL through native D3D12, with independent resource binding and command recording |
| Verification | Windows/Linux core tests, Linux software-Vulkan tests, Windows D3D12 WARP tests, and shared numerical fixtures |

The installed SDK currently exports **`LoxFX::Core`**. GPU implementations
are available as internal integration harnesses while the complete simulation
pipeline and SDK-facing backend interfaces take shape.

### Simulation pipeline

LoxFX follows the six-stage TressFX guide-strand execution model:

1. **Integration and global shape** — Verlet motion, translated root
   attachments, pinned roots, and rest-shape relaxation. *Executing.*
2. **Strand-level motion** — per-guide rotation, translation, and
   velocity-shock parameters using compact root-position history. *Executing.*
3. **Velocity-shock propagation** — apply strand-level motion to guides. *Executing.*
4. **Local shape constraints** — preserve strand curvature and local structure. *Executing.*
5. **Length, wind, and collision** — length and wind execute; contact and
   final tangent updates remain.
6. **Follower update** — publish guide/follower positions and final tangents. *Executing.*

Development continues through full skeletal attachments, body collision,
and a minimal visual consumer of the published GPU buffers. Physical-GPU
profiling and matched reference-scene comparisons will then guide performance
and rendering improvements.

## Godot engine adapter (opt-in)

A new, independently packaged GDExtension can import and prepare TressFX grooms
and display packed guide/follower positions in Godot using a diagnostic line
mesh. The generic Godot node is **not tied to Godauldron**. Native GPU
simulation, production strand shading and zero-copy rendering remain separate
integration milestones; do not benchmark the diagnostic CPU upload path as a
GPU renderer. See [Godot adapter](adapters/godot/README.md).

## Build the core

Requirements: **CMake 3.28+** and a **C++23 compiler and standard library**
on Windows or Linux. The core requires no GPU SDK or game engine.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Inspect an existing TressFX 4.x groom:

```sh
loxfx_tfx_inspect path/to/hair.tfx
loxfx_tfx_inspect path/to/hair.tfx 3 --prepare
```

The optional follower count and `--prepare` report the prepared strand
layout, static data footprint, and planned GPU work. Strand preparation
happens once, outside the simulation frame loop.

Install the core for another CMake project:

```sh
cmake --install build --config Release --prefix /path/to/loxfx-install
```

```cmake
find_package(LoxFX CONFIG REQUIRED)
target_link_libraries(app PRIVATE LoxFX::Core)
```

Point `CMAKE_PREFIX_PATH` at the installation prefix when configuring
the consumer. The exported target carries the C++23 requirement.

## GPU development

The two native backends share simulation mathematics and prepared groom
semantics while keeping device ownership, resource binding, and command
submission API-specific. Multiple grooms can share one device context.
Simulation stages record into caller-supplied commands; the host controls
submission and synchronization.

### Vulkan

With the Vulkan SDK and `dxc` available:

```sh
cmake -S . -B build/gpu -DLOXFX_BUILD_VULKAN_SMOKE=ON -DBUILD_TESTING=ON
cmake --build build/gpu --config Release
ctest --test-dir build/gpu -C Release -R loxfx_vulkan_integration --output-on-failure
```

### Direct3D 12

On Windows with the Windows SDK and `dxc`:

```powershell
cmake -S . -B build/d3d12 -A x64 -DLOXFX_BUILD_D3D12_SMOKE=ON -DBUILD_TESTING=ON
cmake --build build/d3d12 --config Release
ctest --test-dir build/d3d12 -C Release -R loxfx_d3d12_integration --output-on-failure
```

CI exercises the Vulkan harness with Mesa software Vulkan and the D3D12
harness with WARP. These establish functional execution and numerical
checks; performance qualification uses physical GPUs and matched-quality
reference scenes.

## Architecture and roadmap

The design separates authored assets, prepared simulation storage, native
GPU execution, and optional rendering or engine adapters. **Vulkan 1.4**
and **D3D12 Shader Model 6.6** are the intended production targets;
Godot is an early integration consumer, not a dependency.

The engineering priorities are to complete the remaining three stages,
validate animated attachments and collision, publish renderable follower
data, and measure visual quality and full-frame cost before selecting
further solver, LOD, or rendering optimizations.

- [Engineering roadmap](docs/ROADMAP.md)
- [GPU ownership and execution architecture](docs/GPU_RUNTIME_ARCHITECTURE.md)
- [D3D12/Vulkan parity contract](docs/BACKEND_PARITY.md)
- [Reference simulation trace](docs/SIMULATION_EXECUTION_TRACE.md)
- [Optimization backlog](docs/OPTIMIZATION_BACKLOG.md)
- [Modernization research](docs/MODERNIZATION_RESEARCH.md)

Contributors should consult [AGENTS.md](AGENTS.md) for design, validation,
and review requirements.

## License

LoxFX is licensed under [Apache-2.0](LICENSE). Code adapted from AMD
TressFX retains its original MIT license and attribution; see
[NOTICE.md](NOTICE.md). The reference implementation is not a build
dependency.
