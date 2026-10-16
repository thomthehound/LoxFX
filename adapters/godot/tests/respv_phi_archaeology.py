# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
"""Recreate the August 7, 2024 final-Phi workaround with historical source.

Never treat successful modern validation as proof of historical equivalence.
For each fixture, compare old source with its safeguard and old source minus
*only* its last Phi pass. Unsupported ancient shader inputs are recorded.
"""
import argparse
import json
import subprocess
from pathlib import Path


def run(*args):
    return subprocess.run([str(a) for a in args], text=True, capture_output=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--historical", required=True, type=Path)
    p.add_argument("--probe", required=True, type=Path)
    p.add_argument("--work", required=True, type=Path)
    p.add_argument("--inputs", nargs="+", required=True, type=Path)
    a = p.parse_args()
    upstream = a.historical.resolve()
    work = a.work.resolve()
    source = (upstream / "re-spirv.cpp").read_text()
    old = """        if (!optimizerCompactPhis(c)) {
            return false;
        }"""
    if source.count(old) != 1:
        raise RuntimeError("The 2024 Phi compaction source changed")
    variants = {
        "2024-with-final": source,
        "2024-without-final": source.replace(old, """        if (false && !optimizerCompactPhis(c)) {
            return false;
        }"""),
    }
    for variant, content in variants.items():
        folder = work / variant
        folder.mkdir(parents=True, exist_ok=True)
        cpp = folder / "re-spirv.cpp"
        cpp.write_text(content)
        binary = folder / "probe"
        compile_result = run(
            "c++", "-std=c++17", "-O2", "-DNDEBUG",
            "-I" + str(upstream),
            "-I" + str(upstream / "external/SPIRV-Headers/include"),
            cpp, a.probe.resolve(), "-o", binary
        )
        if compile_result.returncode:
            raise RuntimeError(f"Historical build failed {variant}: {compile_result.stderr[-3500:]}")
    # The 2024 parser miscounts null-terminated OpEntryPoint names whose
    # lengths are multiples of four ("main" is four characters). Rename
    # only the entry-point string to "entry" (five characters): this is
    # semantically inert and works around an unrelated historical parser bug.
    # The same normalized SPIR-V is submitted to both old optimizer variants.
    legacy_inputs = work / "legacy-inputs"
    legacy_inputs.mkdir(parents=True, exist_ok=True)
    report = []
    for src in a.inputs:
        for src in sorted(src.parent.glob("nested_phi_prune_*.spv")) if src.name == "phi_order_stress.spv" else [src]:
            dis = run("spirv-dis", "--raw-id", src.resolve())
            if dis.returncode:
                print("HISTORICAL_DIS_FAIL:", src.name, dis.stderr[:200])
                continue
            assembly = dis.stdout.replace('"main"', '"entry"')
            normalized = legacy_inputs / src.name
            asmfile = legacy_inputs / (src.name + ".spvasm")
            asmfile.write_text(assembly)
            normalized_result = run("spirv-as", "--target-env", "vulkan1.2", asmfile, "-o", normalized)
            if normalized_result.returncode:
                print("HISTORICAL_NORMALIZE_FAIL:", src.name, normalized_result.stderr[:300])
                continue
            record = {"input": src.name, "variants": {}, "entry_point_renamed": '"main"' in dis.stdout}
            for variant in variants:
                target = work / variant / (src.name + ".out.spv")
                result = run(work / variant / "probe", normalized, target)
                if result.returncode:
                    record["variants"][variant] = {
                        "ran": False, "code": result.returncode, "error": result.stderr.strip()[-700:],
                    }
                    continue
                validation = run("spirv-val", "--target-env", "vulkan1.2", target)
                record["variants"][variant] = {
                    "ran": True,
                    "valid": validation.returncode == 0,
                    "bytes": target.stat().st_size,
                    "error": validation.stderr.strip()[-700:],
                }
            left = work / "2024-with-final" / (src.name + ".out.spv")
            right = work / "2024-without-final" / (src.name + ".out.spv")
            if left.exists() and right.exists():
                record["changed"] = left.read_bytes() != right.read_bytes()
            print("HISTORICAL_PHI:", json.dumps(record), flush=True)
            report.append(record)
    work.mkdir(parents=True, exist_ok=True)
    (work / "historical.json").write_text(json.dumps(report, indent=2) + "\n")
    breaking = [r for r in report if r.get("changed")]
    print("HISTORICAL_CHANGED_CASES:", json.dumps([r["input"] for r in breaking]))


if __name__ == "__main__":
    main()
