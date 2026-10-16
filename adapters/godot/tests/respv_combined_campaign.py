# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
"""Research-only re-SPIRV combined correctness and performance comparison.

All variants share the loop-Phi liveness correction. The stock variant is
included for performance diagnostics only; correctness comparison variants
share the switch-selector repair and differ only in dominance/Phi handling.
The exact reachability variant is the reference for binary equality.
"""
from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path

from respv_semantic_campaign import HEADER, TAIL, SPECS, SHAPES, checked, run, lavapipe_env
from respv_phi_stress_campaign import SHAPES as ADVERSARIAL
from respv_phi_pass_bench import build_phi_flood
from respv_switch_selector_audit import (keep_original_selector,
    allow_switch_without_integer_constant, remove_unused_switch_constant_scan)
from respv_phi_conditional_cleanup import gated_sources


def build(upstream, evidence, probe_source, bench_source, plain_bench_source):
    cpp = {}
    for variant in ("exact_reachability", "structured_corridor"):
        source = evidence / "variants" / variant / "re-spirv.cpp"
        if not source.exists():
            raise RuntimeError("Required independent dominance research variant absent: " + str(source))
        cpp[variant] = allow_switch_without_integer_constant(
            keep_original_selector(source.read_text()))
    final_pass = """        if (!optimizerCompactPhis(context)) {
            return false;
        }"""
    structured = cpp["structured_corridor"]
    if structured.count(final_pass) != 1:
        raise RuntimeError("Unexpected historical Phi cleanup hook count")
    gated, gated_header = gated_sources(structured, (upstream / "re-spirv.h").read_text())
    selective, selective_header = gated_sources(
        structured, (upstream / "re-spirv.h").read_text(), minimum_phi_count=8)
    variants = {
        "stock": ((upstream / "re-spirv.cpp").read_text(), None),
        "exact": (cpp["exact_reachability"], None),
        "structured": (structured, None),
        "gated": (gated, gated_header),
        "selective": (selective, selective_header),
        "selective_no_scan": (remove_unused_switch_constant_scan(selective), selective_header),
        "without_final": (structured.replace(final_pass, """        if (false && !optimizerCompactPhis(context)) {
            return false;
        }"""), None),
    }
    tools = {}
    for name, (source, header) in variants.items():
        folder = evidence / "combined" / "variants" / name
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "re-spirv.cpp").write_text(source)
        if header is not None:
            (folder / "re-spirv.h").write_text(header)
        includes = ["-I" + str(folder), "-I" + str(upstream),
                    "-I" + str(upstream / "external/SPIRV-Headers/include")]
        binaries = {}
        for key, driver in (("probe", probe_source), ("bench", bench_source),
                            ("plain_bench", plain_bench_source)):
            exe = folder / key
            checked(["c++", "-std=c++17", "-O2", "-DNDEBUG", *includes,
                     folder / "re-spirv.cpp", driver, "-o", exe],
                    "compile combined " + name + "/" + key)
            binaries[key] = exe
        tools[name] = binaries
    return tools


