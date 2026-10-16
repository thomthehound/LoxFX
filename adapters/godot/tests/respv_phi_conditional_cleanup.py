# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
"""Conservative conditional Phi cleanup: keep fallback only for late predecessors.

This is a source-only experiment. The parsed schedule decides whether a
predecessor terminator might be processed after a live Phi was first compacted.
The slow historical pass remains enabled for such shaders.
"""
from pathlib import Path
import argparse
import json
import statistics

from respv_phi_pass_bench import build_phi_flood, run


def gated_sources(cpp, header):
    field = "        uint32_t defaultSwitchOpConstantInt = UINT32_MAX;"
    if header.count(field) != 1:
        raise RuntimeError("Shader metadata insertion point changed")
    header = header.replace(field, field + "\n        bool requiresFinalPhiCleanup = false;")

    clear = "        defaultSwitchOpConstantInt = UINT32_MAX;\n"
    if cpp.count(clear) != 1:
        raise RuntimeError("Shader clearing insertion point changed")
    cpp = cpp.replace(clear, clear + "        requiresFinalPhiCleanup = false;\n")

    anchor = """        return true;
    }

    bool Shader::parse(const void *pData, size_t pSize, bool pInlineFunctions) {"""
    if cpp.count(anchor) != 1:
        raise RuntimeError("Shader scheduling insertion point changed")
    insertion = """        // The parsed instruction schedule is invariant for every specialization
        // using this Shader. All predecessor terminators ordered before their
        // Phi are settled by first-pass compaction; otherwise keep the fallback.
        // A loop backedge normally forces the conservative fallback.
        requiresFinalPhiCleanup = false;
        if (!phis.empty()) {
            std::vector<uint32_t> positions(instructions.size(), UINT32_MAX);
            for (uint32_t pos = 0; pos < uint32_t(instructionOrder.size()); ++pos) {
                positions[instructionOrder[pos]] = pos;
            }
            for (const Phi &phi : phis) {
                const uint32_t phiIndex = phi.instructionIndex;
                const uint32_t wordIndex = instructions[phiIndex].wordIndex;
                const uint32_t count = dataWords[wordIndex] >> 16U;
                for (uint32_t j = 3; j < count; j += 2) {
                    const uint32_t predecessorLabelId = dataWords[wordIndex + j + 1];
                    const uint32_t labelIndex = results[predecessorLabelId].instructionIndex;
                    const uint32_t blockIndex = instructions[labelIndex].blockIndex;
                    const uint32_t terminatorIndex = blocks[blockIndex].terminatorInstructionIndex;
                    if (positions[terminatorIndex] > positions[phiIndex]) {
                        requiresFinalPhiCleanup = true;
                        break;
                    }
                }
                if (requiresFinalPhiCleanup) break;
            }
        }

"""
    cpp = cpp.replace(anchor, insertion + anchor)
    hook = """        if (!optimizerCompactPhis(context)) {
            return false;
        }"""
    if cpp.count(hook) != 1:
        raise RuntimeError("Final Phi cleanup hook changed")
    cpp = cpp.replace(
        hook, """        if (context.shader.requiresFinalPhiCleanup && !optimizerCompactPhis(context)) {
            return false;
        }""")
    return cpp, header


def compile_variant(upstream, folder, cpp, header, bench, probe, flags=None):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "re-spirv.cpp").write_text(cpp)
    (folder / "re-spirv.h").write_text(header)
    include = ["-I" + str(folder), "-I" + str(upstream),
               "-I" + str(upstream / "external/SPIRV-Headers/include")]
    output = {}
    for name, client in (("bench", bench), ("probe", probe)):
        exe = folder / name
        run("c++", "-std=c++17", "-O2", "-DNDEBUG", *include,
            folder / "re-spirv.cpp", client, "-o", exe)
        output[name] = exe
    if flags is not None:
        exe = folder / "flags"
        run("c++", "-std=c++17", "-O2", "-DNDEBUG", *include,
            folder / "re-spirv.cpp", flags, "-o", exe)
        output["flags"] = exe
    return output


