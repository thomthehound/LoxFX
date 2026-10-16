# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
"""Audit re-SPIRV's final Phi-compaction workaround and predecessor ordering.

This intentionally changes *only* local copies of upstream source on CI.
Never substitutes a modified compiler in the production LoxFX path.
"""
import argparse
import json
import re
import shutil
import subprocess
from pathlib import Path


HEADER = """#version 460
layout(local_size_x = 1) in;
layout(binding = 0, std430) buffer Values { float values[]; };
layout(push_constant) uniform Parameters { int n; float seed; };
layout(constant_id = 0) const bool A = true;
layout(constant_id = 1) const bool B = false;
void main() {
    int j = int(gl_GlobalInvocationID.x);
    float x = seed;
"""
TAIL = """
    values[j] = x;
}
"""
SHAPES = {
    "select": """
    if (A) { x += float(j) + 1.0; } else { x -= float(j) + 2.0; }
    if (B) { x *= 3.0; } else { x *= 2.0; }
""",
    "nested": """
    if (j < n) {
        if (A) { x += float(j); } else { x -= 7.0; }
    } else {
        if (B) { x *= 3.0; } else { x += 8.0; }
    }
    if (A != B) { x *= 2.0; }
""",
    "loop": """
    for (int i = 0; i < n; ++i) {
        if (A) { x += float(i); } else { x -= float(i); }
        if (i == j) { x += 2.0; } else { x += 3.0; }
    }
    if (B) { x *= 2.0; }
""",
    "diamond": """
    if (A) {
        if (B) { x += 1.0; } else { x += 2.0; }
    } else if (j < n) {
        x += 3.0;
    } else {
        x += 4.0;
    }
    if (x > 0.0) { x *= 1.5; } else { x -= 2.0; }
""",
    "switch": """
    switch (j % 3) {
        case 0: x = A ? x + 1.0 : x - 1.0; break;
        case 1: x = B ? x * 2.0 : x / 2.0; break;
        default: x += 3.0; break;
    }
    if (A) { x += 0.125; } else { x -= 0.125; }
""",
}


def exec_cmd(*cmd):
    return subprocess.run([str(x) for x in cmd], check=False, capture_output=True, text=True)


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError(f"Expected one source match ({source.count(before)}): {before[:80]!r}")
    return source.replace(before, after)


def prepare_variants(source, dest):
    original = source
    skip_tail = """        if (!optimizerCompactPhis(context)) {
            return false;
        }"""
    no_final = replace_once(original, skip_tail, """        // Audit variant: suppress the second Phi-compaction workaround.
        if (false && !optimizerCompactPhis(context)) {
            return false;
        }""")

    # A predecessor label being processed does not guarantee that the
    # predecessor's terminating branch has been simplified. The added edge
    # tests whether explicit terminator-before-Phi ordering fixes that.
    edge = """                    instructionAdjacentListIndices[labelIndex] = addToList(i, instructionAdjacentListIndices[labelIndex], listNodes);
                    instructionAdjacentListIndices[resultIndex] = addToList(i, instructionAdjacentListIndices[resultIndex], listNodes);"""
    ordered_edge = """                    const uint32_t predecessorBlock = instructions[labelIndex].blockIndex;
                    const uint32_t predecessorTerminator = blocks[predecessorBlock].terminatorInstructionIndex;
                    instructionAdjacentListIndices[predecessorTerminator] = addToList(i, instructionAdjacentListIndices[predecessorTerminator], listNodes);
""" + edge
    ordered = replace_once(original, edge, ordered_edge)
    ordered_no_final = replace_once(ordered, skip_tail, """        // Audit variant: explicit predecessor-terminator order, no workaround.
        if (false && !optimizerCompactPhis(context)) {
            return false;
        }""")

    variants = {
        "baseline": original,
        "no-final-phi": no_final,
        "ordered-phi": ordered,
        "ordered-no-final": ordered_no_final,
    }
    dest.mkdir(parents=True, exist_ok=True)
    for name, code in variants.items():
        d = dest / name
        d.mkdir(exist_ok=True)
        (d / "re-spirv.cpp").write_text(code)
    return variants


def compile_variants(source_dir, variants, bin_dir, probe):
    bin_dir.mkdir(parents=True, exist_ok=True)
    headers = source_dir / "external/SPIRV-Headers/include"
    for name in variants:
        output = bin_dir / name
        status = exec_cmd(
            "c++", "-std=c++17", "-O2", "-I" + str(source_dir),
            "-I" + str(headers), str(bin_dir.parent / "sources" / name / "re-spirv.cpp"),
            probe, "-o", output
        )
        if status.returncode:
            raise RuntimeError(name + " failed compilation: " + status.stderr[-3500:])