def median(exe, shader, inline, spec, with_parse, plain=False):
    args = [exe, shader, "300" if with_parse else "500"]
    if not plain:
        args.append(str(inline))
        args.extend(str(spec[k]) for k in range(4))
    if with_parse:
        args.append("parse-run")
    return float(checked(args, "combined benchmark").strip())


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--upstream", type=Path, required=True)
    p.add_argument("--dominance-evidence", type=Path, required=True)
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--probe", type=Path, required=True)
    p.add_argument("--bench", type=Path, required=True)
    p.add_argument("--plain-bench", type=Path, required=True)
    p.add_argument("--oracle", type=Path, required=True)
    p.add_argument("--loxfx", type=Path, nargs=2, required=True)
    args = p.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    tools = build(args.upstream.resolve(), args.dominance_evidence.resolve(),
                  args.probe.resolve(), args.bench.resolve(), args.plain_bench.resolve())
    inputs = work / "inputs"
    inputs.mkdir(parents=True, exist_ok=True)
    outputs = work / "outputs"
    driver = work / "vulkan_oracle"
    checked(["c++", "-std=c++17", "-O2", args.oracle.resolve(), "-lvulkan", "-o", driver],
            "compile Vulkan oracle")
    shapes = {**ADVERSARIAL,
              "dynamic_branches": SHAPES["branch"],
              "nested_switch": SHAPES["switch"],
              "early_returns": SHAPES["early_return"],
              "function_chains": SHAPES["functions"]}
    records = []
    checks = {"cases": 0, "valid_outputs": 0, "binary_equal": 0,
              "vulkan_equal": 0, "no_input_failures": 0}
    for family, body in shapes.items():
        glsl = inputs / (family + ".comp")
        glsl.write_text(HEADER + body + TAIL)
        for opt in ("O0", "O", "Os"):
            raw = inputs / (family + "." + opt + ".spv")
            checked(["glslc", "--target-env=vulkan1.2", "-fshader-stage=compute",
                     "-" + opt, glsl, "-o", raw], "compile " + family)
            if opt == "O0":
                stripped = inputs / (family + ".O0-stripped.spv")
                checked(["spirv-opt", "--strip-debug", raw, "-o", stripped],
                        "strip debug")
                raw = stripped
            checked(["spirv-val", "--target-env", "vulkan1.2", raw], "validate input")
            for inline in ((0, 1) if opt == "O0" else (0,)):
                for spec in SPECS:
                    checks["cases"] += 1
                    override = [str(v) for k, val in sorted(spec.items()) for v in (k, val)]
                    case = {"family": family, "opt": opt, "inline": inline, "spec": spec}
                    results = {}
                    for name, programs in tools.items():
                        if name == "stock":
                            continue  # Stock has known invalid O0 switch output.
                        dest = outputs / name / (
                            family + "." + opt + "." + str(inline) + "." +
                            str(spec[2]) + "." + str(spec[3]) + ".spv")
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        checked([programs["probe"], raw, dest, str(inline), *override],
                                "optimize " + name + " " + str(case))
                        checked(["spirv-val", "--target-env", "vulkan1.2", dest],
                                "validate " + name + " " + str(case))
                        results[name] = dest
                    base = results["exact"].read_bytes()
                    if any(path.read_bytes() != base for path in results.values()):
                        raise RuntimeError("Variant output diverged from exact: " + str(case))
                    checks["valid_outputs"] += len(results)
                    checks["binary_equal"] += 1
                    expected = checked([driver, raw, *override],
                                       "original GPU", lavapipe_env()).strip()
                    actual = checked([driver, results["gated"]],
                                     "combined candidate GPU", lavapipe_env()).strip()
                    if actual != expected:
                        raise RuntimeError("GPU mismatch: " + str(case) +
                                           " original=" + expected + " changed=" + actual)
                    checks["vulkan_equal"] += 1
                    records.append(case)
        print("COMBINED_SEMANTIC_FAMILY:", family, flush=True)

    bench_inputs = [
        ("loxfx_integration", args.loxfx[0].resolve(), 0, SPECS[0], True),
        ("loxfx_follower", args.loxfx[1].resolve(), 0, SPECS[0], True),
        ("switch_o0", inputs / "nested_switch.O0-stripped.spv", 0, SPECS[0], False),
        ("switch_o0_inlined", inputs / "nested_switch.O0-stripped.spv", 1, SPECS[2], False),
        ("nested_join", inputs / "sibling_merges.O0-stripped.spv", 1, SPECS[3], False),
        ("nested_loop", inputs / "two_loops.O.spv", 0, SPECS[1], False),
        ("early_return", inputs / "early_returns.O0-stripped.spv", 1, SPECS[0], False),
    ]
    flood = inputs / "phi_flood"
    flood.mkdir(parents=True, exist_ok=True)
    for phi_count in (16, 64, 128):
        bench_inputs.append(("phi_flood_" + str(phi_count),
                             build_phi_flood(flood, phi_count), 0, SPECS[0], True))
    bench_results = []
    for label, shader, inline, spec, plain in bench_inputs:
        trials = {name: {"cached": [], "parse_run": []} for name in tools}
        for round_ in range(5):
            names = list(tools)
            if round_ % 2:
                names.reverse()
            modes = ("cached", "parse_run") if round_ % 2 == 0 else ("parse_run", "cached")
            for mode in modes:
                for name in names:
                    trials[name][mode].append(
                        median(tools[name]["plain_bench" if plain else "bench"],
                               shader, inline, spec, mode == "parse_run", plain))
        times = {name: {mode: round(statistics.median(values), 4)
                        for mode, values in ms.items()}
                 for name, ms in trials.items()}
        row = {"case": label, "times_us": times, "trials": trials,
               "stock_output_known_invalid": label.startswith("switch_o0")}
        bench_results.append(row)
        print("COMBINED_PERFORMANCE:", json.dumps({
            "case": label, "times_us": times}), flush=True)
    summary = {
        "checks": checks, "families": len(shapes), "semantic_cases": records,
        "bench": bench_results,
    }
    (work / "report.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("COMBINED_SUMMARY:", json.dumps({**checks, "families": len(shapes),
                                           "benchmarks": len(bench_results)}), flush=True)
    if checks["cases"] != 12 * 4 * len(SPECS):
        raise RuntimeError("Expected entire specialization matrix to run")


if __name__ == "__main__":
    main()
