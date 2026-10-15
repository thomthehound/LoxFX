<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Follower and tangent publication — stage six

## GPU output

The sixth source-derived simulation stage publishes renderable positions and
tangents after guide length/wind correction. Each 64-thread compute workgroup
owns packed guide vertices; a lane calculates its final guide tangent and
expands that guide vertex into zero or more follower positions.

Follower positions are derived from the static prepared geometry:

```text
follower_current[v] = guide_current[v] + follower_rest[v] - guide_rest[v]
```

This reproduces the prepared source follower-root and tip-separation offsets
without a redundant per-follower offset buffer or per-frame host repacking.
Each follower copies the corresponding guide's final tangent. Degenerate
segments fall back to rest geometry (then a fixed unit vector), producing
finite normalized tangents.

The GPU retains interleaved guide/follower positions and a matching persistent
float4 tangent buffer. Native Vulkan `VkBuffer` and D3D12 `ID3D12Resource*`
handles are borrowed from internal groom state for future renderer adapters.
The host owns GPU queue submission, transitions, compute/graphics visibility,
and the underlying device lifetime. It must exclude padded guide strands
from visible output.

## Scheduling and resource ownership

The backend records the sixth stage after length/wind (and collision when
available), using an explicit compute-write/read dependency. There are no
per-frame CPU reads or allocations in the record path. Diagnostic readback
methods are exclusively for numerical tests.

The pass intentionally computes guide tangents **after** the current stage-five
length corrections; TressFX's fifth stage ordinarily emits tangents, which
its sixth stage forwards to followers. This placement avoids retaining a
separate intermediate guide-tangent array while publishing equivalent final
guide/follower tangent directions.

## Verification

Native Vulkan software and D3D12 WARP fixtures check:

- Animated guide positions and source-prepared follower offsets, including
  nonzero root radius and tip separation.
- Normalized guide tangents and follower tangent equality.
- Repeatable publication without accumulating offsets.
- Zero-follower guide-only output.

Stage-five collision and complete skeletal attachment inputs remain open.
The GPU publication buffers enable a minimal visual consumer, followed by
matched reference quality and full-frame hardware profiling.

See [the roadmap](ROADMAP.md), [the execution trace](SIMULATION_EXECUTION_TRACE.md),
and [stage-five contract](LENGTH_WIND_STAGE.md).
