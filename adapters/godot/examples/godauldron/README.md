<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Native LoxFX hair laboratory experiment (Godauldron)

This optional directory is maintained by **LoxFX**, not Godauldron. Copy the
contents into `<Godauldron>/experiments/loxfx/`. The package inherits the
generic Godauldron portrait stage, requires the registered `LoxFXGroom3D`
native class, and runs the **real six-stage LoxFX Vulkan (Linux) or D3D12
(Windows)** simulation through SDK-owned diagnostic compute.

## Package the experiment

Build the LoxFX Godot GDExtension with
`-DLOXFX_BUILD_GODOT_ADAPTER=ON -DLOXFX_GODOT_NATIVE_DIAGNOSTIC=ON`, a
matching `godot-cpp` and DXC. Shader binaries are generated into
`<LoxFX build>/shaders/`.

Copy the extension library and shader binaries beside this package:

```text
Godauldron/experiments/loxfx/
  manifest.json
  loxfx.gdextension
  scenes/hair_lab.tscn
  scenes/hair_lab.gd
  assets/Ratboy_mohawk.tfx
  shaders/loxfx_integration.spv
  shaders/loxfx_strand_level.spv
  shaders/loxfx_velocity_shock.spv
  shaders/loxfx_local_shape.spv
  shaders/loxfx_length_wind.spv
  shaders/loxfx_follower_publish.spv
  bin/libloxfx_godot.so
```

On Windows use `.dxil` rather than `.spv` and `bin/loxfx_godot.dll`. The
TFX example is a real asset from the upstream
[AMD TressFX 4.1 reference](https://github.com/thomthehound/TressFX/tree/master/bin/Objects/HairAsset/Ratboy),
not a fake hair proxy; source it and comply with its applicable license.
No TFX binary is copied into LoxFX's source tree.

Godauldron's native experiment preflight will load the descriptor before
instantiating this project-owned inherited scene. For standalone Godot
project usage, package the library/shaders in a regular addon and call the
same native `LoxFXGroom3D` methods from a normal game scene.

## What the first demonstration actually proves

The authored head is the editable neutral Godauldron placeholder. Its
movement is limited to **translation** because the LoxFX skeletal-rotation
attachment API is unfinished. The diagnostic mesh counteracts parent
translation while native strand roots receive the corresponding native-space
translation, avoiding a double-motion visual trick. Hair-body contact,
rotation following, scalp fitting and actual hair shading are **not** solved
by this stage. Source geometry receives only a rough visual fit; judge
artistic quality after examining a hardware-backed viewport.

Interactive Reset/Step/Play invoke the actual LoxFX native pipeline. An
arbitrary timeline seek must rewind and replay native steps (bounded); the
host's analytic presentation-only seek is intentionally overridden.
Capture metadata truthfully identifies native simulation, the diagnostic
GPU→CPU→Godot presentation path, and unmeasured GPU frame cost.

**No synchronous readback result is a production frame-time number.** Future
work must negotiate Godot's renderer GPU resources and completion rules,
implement actual attachment orientation/collision, and shade real strand
geometry before claiming TressFX-quality visuals or performance.
