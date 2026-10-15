<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Animated-root integration and compact motion history — PR #15

Source anchors: TressFX 4.1 `src/Shaders/TressFXSimulation.hlsl`:
`IntegrationAndGlobalShapeConstraints` at lines 610–666;
`UpdateFinalVertexPositions` at lines 576–586; and
`CalculateStrandLevelData` at lines 678–728.

## This slice actually implements

- A host-supplied world-space **translation** of the rest/attachment
  geometry, passed as three floats in the original 48-byte shared
  integration constants. Both SPIR-V and DXIL use the same HLSL.
- Pinned guide roots follow the translation. Remaining movable vertices
  respond through the existing Verlet/global-rest-shape computation. This
  is **translation only**, not skeletal rotation, matrix skinning, teleport
  reset or complete moving-mesh behavior.
- `UpdateFinalVertexPositions` history ordering: save the previous value
  before overwriting the previous/current position. A separate **GPU UAV
  stores just two `Float4` history entries per padded guide** (root
  vertices 0 and 1), matching the subsequent strand-level stage's
  actual reads. Do not allocate another full guide/follower history array
  for this dependency without proof that other stages require it.
- Buffers are uploaded once with existing groom preparation; frame
  integration only updates GPU resources and per-dispatch constants.
  Diagnostic readback is outside the recording hot path.
- Vulkan and D3D12 numerical fixtures animate root translations across
  distinct GPU submissions and inspect the older root history.
- Merged PR #14's late P2 is corrected here: a host-borrowed D3D12
  command queue must belong to the **same COM device identity**. Tests
  construct two independent WARP devices and reject mixed handles.

## Next real stage

The full `CalculateStrandLevelData` shader must build strand rotation/
translation and `skinningQuat` for each guide, consuming the current,
previous and compact previous-previous root pairs. The reference
also uses rest length 0, per-strand bone skinning, VSP coefficient and
acceleration threshold. This slice does **not** fabricate those values
or claim stage-two execution yet.

After that, `VelocityShockPropagation` consumes the strand-level
rotation, translation and coefficient. Preserve the order
integration → strand-level data → VSP and appropriate GPU memory barriers.
Animated bone transforms and robust local-frame/quaternion handling
should be added with source-faithful cross-backend tests, not approximated
through a separate CPU physics pipeline.

**Performance obligations:** test full scenes on physical consumer GPUs.
This history representation is a justified structural memory reduction
relative to storing previous-previous positions for every follower, not
a measured frame-time speedup. OPT-001/002/003/009/011 remain open.
