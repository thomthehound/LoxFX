# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
"""Check re-SPIRV historical revisions for the final Phi-pass dependence.

Uses identical validated SPIR-V inputs and compares the same revision
with and without only its final Phi-compaction safeguard. Compiler APIs
are adjusted with small standalone probe shims, never source changes.
"""
import argparse
import json
import re
import subprocess
from pathlib import Path

# Select milestones at which graph construction, loop support, inlining,
# and cleanup changed; old to new.
ERAS = [
    ("2024-08-reverted", "3c1c0db2ca23"),
    ("2024-08-phi-edges", "3961eeec292a"),
    ("2024-11-extended", "a426e940cfa3"),
    ("2025-03-cycle-guard", "b4ff1f1bd1d8"),
    ("2025-10-inlining-rewrite", "2f9be81bca58"),
    ("2025-12-decoration", "8b9159e38f68"),
    ("2026-05-loop-backedge", "ff2b1a506b0d"),
    ("2026-05-merge-label", "29a77fca3575"),
]


def run(*cmd):
    return subprocess.run([str(v) for v in cmd], text=True, capture_output=True)


def get(repo, rev, path):
    result = run("git", "-C", repo, "show", rev + ":" + path)
    if result.returncode:
        raise RuntimeError(f"Cannot retrieve {rev}:{path}: {result.stderr}")
    return result.stdout


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--repo", type=Path, required=True)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--legacy-probe", type=Path, required=True)
    p.add_argument("--modern-probe", type=Path, required=True)
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--fixtures", type=Path, nargs="+", required=True)
    opts = p.parse_args()
    out = opts.work.resolve()
    out.mkdir(parents=True, exist_ok=True)
    normalized = out / "normalized"
    normalized.mkdir(exist_ok=True)
    inputs = []
    for case in opts.fixtures:
        dis = run("spirv-dis", "--raw-id", case.resolve())
        if dis.returncode:
            raise RuntimeError("Cannot disassemble " + str(case))
        # Normalize the historical OpEntryPoint string-length parser flaw.
        asm = normalized / (case.name + ".spvasm")
        binary = normalized / case.name
        asm.write_text(dis.stdout.replace('"main"', '"entry"'))
        assembled = run("spirv-as", "--target-env", "vulkan1.2", asm, "-o", binary)
        if assembled.returncode:
            raise RuntimeError("Cannot normalize input: " + assembled.stderr)
        valid = run("spirv-val", "--target-env", "vulkan1.2", binary)
        if valid.returncode:
            raise RuntimeError("Bad normalized fixture: " + valid.stderr)
        inputs.append(binary)
    records = []
    for label, sha in ERAS:
        header = get(opts.repo, sha, "re-spirv.h")
        original = get(opts.repo, sha, "re-spirv.cpp")
        # Only alter the *final* wrapper call, never the evaluation pass.
        final_pattern = re.compile(
            r"(?P<all>if \(!optimizerCompactPhis\((?P<arg>[a-zA-Z_]+)\)\) {\s*return false;\s*})"
        )
        matches = list(final_pattern.finditer(original))
        if len(matches) != 1:
            raise RuntimeError(f"Final Phi safeguard ambiguous in {sha}: {len(matches)}")
        fragment = matches[0].group("all")
        no_final = original.replace(fragment, "if (false && !" + fragment[5:])
        modern = "bool pInlineFunctions" in header
        probe = opts.modern_probe if modern else opts.legacy_probe
        era = {"rev": label, "sha": sha, "new_api": modern, "variants": {}}
        binaries = {}
        for mode, code in (("original", original), ("no-final", no_final)):
            folder = out / label / mode
            folder.mkdir(parents=True, exist_ok=True)
            (folder / "re-spirv.h").write_text(header)
            cpp = folder / "re-spirv.cpp"
            cpp.write_text(code)
            exe = folder / "probe"
            result = run("c++", "-std=c++17", "-O2", "-DNDEBUG",
                         "-I" + str(folder),
                         "-I" + str(opts.headers.resolve()),
                         cpp, probe.resolve(), "-o", exe)
            if result.returncode:
                era["variants"][mode] = {"built": False, "error": result.stderr[-1000:]}
            else:
                binaries[mode] = exe
                era["variants"][mode] = {"built": True}
        era["inputs"] = []
        if len(binaries) == 2:
            for case in inputs:
                c = {"name": case.name}
                outputs = {}
                for mode, exe in binaries.items():
                    out_file = out / label / mode / (case.name + ".out")
                    exec_result = run(exe, case, out_file)
                    if exec_result.returncode:
                        c[mode] = {"ran": False, "exit": exec_result.returncode,
                                   "error": exec_result.stderr[-400:]}
                        continue
                    validation = run("spirv-val", "--target-env", "vulkan1.2", out_file)
                    c[mode] = {"ran": True, "valid": validation.returncode == 0,
                               "error": validation.stderr[-500:]}
                    outputs[mode] = out_file.read_bytes()
                if len(outputs) == 2:
                    c["changed"] = outputs["original"] != outputs["no-final"]
                era["inputs"].append(c)
        print("PHI ERA:", json.dumps(era), flush=True)
        records.append(era)
    (out / "eras.json").write_text(json.dumps(records, indent=2) + "\n")


if __name__ == "__main__":
    main()
