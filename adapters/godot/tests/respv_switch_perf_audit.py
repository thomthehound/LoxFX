# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
"""Cost comparison of the original and experimental switch-to-branch optimizer.

This is a separate performance investigation: semantic validation is performed
by respv_switch_selector_audit.py before accepting this experiment.
"""
from pathlib import Path
import argparse
import json
import statistics

from respv_semantic_campaign import SPECS, checked
from respv_switch_selector_audit import (
    make_fix, keep_original_selector, remove_unused_switch_constant_scan,
    allow_switch_without_integer_constant,
)

# The first two controls exercise the known-invalid original O0 reduction.
# The rest cover unaffected and normally valid specialization workloads.
CASES = [
    ("switch_o0_pruned", "switch.O0-stripped.spv", 0, 0, True),
    ("switch_o0_pruned_inlined", "switch.O0-stripped.spv", 1, 2, True),
    ("switch_o0_valid", "switch.O0-stripped.spv", 0, 1, False),
    ("switch_o", "switch.O.spv", 0, 0, False),
    ("switch_os", "switch.Os.spv", 0, 3, False),
    ("switch_loop_o0", "switch_loop.O0-stripped.spv", 1, 3, False),
    ("branch_o", "branch.O.spv", 0, 0, False),
    ("phi_chains_o", "phi_chains.O.spv", 0, 2, False),
    ("nested_o0_inlined", "nested.O0-stripped.spv", 1, 1, False),
]


def sample(exe, shader, inline, spec, parse_every_time, count):
    args = [exe, shader, str(count), str(inline)]
    args += [str(spec[i]) for i in range(4)]
    if parse_every_time:
        args.append("parse-run")
    return float(checked(args, "switch benchmark").strip())


def median_and_mad(samples):
    median = statistics.median(samples)
    return {"median_us": round(median, 4),
            "mad_us": round(statistics.median(abs(s - median) for s in samples), 4),
            "trials_us": [round(s, 4) for s in samples]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--benchmark", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    upstream = args.upstream.resolve()
    inputs = args.input.resolve()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)

    baseline = (upstream / "re-spirv.cpp").read_text()
    variants = {"current": baseline, "switch-as-branch": make_fix(baseline),
                "keep-original-selector": keep_original_selector(baseline),
                "no-synthetic-selector-scan": remove_unused_switch_constant_scan(
                    allow_switch_without_integer_constant(keep_original_selector(baseline)))}
    executables = {}
    for name, source in variants.items():
        source_file = work / (name + ".cpp")
        source_file.write_text(source)
        exe = work / (name + ".exe")
        checked(["c++", "-std=c++17", "-O2", "-DNDEBUG",
                 "-I" + str(upstream),
                 "-I" + str(upstream / "external/SPIRV-Headers/include"),
                 source_file, args.benchmark.resolve(), "-o", exe],
                "compile switch benchmark " + name)
        executables[name] = exe

    report = []
    for title, shader_name, inline, spec_index, invalid_original in CASES:
        shader = inputs / shader_name
        if not shader.exists():
            raise RuntimeError("Missing previously compiled shader: " + str(shader))
        spec = SPECS[spec_index]
        measurements = {name: {"cached": [], "parse_run": []} for name in executables}
        for trial in range(5):
            order = list(executables)
            if trial % 2:
                order.reverse()
            for mode in ("cached", "parse_run") if trial % 2 == 0 else ("parse_run", "cached"):
                for name in order:
                    elapsed = sample(executables[name], shader, inline, spec,
                                     mode == "parse_run", 450 if mode == "cached" else 250)
                    measurements[name][mode].append(elapsed)
        data = {
            "workload": title,
            "input": shader_name,
            "inline": inline,
            "spec": spec,
            "original_semantics_known_invalid": invalid_original,
            "metrics": {},
        }
        for name in executables:
            data["metrics"][name] = {
                mode: median_and_mad(measurements[name][mode])
                for mode in ("cached", "parse_run")
            }
        data["change_pct"] = {
            variant: {
                mode: round(100 * (
                    data["metrics"][variant][mode]["median_us"] /
                    data["metrics"]["current"][mode]["median_us"] - 1
                ), 2) for mode in ("cached", "parse_run")
            } for variant in executables if variant != "current"
        }
        print("SWITCH_PERF:", json.dumps(data), flush=True)
        report.append(data)
    (work / "report.json").write_text(json.dumps({
        "baseline": "same upstream plus established loop liveness patch",
        "variants": [
            "stock switch specialization",
            "fully reduced switch to OpBranch",
            "retain original OpSwitch selector dependency",
            "retain selector and remove obsolete fallback-constant parsing",
        ],
        "units": "microseconds per operation; positive change_pct means regression",
        "trials_per_case": 5,
        "inner_batches_per_trial": 7,
        "experimental": True,
        "cases": report,
    }, indent=2) + "\n")
    print("SWITCH_PERF_SUMMARY:", json.dumps({
        "cases": len(report),
        "valid_baseline_cases": sum(not r["original_semantics_known_invalid"] for r in report),
        "invalid_baseline_cases": sum(r["original_semantics_known_invalid"] for r in report),
    }), flush=True)


if __name__ == "__main__":
    main()
