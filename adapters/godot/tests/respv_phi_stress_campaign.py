# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
"""Deterministic structured-CFG stress against re-SPIRV's final Phi safeguard.

Compiles valid, distinct GLSL control-flow families; requires agreement with
the original Vulkan shader for both optimizer variants and reports Phi counts.
Inputs and outputs are preserved as CI artifacts for differential diagnosis.
"""
import argparse
import json
from pathlib import Path

from respv_semantic_campaign import HEADER, TAIL, SPECS, checked, lavapipe_env, run

SHAPES = {
    "sibling_merges": """void run_case(inout int x, int j) {
        if ((j & 1) != 0) {
            if (A) x += 7; else x -= 2;
            if ((j & 3) == 1) x = x * 2; else x += 5;
        } else {
            if (B) x += 10; else x -= 3;
            if ((j % 3) == 0) x += 17; else x -= 4;
        }
        if ((j & 2) != 0) x ^= MODE; else x += 3;
    }""",
    "return_nested": """int helper(int x, int j) {
        if ((j & 1) != 0) {
            if (B) return x - 7;
            if (j == MODE) return x + 19;
        }
        if (A) x += 11; else x -= 2;
        return x;
    }
    void run_case(inout int x, int j) {
        if ((j & 3) == 0) x = helper(x,j);
        else if (A) x = helper(x + 1,j + 1);
        else x = helper(x - 3,j + 2);
        if (B) x += MODE;
    }""",
    "continue_break": """void run_case(inout int x, int j) {
        for (int i=0;i<COUNT+1;i++) {
            if ((i+j) % 3 == 0) {
                if (A && i != MODE) {x += 4; continue;}
                x -= 8;
            } else {
                if (B && i > 1) break;
                x += i + j;
            }
            if ((i&1) != 0) x = x*2; else x ^= 3;
        }
    }""",
    "switch_siblings": """void run_case(inout int x, int j) {
        if (A) {
            switch ((j + MODE) % 4) {
                case 0: x += 3; break;
                case 1: x -= 5; break;
                case 2: x ^= 7; break;
                default: x += 2; break;
            }
        } else {
            if (j % 2 == 0) x -= 11; else x += 6;
        }
        if (B && j > 2) x *= 2; else x += j;
    }""",
    "switch_fallthrough": """void run_case(inout int x, int j) {
        switch ((j+MODE)&3) {
            case 0:
            case 2: if (A) x += 9; else x -= 9; break;
            case 1: if (B) x *= 2; else x += 1; break;
            default: x ^= 13; break;
        }
        if ((j&1)!=0) x += 3; else x -= 2;
    }""",
    "two_loops": """void run_case(inout int x, int j) {
        for (int i=0;i<COUNT;i++) {
            if ((i+j)&1) x += 2+i; else x -= 1;
            for (int k=0;k<2;k++) {
                if (A && (k+j+i)%3 == 0) x += 4;
                else if (B) x -= 5;
                else x ^= k + 1;
            }
        }
    }""",
    "multiple_helpers": """int f(int a, int j) {
        if ((j&1)==0) return A ? a+3 : a-2;
        if (B) return a*2;
        return a+j;
    }
    int g(int a, int j) {
        if ((j%3)==0) a=f(a,j);
        else if ((j%3)==1) a=f(a+1,j+1);
        else a=f(a-1,j+2);
        return a;
    }
    void run_case(inout int x, int j) {
        for(int i=0;i<COUNT;i++) x=g(x,j+i);
        if(A != B) x=g(x,j);
    }""",
    "loop_switch_merge": """void run_case(inout int x, int j) {
        int k=0;
        while(k<COUNT) {
            switch ((j+k+MODE)&3) {
                case 0: if(A) x += 5; else x -= 3; break;
                case 1: if(B) x *= 2; else x += 4; break;
                case 2: if ((j&1)!=0) x ^= 7; else x -= 2; break;
                default: x += k+1; break;
            }
            if ((j+k)&1) x += 1; else x -= 1;
            k++;
        }
    }""",
}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--inputs", type=Path, required=True)
    p.add_argument("--variants", type=Path, required=True)
    p.add_argument("--oracle", type=Path, required=True)
    p.add_argument("--work", type=Path, required=True)
    a = p.parse_args()
    a.work.mkdir(parents=True, exist_ok=True)
    inputs = a.work / "inputs"
    inputs.mkdir(exist_ok=True)
    results = []
    counts = {"attempted": 0, "optimized": 0, "gpu_compared": 0,
              "pass_differences": 0, "invalid": 0, "unsupported": 0}
    binaries = {name: a.variants.resolve() / name / "probe"
                for name in ("with-final", "without-final")}
    for family, body in SHAPES.items():
        shader_src = inputs / (family + ".comp")
        shader_src.write_text(HEADER + body + TAIL)
        for mode in ("O0", "O", "Os"):
            raw = inputs / (family + "." + mode + ".spv")
            checked(["glslc", "--target-env=vulkan1.2",
                     "-fshader-stage=compute", "-" + mode, shader_src,
                     "-o", raw], "compile stress input")
            if mode == "O0":
                stripped = inputs / (family + ".O0-stripped.spv")
                checked(["spirv-opt", "--strip-debug", raw, "-o", stripped],
                        "strip stress input debug")
                raw = stripped
            checked(["spirv-val", "--target-env", "vulkan1.2", raw],
                    "validate stress input")
            dis = checked(["spirv-dis", "--raw-id", raw], "disassemble input")
            phi_count = dis.count("OpPhi ")
            for inline in ((0, 1) if mode == "O0" else (0,)):
                for spec in SPECS:
                    counts["attempted"] += 1
                    overrides = [str(v) for k, value in sorted(spec.items())
                                 for v in (k, value)]
                    record = {"family": family, "mode": mode,
                              "inline": inline, "spec": spec,
                              "input_phi_count": phi_count, "variants": {}}
                    outputs = {}
                    for name, exe in binaries.items():
                        output = a.work / "outputs" / name / (
                            family + "." + mode + "." + str(inline) +
                            "." + str(spec[2]) + "-" + str(spec[3]) + ".spv")
                        output.parent.mkdir(parents=True, exist_ok=True)
                        run_result = run([exe, raw, output, str(inline), *overrides])
                        record["variants"][name] = {
                            "accepted": run_result.returncode == 0,
                            "error": run_result.stderr[-500:]}
                        if run_result.returncode == 0:
                            validation = run(
                                ["spirv-val", "--target-env", "vulkan1.2", output])
                            record["variants"][name]["valid"] = validation.returncode == 0
                            record["variants"][name]["validation_error"] = validation.stderr[-600:]
                            outputs[name] = output
                    if any(not x["accepted"] for x in record["variants"].values()):
                        counts["unsupported"] += 1
                        # A pass-dependent acceptance failure is significant.
                        if len(outputs) == 1:
                            raise RuntimeError("Phi cleanup changes optimizer acceptance: " + str(record))
                        results.append(record)
                        continue
                    counts["optimized"] += 1
                    if not all(x["valid"] for x in record["variants"].values()):
                        counts["invalid"] += 1
                        raise RuntimeError("Phi stress produced invalid SPIR-V: " + str(record))
                    if outputs["with-final"].read_bytes() != outputs["without-final"].read_bytes():
                        counts["pass_differences"] += 1
                        raise RuntimeError("Phi cleanup changed output: " + str(record))
                    expected = checked([a.oracle.resolve(), raw, *overrides],
                                       "original GPU", lavapipe_env()).strip()
                    optimized = checked([a.oracle.resolve(), outputs["without-final"]],
                                        "optimized GPU", lavapipe_env()).strip()
                    if optimized != expected:
                        raise RuntimeError("Phi cleanup shader differs from input GPU: "
                                           + str(record) + " expected " + expected
                                           + " actual " + optimized)
                    counts["gpu_compared"] += 1
                    results.append(record)
        print("PHI_STRESS_FAMILY:", family, flush=True)
    (a.work / "report.json").write_text(
        json.dumps({"summary": counts, "records": results}, indent=2) + "\n")
    print("PHI_STRESS_SUMMARY:", json.dumps(counts), flush=True)
    if counts["attempted"] != len(SHAPES) * len(SPECS) * 4:
        raise RuntimeError("CFG stress matrix was not fully attempted")
    if counts["optimized"] < 80 or counts["gpu_compared"] < 80:
        raise RuntimeError("CFG stress campaign did not reach meaningful coverage")


if __name__ == "__main__":
    main()