def create_inputs(outdir):
    outdir.mkdir(parents=True, exist_ok=True)
    shaders = []
    for shape, body in SHAPES.items():
        src = outdir / (shape + ".comp")
        src.write_text(HEADER + body + TAIL)
        for suffix, opt in (("O", "-O"), ("Os", "-Os"), ("O0", "-O0")):
            raw = outdir / (shape + "." + suffix + ".spv")
            result = exec_cmd("glslc", "--target-env=vulkan1.2", "-fshader-stage=compute", opt, src, "-o", raw)
            if result.returncode:
                raise RuntimeError(f"glslc {shape} {opt}: {result.stderr}")
            if suffix == "O0":
                stripped = outdir / (shape + ".O0-stripped.spv")
                result = exec_cmd("spirv-opt", "--strip-debug", raw, "-o", stripped)
                if result.returncode:
                    raise RuntimeError(result.stderr)
                raw = stripped
            checked = exec_cmd("spirv-val", "--target-env", "vulkan1.2", raw)
            if checked.returncode:
                raise RuntimeError(f"Input is invalid {raw}: {checked.stderr}")
            shaders.append(raw)
    return shaders


def inspect(path):
    result = exec_cmd("spirv-val", "--target-env", "vulkan1.2", path)
    if result.returncode:
        return {"valid": False, "error": (result.stderr or result.stdout).strip()}
    s = exec_cmd("spirv-dis", "--raw-id", path)
    return {"valid": True, "phi_count": s.stdout.count("OpPhi "), "bytes": path.stat().st_size}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--graph-probe", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--loxfx", type=Path, nargs=2, required=True)
    a = parser.parse_args()
    work = a.work.resolve()
    source_dir = a.upstream.resolve()
    original = (source_dir / "re-spirv.cpp").read_text()
    variants = prepare_variants(original, work / "sources")
    compile_variants(source_dir, variants, work / "bin", a.probe.resolve())
    for name in ("baseline", "ordered-phi"):
        status = exec_cmd(
            "c++", "-std=c++17", "-O2", "-I" + str(source_dir),
            "-I" + str(source_dir / "external/SPIRV-Headers/include"),
            work / "sources" / name / "re-spirv.cpp",
            a.graph_probe.resolve(), "-o", work / "bin" / (name + "-graph")
        )
        if status.returncode:
            raise RuntimeError(name + " graph probe compile failure: " + status.stderr[-3000:])
    inputs = create_inputs(work / "shaders")
    inputs.extend(a.loxfx)
    results = []
    changes = []
    order_probes = []
    for path in inputs:
        graph = {"shader": path.name}
        for name in ("baseline", "ordered-phi"):
            proc = exec_cmd(work / "bin" / (name + "-graph"), path)
            graph[name] = {
                "ok": proc.returncode == 0,
                "late": len(re.findall(r"^LATE_PHI_PREDECESSOR", proc.stdout, flags=re.M)),
                "details": proc.stdout.splitlines()[:30],
                "error": proc.stderr[-350:],
            }
        order_probes.append(graph)
        for spec_id, spec_value in ((None, None), (0, 0), (0, 1), (1, 0), (1, 1)):
            tag = f"{path.stem}.spec{spec_id}-{spec_value}" if spec_id is not None else path.stem + ".nospec"
            record = {"shader": path.name, "spec_id": spec_id, "value": spec_value, "variants": {}}
            for mode in variants:
                output = work / "outputs" / mode / (tag + ".spv")
                output.parent.mkdir(parents=True, exist_ok=True)
                args = [str(work / "bin" / mode), str(path), str(output)]
                if spec_id is not None:
                    args.extend((str(spec_id), str(spec_value)))
                proc = exec_cmd(*args)
                if proc.returncode != 0:
                    record["variants"][mode] = {"executed": False, "error": proc.stderr.strip()[-350]}
                    continue
                record["variants"][mode] = inspect(output)
                record["variants"][mode]["executed"] = True
                record["variants"][mode]["same_as_baseline"] = (
                    output.read_bytes() == (work / "outputs" / "baseline" / (tag + ".spv")).read_bytes()
                    if mode != "baseline" else True
                )
            variants_done = record["variants"]
            if variants_done["baseline"].get("valid") and variants_done["no-final-phi"].get("valid"):
                if not variants_done["no-final-phi"]["same_as_baseline"]:
                    changes.append({"shader": path.name, "spec_id": spec_id, "value": spec_value, "type": "late-phi-mattered"})
            results.append(record)
    (work / "audit.json").write_text(json.dumps(results, indent=2) + "\n")
    counts = {}
    for name in variants:
        counts[name] = {
            "valid": sum(x["variants"][name].get("valid", False) for x in results),
            "invalid": sum(x["variants"][name].get("executed") and not x["variants"][name].get("valid", False) for x in results),
            "error": sum(not x["variants"][name].get("executed", False) for x in results),
        }
    print("COUNTS:", json.dumps(counts))
    print("CASES THAT DIFFER WITHOUT FINAL PHI:", json.dumps(changes[:50]))
    print("PHI ORDERING:", json.dumps([
        {"shader": entry["shader"], "baseline_late": entry["baseline"]["late"],
         "ordered_late": entry["ordered-phi"]["late"]}
        for entry in order_probes if entry["baseline"]["late"]
    ]))
    (work / "summary.json").write_text(json.dumps({
        "counts": counts, "late_phi_cases": changes, "ordering": order_probes,
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
