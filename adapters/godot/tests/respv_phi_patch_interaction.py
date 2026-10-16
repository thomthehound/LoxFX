# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
"""Determine if the loop Phi-reference-count patch masks the final-Phi pass.

Use identical modern re-SPIRV versions and same input shaders in a full
2x2 experiment: backedge liveness patch on/off, final Phi pass on/off.
"""
import argparse
import json
import subprocess
from pathlib import Path


def run(*args):
    return subprocess.run([str(x) for x in args], text=True, capture_output=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--upstream", type=Path, required=True)
    p.add_argument("--patched-upstream", type=Path, required=True)
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--probe", type=Path, required=True)
    p.add_argument("--shader-folder", type=Path, required=True)
    p.add_argument("--loxfx", nargs=2, type=Path, required=True)
    a = p.parse_args()
    inputs = sorted(a.shader_folder.glob("*.spv")) + [v.resolve() for v in a.loxfx]
    anchor = """        if (!optimizerCompactPhis(context)) {
            return false;
        }"""
    out = a.work.resolve()
    out.mkdir(parents=True, exist_ok=True)
    statuses = {}
    for tag, upstream in (("unpatched", a.upstream.resolve()), ("backedge-patched", a.patched_upstream.resolve())):
        cpp = (upstream / "re-spirv.cpp").read_text()
        if cpp.count(anchor) != 1:
            raise RuntimeError("Unknown final Phi hook in " + tag)
        for final in ("with-final", "without-final"):
            mode = tag + "-" + final
            source = cpp
            if final == "without-final":
                source = cpp.replace(anchor, """        if (false && !optimizerCompactPhis(context)) {
            return false;
        }""")
            folder = out / mode
            folder.mkdir(exist_ok=True)
            cpp_path = folder / "re-spirv.cpp"
            cpp_path.write_text(source)
            binary = folder / "probe"
            built = run("c++", "-std=c++17", "-O2", "-DNDEBUG",
                        "-I" + str(upstream),
                        "-I" + str(upstream / "external/SPIRV-Headers/include"),
                        cpp_path, a.probe.resolve(), "-o", binary)
            if built.returncode:
                raise RuntimeError(f"{mode} compile failure: {built.stderr[-1900:]}")
            for inp in inputs:
                result_file = folder / (inp.name + ".out")
                cmd = run(binary, inp.resolve(), result_file)
                if cmd.returncode:
                    statuses[(inp.name, mode)] = {
                        "ran": False, "code": cmd.returncode, "error": cmd.stderr[-400:],
                    }
                    continue
                valid = run("spirv-val", "--target-env", "vulkan1.2", result_file)
                statuses[(inp.name, mode)] = {
                    "ran": True,
                    "valid": valid.returncode == 0,
                    "error": valid.stderr[-400:],
                    "bytes": result_file.stat().st_size,
                }
    final_cases = []
    observed = {"unpatched": 0, "backedge-patched": 0}
    fail_count = {"unpatched": 0, "backedge-patched": 0}
    for inp in inputs:
        record = {"shader": inp.name}
        for tag in ("unpatched", "backedge-patched"):
            with_mode = tag + "-with-final"
            without_mode = tag + "-without-final"
            with_info = statuses[(inp.name, with_mode)]
            without_info = statuses[(inp.name, without_mode)]
            record[tag] = {"with": with_info, "without": without_info}
            if not with_info["ran"] or not without_info["ran"]:
                fail_count[tag] += 1
            else:
                left = (out / with_mode / (inp.name + ".out")).read_bytes()
                right = (out / without_mode / (inp.name + ".out")).read_bytes()
                record[tag]["changed"] = left != right
                record[tag]["validity_changed"] = with_info["valid"] != without_info["valid"]
                if left != right or with_info["valid"] != without_info["valid"]:
                    observed[tag] += 1
        if record["unpatched"].get("changed") or record["backedge-patched"].get("changed"):
            print("PHI_PATCH_INTERACTION_DIFFERENCE:", json.dumps(record), flush=True)
        final_cases.append(record)
    summary = {"total": len(inputs), "change_counts": observed, "optimizer_failure_counts": fail_count,
               "cases": final_cases}
    print("PHI PATCH INTERACTION:", json.dumps({k: v for k, v in summary.items() if k != "cases"}))
    (out / "interaction.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
