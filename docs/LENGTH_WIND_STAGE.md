<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Guide length and wind constraints — stage-five slice

## GPU execution

The first stage-five slice adapts the parallel length and wind portions of
AMD TressFX 4.1 `LengthConstriantsWindAndCollision`. Each 64-lane workgroup
processes guide vertices using the established master-guide mapping.
Positions and prepared segment rest lengths are loaded once per workgroup
into shared memory. The odd/even distance solver uses independent pairs
with group-wide barriers between constraint phases and iterations.

The wind term uses the reference's four-vector guide-dependent blend,
applied to movable interior vertices before the length iterations.
A configurable position-delta clamp controls excessively large velocities,
using actual delta magnitude rather than the original squared-ratio
calculation. The record-only backends preserve command submission and
inter-stage synchronization at host boundaries.

## Prepared GPU storage

The CPU asset retains its authored reference-compatible layout.
GPU preparation extracts only `padded_guides × vertices_per_strand` scalar
rest lengths from the already prepared geometry and uploads them once.
The immutable GPU allocation packs guide-only rest lengths followed by
one Float4 root offset/tip coefficient per packed strand. Stage five reads
4-byte guide lengths; stage six reads compact 16-byte strand records.
Neither needs per-frame rest-length recalculation, data copy or CPU readback.

Vulkan exposes the packed byte-addressed storage buffer at binding 5 in
the existing device-owned descriptor layout. D3D12 exposes the equivalent
root SRV at `t5`.
Both backends use the same 96-byte parameter layout and canonical HLSL
shader. Device admission enforces the required descriptor and push-constant
capacity.

## Verification

Software Vulkan and native D3D12 WARP execute matched fixtures:

- Exact odd/even four-vertex distance correction after a controlled
  gravitational displacement.
- Attached-vertex preservation and untouched follower storage with
  multiple followers per guide.
- Guide-dependent wind response for a movable interior vertex.
- A 64-vertex guide using the parallel shared-memory barriers.

This is a **partial stage five**: capsule/body collision, source tangent
updates, and velocity-response contact behavior are not implemented here.
They remain necessary before source-complete full-stage claims or visual
quality qualification. Follower publication is the next separate stage.

See [simulation execution trace](SIMULATION_EXECUTION_TRACE.md),
[optimization backlog](OPTIMIZATION_BACKLOG.md), and
[engineering roadmap](ROADMAP.md).
