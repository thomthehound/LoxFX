# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
"""Executable Vulkan regression for the cross-branch store/load miscompile."""
import argparse
import json
from pathlib import Path

from respv_semantic_campaign import checked, lavapipe_env


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--oracle", type=Path, required=True)
    p.add_argument("--result", type=Path, required=True)
    a = p.parse_args()
    work = a.work.resolve()
    oracle = work / "dominance_vulkan_oracle"
    checked(["c++", "-O2", "-std=c++17", a.oracle.resolve(),
             "-lvulkan", "-o", oracle], "compile dominance Vulkan oracle")
    expected = {
        "right_store.spv": [7] + [11] * 7,
        "left_store.spv": [11] + [7] * 7,
        "entry_store.spv": [11] * 8,
    }
    variants = ("baseline", "cached_filtered_exact", "cached_without_final_phi",
                "exact_reachability", "precomputed_domtree", "adaptive_domtree", "adaptive_bitset", "structured_corridor")
    report = {}
    for shader, values in expected.items():
        source = work / "shaders" / shader
        raw = checked([oracle, source], "original Vulkan", lavapipe_env()).strip()
        want = ",".join(str(x) for x in values)
        if raw != want:
            raise RuntimeError("Original input behavior mismatch: " + shader +
                               " expected " + want + " got " + raw)
        record = {"original": raw}
        for name in variants:
            output = work / "outputs" / name / shader
            actual = checked([oracle, output], name + " Vulkan", lavapipe_env()).strip()
            record[name] = actual
            if name != "baseline" and actual != raw:
                raise RuntimeError(name + " did not preserve input semantics on " +
                                   shader + ": " + actual + " != " + raw)
        if shader == "right_store.spv" and record["baseline"] == raw:
            raise RuntimeError("Known stock optimizer semantic bug stopped reproducing")
        report[shader] = record
        print("DOMINANCE_GPU:", json.dumps({"shader": shader, "results": record}),
              flush=True)
    a.result.parent.mkdir(parents=True, exist_ok=True)
    a.result.write_text(json.dumps(report, indent=2) + "\n")
    print("DOMINANCE_GPU_SUMMARY:",
          json.dumps({"inputs": len(report), "corrected_gpu_matches": 21,
                      "baseline_right_store_diverges": True}), flush=True)


if __name__ == "__main__":
    main()
