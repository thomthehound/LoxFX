<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Local-shape constraints — stage four

## Solver contract

The fourth stage follows TressFX 4.1's `LocalShapeConstraints` source
algorithm. One GPU invocation owns an entire padded **guide strand** and
applies its corrections sequentially along that strand. Each local
triplet determines the rotation from the skinned rest segment to the
current segment, then adjusts the middle and following movable vertices
toward the corresponding rest-shape bend.

The source-normalized stiffness is `0.5 * min(local_stiffness, 0.95)`.
Vertex `w == 0` marks pinned endpoints; only position XYZ changes. Other
strands, generated followers, and current/previous velocity history remain
untouched by this stage. The skinning quaternion is read from the existing
strand-motion buffer; with the current translation-only attachment path it
remains identity until skeletal attachments are added.

## Iterations and synchronization

The TressFX host performs repeated local-shape dispatches for strands
shorter than 64 vertices. For 64-vertex strands, it passes the requested
iteration count to the shader in a single dispatch, but the released shader
does not actually loop. LoxFX executes the **intended** iteration count:

- 4–32 vertices: repeat the guide-strand dispatch using the prepared
  work-plan iteration count; insert Vulkan compute-memory / D3D12 UAV
  barriers between dependent dispatches.
- 64 vertices: one dispatch and an explicit per-guide shader loop over the
  prepared iteration budget, without unnecessary inter-dispatch barriers.
- Zero iterations or zero stiffness: record no local-shape work.
- Preparation limits the local-shape iteration budget to 64, preventing
  accidental unbounded CPU command recording and shader loops.

The implementation uses the existing guide/rest and per-guide motion
buffers. Vulkan adds a rest-buffer descriptor to the existing shared
descriptor-set layout; D3D12 adds a root SRV for the same binding. There
are no additional groom allocations, frame submissions, or CPU readbacks.

## Numerical verification

Matching software Vulkan and native D3D12 WARP tests cover:

- Exact four-vertex one-iteration source-stiffness corrections for an
  animated guide with two pinned proximal vertices.
- Multiple short-strand dispatches and corresponding GPU dependencies.
- One-versus-multiple internal iterations for 64-vertex strands.
- Stable pinned roots, unchanged generated followers, and finite results.

Native-device throughput and visual fidelity are evaluated after
length/wind/collision and follower publication provide the complete
renderable simulation baseline.

See [simulation execution trace](SIMULATION_EXECUTION_TRACE.md),
[backend parity](BACKEND_PARITY.md), and
[optimization backlog](OPTIMIZATION_BACKLOG.md).
