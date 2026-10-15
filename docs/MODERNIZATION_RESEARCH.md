<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# LoxFX — Modernization Research and Engineering Options

**Status:** Research-backed engineering options; updated 15 October 2026. The milestone gates in [ROADMAP.md](ROADMAP.md) track implementation.  
**Repository:** https://github.com/thomthehound/LoxFX  
**Reference:** [AMD TressFX 4.1](https://github.com/thomthehound/TressFX) (January 2020; MIT). The implementation is maintained separately for comparison.

## Executive decision

Modernize LoxFX as an **engine-independent, cross-vendor, real-time strand and fur simulation SDK**, with optional hair-rendering components and adapters. Retain the source-grounded TressFX 4.1 implementation as the first functional and performance reference. Develop newer algorithms as measured alternatives, not preconditions for a working release. The differentiator is a usable, permissively licensed, portable system on ordinary consumer GPUs—not cinematic strand counts purchased with excessive GPU time, memory, or engine coupling.

The highest-value near-term investments are (1) correctness and repeatable benchmarking, (2) decoupling the core from Cauldron while preserving dispatch/dataflow contracts, (3) guide/strand/render LOD and GPU-resident orchestration, (4) robust collision and fixed-step behavior, and (5) a practical authoring pipeline. XPBD compliance, volume-mediated inter-strand interaction, and improved anisotropic shading merit bounded experiments. Full Cosserat rods on every strand, neural solvers, hardware-specific ray tracing as the primary renderer, and obligatory mesh-shader pipelines are **not** launch requirements.

**Decision rule:** An improvement must demonstrably increase image quality, simulation stability, authoring usefulness, integration flexibility, or throughput on target hardware. Literature results on other systems do not count as a LoxFX speedup.

## 1. Provenance, licensing, and target platform

### 1.1 Source and license posture

- LoxFX is an independent Apache-2.0 repository. AMD's original TressFX source and MIT license remain in the separate [reference repository](https://github.com/thomthehound/TressFX); no AMD source file has been imported into LoxFX yet. If an implementation is subsequently adopted, retain the applicable original MIT text and AMD copyright/attribution.
- License **new LoxFX-authored files under Apache-2.0 with SPDX identifiers**; preserve the original licenses and attribution for any imported third-party work. Track incorporated third-party files in [NOTICE.md](../NOTICE.md). Changes that mix code from different origins require per-file review, not automatic blanket relicensing.
- Review sample models, textures, Maya exporter material, submodules, binary redistributions, and any subsequently incorporated algorithms/assets separately from source-code licensing. An academic paper is evidence for a technique, **not** a grant to copy its implementation.
- The root Apache-2.0 license covers LoxFX-authored files; if MIT-origin source is imported, its own license and attribution remain effective. Do not imply that AMD-authored files originated under Apache-2.0.

### 1.2 Runtime and hardware requirements

| Area | Intended requirement | Qualification |
|---|---|---|
| CPU | Functional CPU path and headless reference/diagnostics | Not necessarily the real-time performance target for dense hair |
| GPU APIs | Native Vulkan 1.4 and Direct3D 12 SM 6.6+ | Independently checked at device initialization, not inferred from OS/vendor name |
| GPU operations | Actual subgroup/wave support and arithmetic for high-throughput paths | Subgroup size/operations probed; shader-model level alone is insufficient |
| Reference family | AMD RDNA1-class API floor, AMD Radeon 680M-class practical low end | Release support depends on real API/driver capabilities and performance tests |
| Other vendors | Intel integrated Arc and NVIDIA as cross-vendor targets | Avoid vendor-identity special cases; compile capability-selected variants |
| Optional acceleration | wave-size variants, FP16 storage, supported mesh shaders, advanced atomic/RT functions | Never required by baseline; enabled only with measured benefit |
| Compiler | HLSL + DXC to DXIL/SPIR-V as the initial shared-shader candidate | Reconsider only if measured compiler, portability or debugging evidence warrants it |
| Platforms | C++23 and CMake 3.28+, Linux and Windows first; native SDK and headless tests | Godot is an optional adapter/acceptance harness, not a runtime dependency; CI uses ubuntu-24.04 and windows-latest |

Perform device capability selection and shader variant caching during device/context creation. **No device interrogation on each dispatch.** Preserve a small set of purposeful kernel variants, rather than an explosion of mutually incompatible paths. Capability and actual throughput must both pass admission criteria; development software adapters provide functional CI, not production performance evidence. [1]

## 2. What exists, and what the newer systems teach

The reference TressFX 4.1 tree already contains the right major components: a six-pass strand simulation orchestration in `src/TressFX/TressFXSimulation.cpp`; integration/global-shape constraints, strand-level data, velocity shock propagation, local-shape constraints, length/wind/collision, and follow-hair updates in `src/Shaders/TressFXSimulation.hlsl`; SDF collision infrastructure; and separate ShortCut and per-pixel linked-list transparency implementations. Its engine interface (`EI_*`) was intended to isolate host resources and dispatches, but needs a resource ownership and dispatch-dependency audit before replacement. [2]

Contemporary competitors validate certain patterns without proving that TressFX must be discarded:

| Reference | Transferable insight | Important limit |
|---|---|---|
| Unreal Groom/Niagara | GPU XPBD, angular spring/Cosserat options, voxel velocity-field approximation to self-interaction, guide/render separation | Engine-specific implementation and resource graph [3] |
| Unity Demo Team Hair | Hierarchical clustering, coverage-preserving LOD, indirect dispatch, variable simulation quality, volume density/pressure, authored curves | Unity-specific code/license; reported 1.5–2.5× improvement is *its* solver benchmark, not LoxFX [4] |
| AMD TressFX 5.0 for UE5 | Continued practical value of LOD, deep shadow approximation, indirect clearing, engine lighting integration | No current independent TressFX 5 SDK release [5] |
| TressFX-Godot (2026) | Independent demonstration of Blender export, GDExtension integration, GPU simulation and collision | Reports high per-vertex memory/triangle costs, frame-dependent simulation, limited OIT/shadows. Good comparison target, not proof of production cross-vendor parity [6] |
| 2024 hair meshes/interpolation research | Compressed representation and better reconstruction from fewer simulated guides | Published peak results depend on method/hardware/scene; integration and authoring costs unmeasured in LoxFX [7,8] |

**Differentiation test:** If LoxFX can be embedded in a tiny native C++ application and at least one independent engine without carrying Cauldron or an engine's renderer, it fills a practical gap that engine-specific Groom packages do not.

## 3. High-priority techniques: direct engineering returns

### 3.1 Separate three different LOD decisions — **P0 / high return**

Maintain distinct policies for **simulation guides**, **rendered strands**, and **representation**. Simulate a compact set of physically meaningful guides; reconstruct followers and maintain clumps; reduce guide density and update cadence based on projected influence, visibility, motion, and collision risk; select strand ribbons, cards, or impostor/volume approximations independently for rendering. Preserve perceived density and silhouette when transitioning LOD, rather than merely deleting random strands. When a distant groom becomes near-field, reconstruct state with stable velocities and rest-shape consistency to avoid popping.

Keep collision-active strands near interacting surfaces alive even if their owning groom is partially occluded. Screen-space LOD alone cannot detect off-screen physical contacts. Allocate/compact guide work on GPU and use indirect dispatch/draw where supported so selected workload actually decreases; avoid readback to determine dispatch counts. Schedule hair groups together to amortize small dispatch overhead.

**Measure:** simulation particle count, processed strand count, overdraw, GPU dispatch count, actual frame GPU time, memory footprint, popping/flicker score, and LOD transition latency. Begin with static hierarchical clusters and bounded GPU selection; procedural representation changes come later. Unity's clustering/coverage technique and 2024 hair-mesh results are research references, not source to copy. [4,7]

### 3.2 Preserve GPU-resident simulation and existing orchestration — **P0 / high return**

Audit the host graph from skinned roots to updated positions and render publication. Record buffer owners, frame roles, barriers, per-groom constant buffers, SDF update invalidation, and ordered launch dependencies. Retain semantics while coalescing redundant dispatches and barriers **only after** documenting real producer/consumer hazards. Reuse persistent descriptors, upload rings, pipeline objects, staging resources, scratch allocations, and device-local history. Track peak allocations and garbage collection/resizing pauses.

A small-frame GPU hair workload can be dominated by many submissions, independent shader binds, CPU stalls, or repeated SDF generation, not constraint arithmetic. Integrate the GPU timeline with render consumption without blocking the CPU each frame. If a renderer cannot share GPU resources, explicitly charge transfer cost and latency to that adapter rather than hiding it in the benchmark.

**Measure:** GPU timestamps per stage and total, submission cost, number of UAV/compute barriers, frame overlap, CPU time, readback bytes, transient allocation count, pipeline compilation/pacing. No per-dispatch device feature probes. Keep the simulation and execution contracts usable without a specific external runtime. [2]

### 3.3 Fixed timestep with graceful catch-up — **P0 / high return**

Separate simulation time from render frames. Support a configurable fixed timestep, bounded substeps, frame-rate-independent material parameters, interpolated rendering, and deterministic reset/teleport handling. Maintain explicit previous/current root frames and strand positions; do not naïvely feed a large `dt` through velocity shock propagation. On stalled frames, limit catch-up work and choose an explicit degraded mode instead of running unbounded steps or silently increasing stiffness.

**Measure:** style preservation and overshoot under 30/60/120 Hz presentation, animation stops/teleports, identical simulated elapsed time under variable render cadence, worst-case step count, and incremental GPU cost. This addresses a known weakness of at least one current TressFX-Godot implementation. [6]

### 3.4 Collision representations as an adaptive portfolio — **P0→P1 / high return**

Keep TressFX's SDF collision capability, but use the cheapest *correct enough* collider for each case: analytical spheres/capsules for broad scalp/neck/shoulders; localized SDFs for irregular static or moderately deforming surfaces; dynamic mesh proximity or local signed-distance patches for tight contact regions. Update distance data only when relevant geometry changes; cache static regions. Start with physically meaningful hair radius/margin and friction, plus segment-level or swept tests for fast moving hair, rather than relying entirely on vertex distance samples that can tunnel through thin or moving objects.

Avoid treating unsigned or poorly sampled distances as dependable signed fields for open garment meshes. Prefer a conservative contact mode or user-directed collider semantics where sign cannot be established. Use a local cell budget and spatial bounds; rebuilding full high-resolution SDF volumes every frame is not a viable baseline on integrated GPUs. Address the open 4.1 multiple-hair-object constant-buffer aliasing report as a correctness acceptance case. [2,9]

**Measure:** penetration depth, tunnel-through rate, false push-out, SDF update milliseconds, changed-cell fraction, contact behavior on fast head turns, memory per collider, and multi-groom isolation.

### 3.5 Memory layout and subgroup-aware kernels — **P0→P1 / high return**

Audit positions/history/tangents/inverse masses/root frames, SoA versus AoS access, padded strand lengths, and follow-hair duplication. A useful reference workload is 8k guides × 16 particles = 128k simulated particles: just three 16-byte position/history buffers occupy ~5.9 MiB before tangents, rest data, constraints, followers, and render resources. This arithmetic is not an empirical footprint result; it shows why every additional vector per particle matters.

Design wave/subgroup-based intra-strand reductions where helpful, but do not assume a fixed 32-lane wave or that every workgroup is exactly one wave. Test 16/32/64 point strands and varying subgroup widths. FP16 **storage** of secondary/rest/follower data and compressed tangents/offsets may save bandwidth; keep state integration precision adequate for long hair, fast motion, and accumulation. Prefer compressed relative offsets for followers over redundant full dynamic state when validation permits.

**Measure:** register use, memory traffic, occupancy, LDS traffic, barriers, cache misses, bandwidth, numerical drift, artifact thresholds, and per-groom allocations. FP16 arithmetic and packed storage are different choices. Matrix instructions/WMMA are not a general-purpose accelerator for mostly sparse, short-chain strand constraints. [1]

## 4. Simulation improvements worth controlled experiments

### 4.1 XPBD-style compliance on selected constraints — **P1 / strong candidate**

The main technical attraction is reduced sensitivity of effective stiffness to timestep and iteration count. Introduce an **optional**, testable XPBD version of stretch and bend/local-shape constraints, preserving the original TressFX solver as a reference. Reset or transport constraint multiplier state correctly across frame discontinuities and LOD reconfiguration. Compare constraint convergence, time-to-pose, and costs across realistic grooms, not only isolated rods. [3,10]

**Adopt if:** higher fidelity or more stable authoring at comparable budget, or materially fewer iterations for equal visual error. **Reject/limit if:** multiplier storage, register pressure, and additional passes cost more than the quality benefit for common guide counts.

### 4.2 Local rod/bending models for curly and styled hair — **P1 experiment / selective**

Higher-order bend/twist control and rest-curvature preservation can improve braids, curls, stiff whiskers, and tightly styled hair. Implement as a **quality mode for selected guide groups**, not a universal Cosserat-rod solver. Keep cheap angular/local-shape constraints for most strands. More degrees of freedom, stable frames, additional state, and iteration cost make full rod dynamics expensive at scale; even the elastic-rod literature often demonstrates hundreds, not tens of thousands, of fully resolved rods in real time. [3,11]

**Adopt if:** curl shape survives vigorous head motion and contact at acceptable incremental milliseconds and memory on an integrated GPU.

### 4.3 Approximate collective hair behavior — **P1 experiment / bounded resolution**

Guide-to-guide interactions and groom volume matter more perceptually than perfect pairwise physical collisions. Evaluate a **coarse voxel velocity/density field** to resist unrealistic bulk collapse and impart coherent motion, supported by local neighboring-clump repulsion for close interactions. This follows the principles found in Unreal's average velocity field and Unity's volume-density treatment. Keep resolution proportional to screen footprint and physical volume; use sparse/local grids when a dense grid becomes wasteful. [3,4]

Do not start with all-strand pairwise collision or dense 3D high-resolution fluid simulation. Use a contact-density and over-compression fixture; measure atomics/contention, rasterize/sampling passes, memory, and apparent volume.

### 4.4 Strand reconstruction and interpolation — **P1 / high potential**

Reconstruct followers from simulated guide frames using rest-relative coordinates, orientation transport, and style/clump descriptors. The 2024 physically guided interpolation literature identifies major errors in naïve precomputed skinning when hair deforms far from rest, especially straight/kinked and curly/zigzag cases. Test selected corrections that maintain follower rest lengths and guide-frame continuity while avoiding a separate physical simulation for followers. [8]

**Adopt if:** substantial reduction in the guide count required to achieve a given visual error, with reconstruction cheaper than simulating the removed guides.

## 5. Rendering: modular and budgeted, not a second mandatory engine

**Core contract:** Simulation produces positions, tangents, radius/material/LOD identifiers, and motion history. A renderer may use them for view-facing ribbons, clustered/follower geometry, strands, cards, volumes, or ray tracing. The host engine retains ownership of its render graph, lighting, temporal history, scene depth, and final compositing. The LoxFX package may ship its own reference renderer and reusable shader modules, but core simulation must not require them.

1. **Start with a robust inexpensive path:** two-sided tapered ribbons or a supported curve primitive, an anisotropic Kajiya–Kay / dual-lobe approximation, coverage-aware antialiasing, motion vectors, and a credible head-contact shadow. Avoid forcing MSAA; check TAA on/off and short-hair aliasing.
2. **Preserve ShortCut and PPLL as selectable reference paths:** ShortCut/limited-layer approximate OIT is an important quality/performance option; PPLL offers higher-quality ordering at memory/bandwidth/overflow cost. Add explicit allocation bounds and graceful capacity overflow handling. Measure fragment overdraw, shader cost, screen resolution scaling, and memory. Reassess weighted blended OIT or bounded K-buffer alternatives against representative grooms rather than assuming any one technique wins universally.
3. **Add a modest shadow hierarchy:** self-occlusion/root darkening and simple deep-opacity approximation first, better light-space transmittance at premium quality. Shadow passes can dwarf simulation costs for dense fur; choose quality per light and distance.
4. **Investigate compressed hair-mesh or curve-driven rendering as a separate R&D line:** SIGGRAPH 2024 shows major storage/bandwidth potential. Do not require mesh shaders for the baseline; the authors describe vertex/fragment and compute alternatives. Retain a conventional ribbon fallback. [7]
5. **Hardware RT and linear swept spheres are optional:** NVIDIA Blackwell LSS is an important future capability, but those reported performance gains are not portable guarantees for RDNA1/680M/Intel integrated Arc. RTX-specific features must never determine LoxFX's shipping floor. [12]

**Rendering gate:** Compare at matched lighting, pixel coverage, strand density, resolution, and temporal AA. Record GPU render milliseconds separately from simulation, VRAM and overdraw, specular breakup, scalp exposure, and self-shadowing quality. Do not use frame-rate-only comparisons with different feature sets.

## 6. Authoring, interchange, and asset model

Create a **neutral LoxFX groom asset** with explicit guide topology; root attachment coordinates/bone bindings; rest positions and local frames; material groups; per-group stiffness, damping and geometry radius; texture/width/colour attributes; clump/curl/style descriptors; and imported provenance. Preserve unknown source attributes in the preparation step, rather than silently dropping them. Store preprocessing products (LOD clusters, follower mappings, collision proxies) as regenerable derived data, not as the only representation of the authored groom.

Support `.tfx` as a compatibility import/export path. Prioritize **Blender hair curves via Alembic and USD BasisCurves** with validation of basis, units, coordinate conventions, curve width, group assignment, and root binding. Blender has supported hair-curves import/export in Alembic and USD since 4.2, but its current USD exporter documents attribute gaps (for example, parent hair color), so a LoxFX authoring utility may still be justified. [13,14]

Keep exporters and editors separate from the simulation SDK. Accept both an artist-authored groom and an engine-procedural groom. Avoid making Maya, Blender, Godot, Unreal, or a particular character topology mandatory.

## 7. Optional integration with external simulation systems

LoxFX must remain usable as an independent simulator. An external engine can
provide root transforms, collision geometry, and scheduling without becoming
the authority for LoxFX's guide-strand state. Interoperability may eventually
reduce resource copies, but it must be measured and must not impose a mandatory
dependency or a particular engine's data model.

**Potential integration levels:**

1. **Independent operation:** an application supplies attachment transforms and
   collision primitives or SDFs; LoxFX advances its simulation and publishes
   rendered/simulated strand buffers.
2. **Shared device execution:** an optional adapter accepts compatible allocator,
   queue, timeline/fence and descriptor contracts. Device capabilities and
   pipeline variants are resolved at setup, never per dispatch.
3. **Reciprocal contact:** a host that supports coupled rigid/deformable
   simulation may exchange collider state and aggregate impulses at defined
   substep/publication boundaries. Ownership of body and hair state remains
   explicit.
4. **Co-scheduling:** if benchmarks justify it, compatible hosts may run
   collision and hair work in a single dependency graph, while preserving each
   system's simulation ownership and standalone functionality.

**Avoid:** per-frame readback of the full groom, two conflicting owners of
rigid-body state, unsynchronized time domains, exposing backend-native handles
in neutral asset structures, or making zero-copy claims without validated
memory lifetime and hazard contracts.

## 8. Quantitative performance and quality validation

### 8.1 Minimum GPU tiers and fixtures

| Fixture | Example workload (test input, not a performance claim) | Main risk |
|---|---|---|
| Short hair / fur | 2k–5k guides, 8–16 points, modest follow density | overhead, silhouette, aliasing |
| Ordinary medium groom | 8k guides, 16 points; multiple moving colliders | solver throughput, SDF update, medium-range LOD |
| Dense character | 20k guides × 16 points; separate 50k–100k visual followers | VRAM, interpolation, shading/overdraw |
| Long/curly hair | 8k guides × 32 points, large displacements | stiffness, curl restoration, tunneling |
| Multi-character | 4–8 simultaneous grooms at varied distances | batching, memory persistence, LOD scheduling |
| Fur close-up | dense short groom with many screen-space overlapping fibers | overdraw, shadows, alpha handling |

Track AMD Radeon 680M-class integrated hardware as the practical lowest-performance reference, an RDNA1-class device where available for API/architecture coverage, modern discrete AMD, integrated Intel Arc, and an NVIDIA comparison system. Report driver/API support and actual feature admission for each. Software Vulkan/D3D12 adapters remain correctness-only CI paths.

### 8.2 Acceptance metrics

Record medians **and** p95/worst visible spikes for: simulate/root skin/constraints/collision/follower update/render/shadow GPU timestamps; CPU update and submissions; upload/download bytes; peak resident video/shared memory; allocations/frame; source-bound stage counts; worst penetration; visible popping, shimmer, and clump collapse. Compare against **the unmodified 4.1 LoxFX baseline on the same hardware**, an independent CPU/reference oracle where meaningful, and one contemporary renderer at equivalent visual settings. Report absolute costs, not only relative percentage gains.

Provisional interactive policy: allow the application to set a **per-groom and aggregate GPU time/memory budget**, with adaptive quality reduction before frame cost escapes control. Derive concrete tier budgets from measured baseline runs rather than inventing a universal 1 ms or 2 ms promise. For each optimization, state whether it improves latency, throughput, quality-per-millisecond, or merely shifts work between CPU and GPU.

**Tests that must exist:** frame-rate sweep; animation restart/teleport; rapid head rotation; wind; static/dynamic collision; overlapping groom instances with different particle counts; long curly and short stiff hair; interaction during LOD transitions; 1080p/1440p rendering; transparency overflow; device loss/resource resizing; separate native and Godot harness behavior. Test source-stage ordering and result-publication correctness, not just visual plausibility.

## 9. Prioritized implementation program

| Stage | Scope | Exit gate |
|---|---|---|
| **M0 — Baseline and compliance** | Establish SPDX/license notices, dependency and asset inventory; current CMake builds; sample/test harness; reproducible shader compilation; benchmark capture; categorize 4.1 issues | Original demos run on supported development hardware; stage timings and behavior documented; no undocumented change of source semantics |
| **M1 — Engine-neutral GPU simulator** | Isolate core asset/state/solver and host launch graph from Cauldron; native Vulkan and D3D12 backends; CPU reference harness; no compulsory rendering dependency | Standalone sample runs the same groom with documented GPU resources and dispatch dependencies on both APIs; native oracle agrees within justified physical tolerances |
| **M2 — Real-time scalability** | Persistent buffers, batching, guide/follower representation, indirect LOD, fixed timestep, compiler/kernel instrumentation | Measured cost/quality improvements on lower-end GPU; no new per-dispatch capability queries or CPU readback for LOD decisions |
| **M3 — Contact and simulation quality** | Fix SDF ownership/update bugs; analytical and local-SDF collision; selected XPBD constraints; bounded volume interaction | Multi-groom and moving-body tests pass; quantified penetration/stability improvement without unacceptable time/memory regressions |
| **M4 — Render and authoring adapters** | Reference ribbon shading; optional ShortCut/PPLL; shadows; Blender/Alembic/USD ingest; Godot adapter | Demonstrate one groom rendered through independent native renderer and Godot with traceable imported attributes and quality settings |
| **M5 — Interop and advanced R&D** | Optional external scheduling/contact integrations; guided follower physics; hair mesh representation; selective advanced material/RT features | Each optional feature proves unique value under a documented consumer GPU budget and leaves the baseline available |

The order is strategic, not a mandate to complete every feature in each stage. Parallelize source licensing/build work and benchmark infrastructure early; keep simulation algorithm changes behind A/B switches until validated. Public engine-neutral APIs should stabilize only after at least two different hosts have exercised them.

## 10. Explicitly deferred or rejected as default architecture

- **Fully physical per-fiber contact for every visual strand:** quadratic/neighborhood explosion and unstable cost under dense grooms. Simulate guides and use collective approximations first.
- **Dense whole-head 3D fluid/grid simulation at high resolution:** excessive rasterization, atomic contention, and bandwidth for limited incremental visual gain on the baseline GPU. Bounded coarse grids are different and worth evaluating.
- **Full Cosserat rods throughout:** extra per-segment frames/state and solves can easily erase the advantage of a lightweight guide pipeline. Quality mode only if it wins measured tests.
- **Neural/learned dynamics or offline precomputed simulation as a runtime dependency:** training data, portability and authoring burdens; may be useful for future cached/styled modes but not a baseline SDK.
- **Mandatory hardware RT, geometry/mesh shaders, cooperative matrices, or vendor libraries:** violates practical hardware baseline and portability goals. Optional backends may exploit them.
- **One universal high-quality transparency algorithm:** PPLL can be costly, while approximate OIT may be perceptually insufficient in close-ups. Expose selectable bounded paths.
- **Full rewrite before baseline measurement:** loses proven orchestration and performance strategy. Change only what a source audit or measured bottleneck justifies.
- **An engine-specific Godot-only implementation:** a useful adapter but not the definition of the SDK; an existing MIT Godot port already serves as a point of comparison. [6]

## 11. First engineering investigations (highest information value)

1. **Baseline build-and-profile:** compile the unmodified reference DX12/Vulkan samples with current toolchains, enumerate actual stages and barriers, add GPU timestamps and a representative suite. Record failed dependencies/build obstacles separately from algorithmic defects.
2. **Simulation graph/source audit:** map `TressFXSimulation::Simulate`, `TressFXHairObject`, `TressFXSDFCollision`, resource lifetimes, double-buffered uniforms, update/skin/collision/render publication, and all existing synchronization. Capture the multiple-hair SDF constant-buffer report as a unit/integration fixture.
3. **Scalability proof:** profile three guide densities, 8/16/32 point strands, and one versus four moving grooms. Implement a minimal guide-only indirect LOD experiment without replacing the solver; quantify whether dispatch, memory, interpolation, or shading dominates.
4. **XPBD A/B micro-study:** introduce compliance for one expensive/stability-limiting constraint family behind an option; run equal-cost quality comparisons with the original solver under multiple `dt`/iteration settings.
5. **Adapter contract demonstration:** construct a tiny C++ host and an adapter consuming positions with explicit GPU synchronization. Defer tight host-engine integration until LoxFX's own resource and contact interfaces are understood.

**Go/no-go after M1/M2:** Continue if a genuinely embeddable core works on Vulkan and DX12, known correctness defects are bounded, and useful groom densities meet an application-configurable interactive cost target on lower-end supported consumer hardware. Reassess large architectural rewrites only with profiler evidence that the legacy solver or representation imposes a fundamental ceiling.

## Sources and reference map

[1] Khronos, [Vulkan subgroup guide](https://docs.vulkan.org/guide/latest/subgroups.html); Microsoft, [HLSL Shader Model 6.6 specification](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/HLSL_ShaderModel6_6.md).  
[2] TressFX reference [simulation source](https://github.com/thomthehound/TressFX/blob/master/src/TressFX/TressFXSimulation.cpp), [simulation kernels](https://github.com/thomthehound/TressFX/blob/master/src/Shaders/TressFXSimulation.hlsl), [SDF collision](https://github.com/thomthehound/TressFX/blob/master/src/TressFX/TressFXSDFCollision.cpp), and [MIT license](https://github.com/thomthehound/TressFX/blob/master/license.txt); AMD [4.1 release](https://github.com/GPUOpen-Effects/TressFX/releases/tag/v4.1.0).  
[3] Epic Games, [Hair Physics in Unreal Engine](https://dev.epicgames.com/documentation/unreal-engine/hair-physics-in-unreal-engine---overview) and [Hair Rendering and Simulation](https://dev.epicgames.com/documentation/unreal-engine/hair-rendering-and-simulation-in-unreal-engine).  
[4] Unity Demo Team, [Hair package](https://github.com/Unity-Technologies/com.unity.demoteam.hair), [CHANGELOG](https://github.com/Unity-Technologies/com.unity.demoteam.hair/blob/master/CHANGELOG.md), [package feature overview](https://openupm.com/packages/com.unity.demoteam.hair/).  
[5] AMD GPUOpen, [TressFX 5.0 for Unreal Engine 5.4](https://gpuopen.com/learn/tressfx-5-0-unreal-engine-5-update/), 4 August 2025.  
[6] Ismail Ivanov, [TressFX-Godot](https://github.com/ismailivanov/TressFX-Godot), especially [README limitations](https://github.com/ismailivanov/TressFX-Godot/blob/main/README.md) and [assets](https://github.com/ismailivanov/TressFX-Godot/blob/main/docs/Assets.md), 2026.  
[7] Bhokare et al., [Real-Time Hair Rendering with Hair Meshes](https://doi.org/10.1145/3641519.3657521), SIGGRAPH 2024; [author interview](https://blog.siggraph.org/2024/12/hair-raising-innovation.html).  
[8] Hsu et al., *Real-time Physically Guided Hair Interpolation*, SIGGRAPH 2024, [conference abstract](https://www.siggraph.org/wp-content/uploads/2024/09/tog-43-4.html).  
[9] TressFX issue [#46: multi-groom SDF constant buffer](https://github.com/GPUOpen-Effects/TressFX/issues/46) and [#47: wind issues](https://github.com/GPUOpen-Effects/TressFX/issues/47).  
[10] Macklin et al., [XPBD: Position-Based Simulation of Compliant Constrained Dynamics](https://doi.org/10.1145/2994258.2994272), MiG 2016; [author explanation](https://blog.mmacklin.com/2016/09/15/xpbd/).  
[11] Umetani et al., [Position-based Elastic Rods](https://www.research.autodesk.com/publications/position-based-elastic-rods/), SCA 2014.  
[12] NVIDIA, [LSS hair-rendering hardware and trade-offs](https://developer.nvidia.com/blog/render-path-traced-hair-in-real-time-with-nvidia-geforce-rtx-50-series-gpus/), February 2025.  
[13] Blender, [Hair curves Alembic/USD support in 4.2](https://developer.blender.org/docs/release_notes/4.2/pipeline_assets_io/).  
[14] Blender, [USD exporter documentation and hair attribute limitations](https://docs.blender.org/manual/en/5.4/files/import_export/usd.html).  
[15] AMD GPUOpen, [FidelityFX Parallel Sort](https://gpuopen.com/fidelityfx-parallel-sort/) as a tested optional GPU primitive for future spatial sort/grid workloads; evaluate actual overhead before adding it.