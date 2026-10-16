# re-SPIRV optimizer investigation — working record

Status: **research only; PR #31 (draft), stacked on PR #30**  
Updated: **2026-10-16**

This record is maintained alongside the executable investigations so that evidence, performance data, open hypotheses, and acceptance criteria survive across development sessions. The draft branch does not replace the production re-SPIRV dependency or alter runtime shader paths.

## Objective and non-negotiable constraints

Repair the re-SPIRV optimizer's CFG, SSA, and liveness defects **without sacrificing its lightweight, fast specialization path**. Correct output is necessary but not sufficient: performance is part of acceptance for every optimizer change, including fixes that appear to simplify code.

Measure two separately relevant workloads:

- **Optimizer-only:** reuse a parsed shader and execute specialization/optimization, the hot path when a module is specialized repeatedly.
- **Parse + optimize:** include parsing and dependency/sort construction, the relevant path when modules are loaded afresh.

Always compare like-for-like compiler configuration, validated inputs, equivalent specialization sets, and identical output semantics. Record absolute times and relative effects rather than percentage-only claims. Check correctness before accepting timing results; tests on known-invalid baseline outputs may diagnose cost but cannot establish semantic equivalence.

Use repeated, order-balanced samples. Include real LoxFX shaders, tiny dominance-heavy programs, nested/switch/loop cases, and Phi-dense scaling. Watch memory allocation, cache lifetime, and one-time parse costs; do not solve a rare CFG case with unconditional preprocessing. A slower path requires an explicit measured tradeoff and optimization work, not an unmeasured assumption that overhead is negligible.

## Findings confirmed by reproductions

