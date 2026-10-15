<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Godot adapter — initial diagnostic strand viewer

The optional `LoxFXGroom3D` GDExtension is the first layer of a **reusable
Godot engine adapter**. It is deliberately independent of Godauldron.
It reads `.tfx` bytes through `LoxFX::Core`, prepares guide and follower
geometry, and displays the **authored** guides (excluding padded guides) plus
their generated followers as line segments. An external native producer may
supply updated **packed** `Vector3` vertex positions using
`publish_positions()` without changing LoxFX simulation mathematics.

**Scope and evidence:** the initial adapter is an SDK-to-engine asset and
dynamic-geometry boundary, **not yet an executing LoxFX simulation adapter**.
The internal Vulkan and D3D12 smoke runtimes are *not* promoted into public
SDK APIs. The bridge to those GPU producers is the next integration increment.
The line renderer and CPU updates are intentionally debug-only; they are not
hair shading and must not be benchmarked as native GPU interoperability.

## Building (optional)

LoxFX's default build remains standalone and has **no Godot dependency**.
Build against a `godot-cpp` checkout matching the actual Godot host API;
for an upstream/master Godot build pass its generated extension API file via
`GODOTCPP_CUSTOM_API_FILE` according to godot-cpp's CMake contract.

```shell
cmake -S . -B build/godot \
  -DLOXFX_BUILD_GODOT_ADAPTER=ON \
  -DLOXFX_GODOT_CPP_SOURCE_DIR=/path/to/godot-cpp \
  -DGODOTCPP_CUSTOM_API_FILE=/path/to/extension_api.json
cmake --build build/godot --config Release --target loxfx_godot
```

The `loxfx.gdextension` descriptor generated in the build tree uses the
configured `LOXFX_GODOT_COMPATIBILITY_MINIMUM` (default 4.7; set this to
the actual earliest compatible API). Place it at
`res://addons/loxfx/loxfx.gdextension`; copy the compiled shared library
into the matching `res://addons/loxfx/bin` directory. This is an explicit
developer build, not a prebuilt binary distribution.

## Using it in a Godot or Godauldron scene

```gdscript
# In a Node3D scene after the extension has been loaded:
var groom := LoxFXGroom3D.new()
add_child(groom)
var source := FileAccess.get_file_as_bytes("res://grooms/example.tfx")
assert(groom.load_tfx_bytes(source, 2, 0.02, 0.2, 5))
print(groom.get_authored_guide_count())
# Only publish positions from a real LoxFX simulation/output provider.
# The Array must include padded guides in LoxFX's packed order.
# groom.publish_positions(packed_simulation_positions)
```

`publish_positions` checks total packed vertex count and finite visible
positions. It updates the existing Godot mesh buffer, not simulation state;
the caller must choose its synchronization and sampling boundaries. Since
this deliberately passes positions through CPU memory, its transfer and
conversion costs are **not representative of the production rendering path**.

Call `load_tfx_bytes` and `publish_positions` on the Godot scene
thread. A native worker or GPU queue should transfer its completed diagnostic
snapshot across a controlled boundary before the scene node updates the mesh.
`LoxFXGroom3D` does not solve thread affinity for native producers.

The vertex update intentionally copies uncompressed 32-bit XYZ data: Godot's
render vertex-buffer format uses three float32 components even if an engine
build uses double-precision CPU `Vector3` values. The adapter bounds both
visible vertex counts and total upload bytes, and excludes workgroup padding
from the displayed mesh. The Linux runtime smoke is configured to exercise
Godot 4.7.2 with a real AMD Ratboy asset. Passing this smoke would not
constitute a visual GPU-output test.

The adapter does not select Vulkan/D3D12 devices, share Godot native
resources, submit on the renderer queue or block each simulation stage.
Those will require a separately reviewed, backend-local GPU interop contract
and a confirmed Godot renderer device and queue lifetime/synchronization
policy. Godauldron must not invent that policy on its behalf.

## Next acceptance criteria

1. Test against the selected Godot native API on Linux and Windows.
2. Display a real asset with working camera, exposure, contrast and stable
   per-frame mesh updates. Ensure debug lines remain visually distinct from
   production ribbon/hair rendering.
3. Connect both native GPU backends to publish diagnostic snapshots without
   making the smoke harness a public API.
4. Introduce proper GPU-resident consumption on render-supported platforms,
   validate compute/graphics cross-queue correctness, and compare whole-frame
   quality and cost with the independent native baseline.