def timing(bench, shader, parse):
    args = [bench, shader, 550 if not parse else 250]
    if parse:
        args.append("parse-run")
    return float(run(*args).strip())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--upstream", type=Path, required=True)
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--bench", type=Path, required=True)
    ap.add_argument("--probe", type=Path, required=True)
    ap.add_argument("--flags", type=Path, required=True)
    ap.add_argument("--loxfx", nargs=2, type=Path, required=True)
    ap.add_argument("--adversarial", nargs="+", type=Path, required=True)
    opts = ap.parse_args()
    upstream = opts.upstream.resolve()
    work = opts.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    code = (upstream / "re-spirv.cpp").read_text()
    header = (upstream / "re-spirv.h").read_text()
    hook = """        if (!optimizerCompactPhis(context)) {
            return false;
        }"""
    guarded_cpp, guarded_header = gated_sources(code, header)
    variants = {
        "original": (code, header),
        "no-final": (code.replace(hook, "        if (false && !optimizerCompactPhis(context)) {\n            return false;\n        }"), header),
        "conditional": (guarded_cpp, guarded_header),
    }
    tools = {name: compile_variant(upstream, work / "bin" / name,
                                  source, h, opts.bench.resolve(), opts.probe.resolve(),
                                  opts.flags.resolve() if name == "conditional" else None)
             for name, (source, h) in variants.items()}
    flood = work / "shaders"
    flood.mkdir(parents=True, exist_ok=True)
    cases = [build_phi_flood(flood, n) for n in (4, 16, 64, 128)]
    cases.extend(path.resolve() for path in opts.loxfx)
    cases.extend(path.resolve() for path in opts.adversarial)
    rows = []
    for shader in cases:
        flag = run(tools["conditional"]["flags"], shader).strip()
        if flag not in ("0", "1"):
            raise RuntimeError("Invalid Phi fallback flag: " + shader.name)
        outputs = {}
        for name, executables in tools.items():
            output = work / "output" / name / shader.name
            output.parent.mkdir(parents=True, exist_ok=True)
            run(executables["probe"], shader, output)
            run("spirv-val", "--target-env", "vulkan1.2", output)
            outputs[name] = output.read_bytes()
        if outputs["original"] != outputs["conditional"]:
            raise RuntimeError("Conditional Phi safeguard changed shader: " + shader.name)
        if outputs["original"] != outputs["no-final"]:
            raise RuntimeError("Expected final pass equivalence absent: " + shader.name)
        measurements = {name: {"cached": [], "parse_run": []} for name in tools}
        for trial in range(5):
            names = list(tools)
            if trial % 2:
                names.reverse()
            for mode in ("cached", "parse_run") if trial % 2 == 0 else ("parse_run", "cached"):
                for name in names:
                    measurements[name][mode].append(
                        timing(tools[name]["bench"], shader, mode == "parse_run"))
        medians = {name: {
            mode: round(statistics.median(vals), 4)
            for mode, vals in results.items()
        } for name, results in measurements.items()}
        row = {"shader": shader.name, "requires_final": flag == "1",
               "times_us": medians,
               "change_pct": {
                   name: {
                       mode: round(100 * (medians[name][mode] /
                                         medians["original"][mode] - 1), 2)
                       for mode in ("cached", "parse_run")
                   } for name in ("no-final", "conditional")
               }}
        print("PHI_CONDITIONAL:", json.dumps(row), flush=True)
        rows.append(row)
    if not any(r["requires_final"] for r in rows):
        raise RuntimeError("No late-predecessor fallback case was exercised")
    if not any(not r["requires_final"] for r in rows):
        raise RuntimeError("No fast-path Phi cleanup omission case was exercised")
    report = {"cases": rows,
              "fast_path_count": sum(not r["requires_final"] for r in rows),
              "fallback_count": sum(r["requires_final"] for r in rows)}
    (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PHI_CONDITIONAL_SUMMARY:", json.dumps({
        "cases": len(rows), "fast_path_count": report["fast_path_count"],
        "fallback_count": report["fallback_count"]}), flush=True)


if __name__ == "__main__":
    main()