1. **Historical final Phi safeguard had a real purpose.** The 2024-08-07 optimizer at revision `3c1c0db2ca23` produces invalid Phi predecessor sets and/or dangling IDs on validated nested/pruned selection fixtures when the last `optimizerCompactPhis()` pass is suppressed. Retaining the pass repairs those outputs. The maintainer's original shader is not available; these are independent reproducers of the same failure category. [Historical differential CI](https://github.com/thomthehound/LoxFX/actions/runs/38949074616).
2. **The reconstructed historical defect disappears at the 2024-08-07 predecessor-label dependency revision** `3961eeec292a` (*Add edges to OpPhi*). The immediate previous revision fails without the pass; that revision and six later pinned versions produce valid, byte-identical results with/without it for both historical test cases. The era script now *asserts* both sides of that boundary rather than just printing outcomes. [Enforced boundary CI](https://github.com/thomthehound/LoxFX/actions/runs/38955658361).
3. **The current Phi safeguard appears redundant on the tested corpus, not proven redundant in general.** A modern 140-case valid-input experiment found zero mutations in the last Phi pass, and removing it did not alter output. A 33-input unpatched/patched-loop-liveness 2×2 comparison found zero pass-dependent differences among accepted cases; five inputs were unsupported in both branches. The predecessor-label fix precedes PR #30, so the loop-liveness patch does not explain the historical improvement.
4. **A separate silent semantic bug exists in load/store forwarding.** `optimizerDoesInstructionDominate()` mistakes DFS ancestry intervals for full CFG dominance. In `right_store.spv`, a conditional store of 11 is forwarded over a merge load that can still see the initialized value 7. SPIR-V validation does not detect this behavioral error. A DFS-negative prefilter followed by exact CFG reachability, with per-optimizer-run caching, avoids unconditional CFG preprocessing and repairs the known miscompile. [Dominance audit/bench CI](https://github.com/thomthehound/LoxFX/actions/runs/38945024130).
5. **The existing loop-Phi fix and the cleanup are distinct.** PR #30 counts SSA uses on excluded Phi backedges for DCE without creating cycles in the instruction topological sort. A predecessor terminator can still be scheduled after a dependent Phi in a validated stress fixture. An explicit terminator→Phi edge prevents that ordering in the fixture, but no corresponding modern output corruption has yet been established.
6. **Executable semantic tests extend beyond validation.** Original input, last-Phi-pass retained, and pass-suppressed shaders were executed through Lavapipe. Across eight shader families, three compile modes, optional function inlining and four specialization sets: all 128 optimizer configurations were accepted; 122 had valid outputs, and 244 GPU executions agreed with the original shaders' eight integer results. Final Phi cleanup caused zero binary differences. Six `-O0` switch configurations exposed a separate deleted-selector ID failure **in both** variants. [Semantic CI](https://github.com/thomthehound/LoxFX/actions/runs/38954765416).
7. **Switch lowering has a validated research repair.** When a specialized `OpSwitch` has no remaining alternative case edges, reducing it to unconditional `OpBranch` (and removing the inapplicable `OpSelectionMerge`) eliminates the synthetic selector lifetime. A 16-configuration experiment repaired all six previous invalid switch outputs, left zero invalid cases, and matched original GPU outputs. This source-only experiment is not production code. Its first performance campaign shows modest optimizer regressions on several small workloads; see the benchmark section below. [Switch experiment CI](https://github.com/thomthehound/LoxFX/actions/runs/38954765416).
8. **The Godot renderer-owned GPU proof still fails in the normal mode.** Vulkan rejects modules with undefined forward-referenced IDs (e.g., 123), while the debug-mode control executes the original LoxFX shaders. Other validation errors arise from debug metadata in the diagnostic control. Do not attribute all of these errors to re-SPIRV without isolating the compiler transformation. [Renderer proof CI](https://github.com/thomthehound/LoxFX/actions/runs/38955658602).

## Measured optimizer costs and benefits

The comparison below is from the combined **cached selective exact-dominance fix + omitted final Phi pass** versus the existing optimizer with PR #30's loop correction. Medians in microseconds from one GitHub Actions Ubuntu runner; five repeated samples per variant. These are microbenchmarks, not application-wide speedups. [Combined benchmark CI](https://github.com/thomthehound/LoxFX/actions/runs/38947135815).

| Workload | Baseline optimize | Combined optimize | Optimizer gain | Baseline parse + optimize | Combined parse + optimize | Total gain |
|---|---:|---:|---:|---:|---:|---:|
| Phi flood: 16 | 7.90 | 7.06 | 10.6% | 25.16 | 23.93 | 4.9% |
| Phi flood: 64 | 49.74 | 35.66 | 28.3% | 106.52 | 94.46 | 11.3% |
| Phi flood: 128 | 141.35 | 93.43 | 33.9% | 253.11 | 202.89 | 19.8% |
| LoxFX integration | 4.25 | 4.15 | 2.4% | 19.11 | 18.60 | 2.6% |
| LoxFX follower/tangent | 4.56 | 4.47 | 1.9% | 20.21 | 19.87 | 1.7% |

Regressions matter: small positive-dominance fixtures had roughly **15–18% slower optimization** and **2–3% slower parse + optimize** with the combined variant, despite very small absolute latency. A naive exact reachability traversal costs far more on many-load programs; unconditional dominator preprocessing increased LoxFX parse+run by about 14–16% in the earlier controlled comparison. The cached filtered approach better preserves the low-overhead fast path. [Six-way dominance benchmark](https://github.com/thomthehound/LoxFX/actions/runs/38945024130).

The switch-to-branch rewrite has not yet been benchmarked against the original switch reducer. Fewer output instructions or fewer source branches do **not**, alone, establish a faster optimizer. Explicitly time both variants on specialized switches and unaffected shaders before considering the change complete.

## Switch rewrite: initial optimizer performance (2026-10-16)

The specialization-aware differential microbenchmark now runs in CI after GPU semantic validation, across both affected switch shapes and unaffected controls. Five order-balanced trials per variant; each trial uses seven timed batches. Results below compare the original switch reducer with the experimental direct `OpBranch` lowering. All times are medians in microseconds per invocation on a shared GitHub Ubuntu runner. **Positive changes mean the experimental variant was slower.** [Vulkan + performance CI](https://github.com/thomthehound/LoxFX/actions/runs/38957606990).

| Workload | Original optimize (µs) | Direct branch optimize (µs) | Change | Original parse + optimize (µs) | Direct branch parse + optimize (µs) | Change |
|---|---:|---:|---:|---:|---:|---:|
| Switch O0, pruned case* | 2.2313 | 2.2730 | +1.87% | 9.2444 | 9.3159 | +0.77% |
| Switch O0, pruned case, inlined* | 2.4946 | 2.5235 | +1.16% | 13.0917 | 13.0988 | +0.05% |
| Switch O0, valid baseline | 2.2612 | 2.2975 | +1.61% | 9.2198 | 9.3319 | +1.22% |
| Switch -O | 1.9098 | 1.9095 | -0.02% | 7.2679 | 7.3022 | +0.47% |
| Switch -Os | 1.9220 | 1.9231 | +0.06% | 7.3181 | 7.3358 | +0.24% |
| Switch-in-loop O0, inlined | 2.4303 | 2.5036 | +3.02% | 13.4222 | 13.4532 | +0.23% |
| Branch -O control | 1.7300 | 1.7451 | +0.87% | 6.8439 | 6.9048 | +0.89% |
| Phi chains -O control | 2.2815 | 2.3049 | +1.03% | 9.0166 | 8.9678 | -0.54% |
| Nested O0 inlined control | 3.1391 | 3.1466 | +0.24% | 15.8747 | 15.9066 | +0.20% |

*The original optimizer emits invalid SPIR-V for these cases; their baseline timings are diagnostic only, not comparisons between two semantically correct implementations.*

**Disposition:** The rewrite restores valid SPIR-V for all six known invalid switch cases, but the first performance sample does **not** establish a free or faster optimization. The ~0.073 µs (+3.02%) cached-opt regression in switch-in-loop warrants replication and profiling; smaller shifts also appear on unaffected controls, so avoid overinterpreting one shared runner. Keep this as an experiment, compare an alternative that repairs the original switch-selector liveness accounting without unconditional extra work, and measure on LoxFX's real modules before accepting it. The significant measured gain from omitting the final Phi pass is a separate workload and cannot simply be subtracted from these switch deltas.

## Unresolved questions and next experiments

1. **Prove or falsify the modern Phi scheduling invariant.** Construct validated shaders with a predecessor terminator scheduled after a dependent `OpPhi`; exercise pruned switch edges, duplicate targets, nested selections, loops, backedges, continue/merge combinations, Phi chains, multi-function and inlining. Instrument final-pass mutations and compare generated CFG/operands with the actual predecessor sets. For valid cases, compare GPU results, not only `spirv-val`.
2. **Resolve switch-lowering performance.** Repeat the nine-case specialization-aware benchmark, examine the +3.02% optimizer-only switch-in-loop regression, and compare alternate selector-liveness fixes against direct-branch lowering. Benchmark actual LoxFX shaders; the current nine-case workload lacks them. Keep optimization time, parse+run, output shader size and GPU dispatch performance as distinct metrics. Avoid unconditional setup for rare switch reductions.
3. **Resolve exact dominance rigorously and cheaply.** Verify the filtered reachability cache on switches, nested loops, unreachable blocks, inlining, and multiple functions. Benchmark worst-case positive candidates, where exact checks dominate cost. Investigate cheaper cache keys or algorithms if performance regresses materially.
4. **Isolate the normal-mode Godot shader invalidity.** Retain the pre/post transformations and validate at each stage; compare stage outputs without assuming this is identical to either the 2024 Phi bug or the independent switch-selector case.
5. **Before removing the Phi safeguard:** require adversarial corpus coverage, verified semantic equivalence, zero unexplained final-pass mutations, and benchmarked latency on representative and worst-case workloads. Prefer a simpler single-pass invariant over replacing a redundant full pass with another unconditional expensive analysis. Re-evaluate for upstream submission only with a small documented reproducer and measurements.

## Experiment and release boundaries

- [PR #30](https://github.com/thomthehound/LoxFX/pull/30): independently developed loop Phi backedge DCE/liveness correction; PR #31 is stacked on it.
- [PR #31](https://github.com/thomthehound/LoxFX/pull/31): diagnostic source variants, adversarial fixtures, historical tests, standalone Vulkan oracle, and microbenchmarks. **Draft; no release decision.**
- [Phi audit workflow](../../../.github/workflows/respv-phi-order-audit.yml), [dominance audit workflow](../../../.github/workflows/respv-dominance-audit.yml), [semantic campaign workflow](../../../.github/workflows/respv-semantic-campaign.yml).
- Each follow-up experiment should append results with reproducible workflow links, workload dimensions, baseline/variant absolute timings, semantic outcomes, and the remaining decision. Do not silently replace historical results with a newer run or combine incompatible measurement baselines.
