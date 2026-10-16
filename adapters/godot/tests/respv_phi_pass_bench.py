# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
"""Benchmark *only* the final Phi pass, including Phi-dense synthetic shaders.

The same original re-SPIRV tree, same compiler, same optimizer input, and
identical retained output are required for a meaningful pass-cost comparison.
"""
import argparse
import json
import statistics
import subprocess
from pathlib import Path


def run(*args):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(" ".join(str(a) for a in args) + "\n" + result.stderr[-2500:])
    return result.stdout


def build_phi_flood(work, variable_count):
    """One dynamic branch and many independent merge Phi operands."""
    shader = work / f"phi_flood_{variable_count}.comp"
    spv = work / f"phi_flood_{variable_count}.spv"
    lines = [
        "#version 460",
        "layout(local_size_x=1) in;",
        "layout(set=0,binding=0,std430) readonly buffer Inputs {float values[];} inputs;",
        "layout(set=0,binding=1,std430) writeonly buffer Outputs {float values[];} outputs;",
        "void main() {",
        "uint idx = gl_GlobalInvocationID.x;",
    ]
    for i in range(variable_count):
        lines.append(f"float a{i} = inputs.values[{i}];")
    lines.append("if ((idx & 1u) == 0u) {")
    for i in range(variable_count):
        lines.append(f"a{i} += float(idx) + {float(i + 1):.1f};")
    lines.append("} else {")
    for i in range(variable_count):
        lines.append(f"a{i} -= float(idx) + {float(i + 1):.1f};")
    lines.append("}")
    lines.append("float total = 0.0;")
    for i in range(variable_count):
        lines.append(f"total += a{i};")
    lines.extend(("outputs.values[idx] = total;", "}"))
    shader.write_text("\n".join(lines) + "\n")
    run("glslc", "-O", "--target-env=vulkan1.2", "-fshader-stage=compute", shader, "-o", spv)
    run("spirv-val", "--target-env", "vulkan1.2", spv)
    return spv


def compile_bench(upstream, source, benchmark, dest):
    dest.parent.mkdir(exist_ok=True, parents=True)
    run("c++", "-std=c++17", "-O2", "-DNDEBUG",
        "-I" + str(upstream), "-I" + str(upstream / "external/SPIRV-Headers/include"),
        source, benchmark, "-o", dest)


def check_equivalent(work, shader):
    outputs = []
    for name in ("baseline", "no-final-phi", "ordered-phi", "ordered-no-final"):
        file = work / "pass-cost" / "outputs" / name / shader.name
        file.parent.mkdir(parents=True, exist_ok=True)
        run(work / "bin" / name, shader, file)
        run("spirv-val", "--target-env", "vulkan1.2", file)
        outputs.append(file.read_bytes())
    if any(blob != outputs[0] for blob in outputs[1:]):
        raise RuntimeError("Phi pass or scheduling variant altered output: " + shader.name)


def sample(bench, shader, count=550, include_parse=False):
    # Alternating order per variant is less important than reporting wide
    # noise intervals; avoid interpreting sub-microsecond noise as victory.
    args = [bench, shader, count]
    if include_parse:
        args.append("parse-run")
    return float(run(*args).strip())


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--upstream", type=Path, required=True)
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--benchmark", type=Path, required=True)
    p.add_argument("--loxfx", nargs=2, type=Path, required=True)
    opts = p.parse_args()
    upstream = opts.upstream.resolve()
    work = opts.work.resolve()
    binaries = {}
    for name in ("baseline", "no-final-phi", "ordered-phi", "ordered-no-final"):
        dest = work / "pass-cost" / "bin" / name
        compile_bench(upstream, work / "sources" / name / "re-spirv.cpp",
                      opts.benchmark.resolve(), dest)
        binaries[name] = dest
    flood_dir = work / "pass-cost" / "shaders"
    flood_dir.mkdir(parents=True, exist_ok=True)
    inputs = [build_phi_flood(flood_dir, n) for n in (4, 16, 64, 128)]
    inputs += [x.resolve() for x in opts.loxfx]
    summary = []
    for shader in inputs:
        diss = run("spirv-dis", "--raw-id", shader)
        count = diss.count("OpPhi ")
        check_equivalent(work, shader)
        trials = []
        for trial in range(5):
            # Swap order to lessen effects of noisy shared CI runners.
            modes = ("baseline", "no-final-phi", "ordered-phi", "ordered-no-final") if trial % 2 == 0 else ("ordered-no-final", "ordered-phi", "no-final-phi", "baseline")
            result = {}
            for mode in modes:
                result[mode] = {
                    "cached_opt_us": sample(binaries[mode], shader),
                    "parse_opt_us": sample(binaries[mode], shader, 250, True),
                }
            trials.append(result)
        medians = {
            mode: {
                key: round(statistics.median(t[mode][key] for t in trials), 4)
                for key in ("cached_opt_us", "parse_opt_us")
            }
            for mode in ("baseline", "no-final-phi", "ordered-phi", "ordered-no-final")
        }
        row = {
            "shader": shader.name, "phi_count": count,
            "medians": medians,
            "reduction_cached_pct": round(100*(1-medians["no-final-phi"]["cached_opt_us"] / medians["baseline"]["cached_opt_us"]), 2),
            "reduction_full_pct": round(100*(1-medians["no-final-phi"]["parse_opt_us"] / medians["baseline"]["parse_opt_us"]), 2),
            "outputs_identical": True,
        }
        summary.append(row)
        print("PHI PASS BENCH:", json.dumps(row), flush=True)
    (work / "pass-cost" / "results.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
