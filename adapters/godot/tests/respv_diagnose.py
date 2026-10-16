# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
"""Compare re-SPIRV output with and without dead-code elimination.

This is an investigation probe, not the renderer-owned integration acceptance test.
"""
import argparse
import json
import re
import subprocess
from pathlib import Path


def run(*command):
    return subprocess.run(command, text=True, capture_output=True, check=False)


def examine(path):
    validation = run("spirv-val", "--target-env", "vulkan1.2", str(path))
    disassembly = run("spirv-dis", "--raw-id", str(path))
    if disassembly.returncode:
        raise RuntimeError(f"Cannot disassemble {path}: {disassembly.stderr}")
    lines = disassembly.stdout.splitlines()
    return {
        "valid": validation.returncode == 0,
        "validation": (validation.stderr or validation.stdout).strip(),
        "phi_count": sum(" OpPhi " in line for line in lines),
        "loop_count": sum("OpLoopMerge" in line for line in lines),
        "disassembly": lines,
    }


def relevant_lines(info):
    errors = info["validation"]
    ids = set(re.findall(r"(?:ID |IDs? |')(?:(?:')?)(\d+)\[%\d+\]", errors))
    if not ids:
        ids = set(re.findall(r"'(\d+)\[%\d+\]'", errors))
    lines = []
    for number, line in enumerate(info["disassembly"]):
        if any(re.search(rf"%{re.escape(ident)}(?!\d)", line) for ident in ids):
            lines.append(f"{number + 1}: {line}")
            if len(lines) >= 30:
                break
    return sorted(ids), lines


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("shaders", nargs="+", type=Path)
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=True)
    results = []
    for shader in arguments.shaders:
        source = examine(shader)
        if not source["valid"]:
            raise RuntimeError(f"Invalid source module {shader}: {source['validation']}")
        record = {
            "shader": shader.name,
            "source": {key: val for key, val in source.items() if key != "disassembly"},
            "variants": {},
        }
        for mode in ("default", "no-dce"):
            output = arguments.output / f"{shader.stem}.{mode}.spv"
            result = run(str(arguments.probe), str(shader), str(output), mode)
            if result.returncode:
                record["variants"][mode] = {
                    "transformation_error": result.stderr.strip(),
                    "returncode": result.returncode,
                }
                continue
            info = examine(output)
            (arguments.output / f"{shader.stem}.{mode}.spvasm").write_text(
                "\n".join(info["disassembly"]) + "\n", encoding="utf-8"
            )
            ids, references = relevant_lines(info)
            record["variants"][mode] = {
                key: val for key, val in info.items() if key != "disassembly"
            }
            record["variants"][mode]["undefined_ids"] = ids
            record["variants"][mode]["undefined_id_references"] = references
            print(
                f"{shader.name}: {mode}: valid={info['valid']} "
                f"phi={info['phi_count']} loops={info['loop_count']}"
            )
            if info["validation"]:
                print(info["validation"][:1500])
            for reference in references:
                print(reference)
        results.append(record)
    (arguments.output / "findings.json").write_text(
        json.dumps(results, indent=2) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
