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

    # Make the historical workaround's actual effect observable.
    # Instrument only the call in the final pass, not the main evaluation.
    final_call = """            if (!optimizerCompactPhi(phi.instructionIndex, rContext)) {
                return false;
            }"""
    traced_call = """            const uint32_t beforeWords = optimizedWords[wordIndex] >> 16U;
            const std::vector<uint32_t> before(
                optimizedWords + wordIndex, optimizedWords + wordIndex + beforeWords);
            if (!optimizerCompactPhi(phi.instructionIndex, rContext)) {
                return false;
            }
            const uint32_t afterWords = optimizedWords[wordIndex] >> 16U;
            bool changed = beforeWords != afterWords;
            for (uint32_t j = 0; !changed && j < beforeWords; ++j) {
                changed = before[j] != optimizedWords[wordIndex + j];
            }
            if (changed) {
                fprintf(stderr, "PHI_FINAL_CHANGED index=%u old_words=%u new_words=%u\\n",
                    phi.instructionIndex, beforeWords, afterWords);
            }"""
    traced = replace_once(original, final_call, traced_call)
    variants = {
        "baseline": original,
        "no-final-phi": no_final,
        "ordered-phi": ordered,
        "ordered-no-final": ordered_no_final,
        "trace_final": traced,
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


def make_phi_order_stress(outdir):
    """Legal structured selection with a Phi depending on the header label.

    The condition is deliberately deep but constant-evaluable *by re-SPIRV*
    so the incoming block's terminator may be processed after its Phi.
    """
    program = """
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %out
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %out DescriptorSet 0
OpDecorate %out Binding 0
OpDecorate %Out Block
OpMemberDecorate %Out 0 Offset 0
%void = OpTypeVoid
%function = OpTypeFunction %void
%bool = OpTypeBool
%int = OpTypeInt 32 1
%float = OpTypeFloat 32
%Out = OpTypeStruct %float
%ptrOut = OpTypePointer StorageBuffer %Out
%ptrFloat = OpTypePointer StorageBuffer %float
%out = OpVariable %ptrOut StorageBuffer
%zero = OpConstant %int 0
%one = OpConstant %int 1
%f_one = OpConstant %float 1.0
%main = OpFunction %void None %function
%entry = OpLabel
"""
    previous = "%one"
    for i in range(64):
        new = f"%chain{i}"
        program += f"{new} = OpIAdd %int {previous} %one\n"
        previous = new
    program += f"""%folded_condition = OpIEqual %bool {previous} %zero
OpSelectionMerge %merge None
OpBranchConditional %folded_condition %merge %early_return
%early_return = OpLabel
OpReturn
%merge = OpLabel
%phi = OpPhi %float %f_one %entry
%storage = OpAccessChain %ptrFloat %out %zero
OpStore %storage %phi
OpReturn
OpFunctionEnd
"""
    asm = outdir / "phi_order_stress.spvasm"
    asm.write_text(program)
    path = outdir / "phi_order_stress.spv"
    assembly = exec_cmd("spirv-as", "--target-env", "vulkan1.2", asm, "-o", path)
    if assembly.returncode:
        print("STRESS ASSEMBLY FAILED", assembly.stderr[:2500])
        return None
    checked = exec_cmd("spirv-val", "--target-env", "vulkan1.2", path)
    if checked.returncode:
        print("STRESS VALIDATION FAILED", checked.stderr[:2500])
        return None
    return path



def make_nested_phi_prune(outdir, chain_length, folded_condition_true):
    """Structured selections with a live Phi merge and one pruned incoming edge.

    Unlike the original ordering stress, the Phi's merge block stays live
    even when the predecessor's direct edge disappears.
    """
    name = f"nested_phi_prune_{chain_length}_{int(folded_condition_true)}"
    lines = [
        "OpCapability Shader",
        "OpMemoryModel Logical GLSL450",
        'OpEntryPoint GLCompute %main "main" %out %gid',
        "OpExecutionMode %main LocalSize 1 1 1",
        "OpDecorate %out DescriptorSet 0",
        "OpDecorate %out Binding 0",
        "OpDecorate %Out Block",
        "OpMemberDecorate %Out 0 Offset 0",
        "OpDecorate %gid BuiltIn GlobalInvocationId",
        "%void = OpTypeVoid",
        "%function = OpTypeFunction %void",
        "%bool = OpTypeBool",
        "%int = OpTypeInt 32 1",
        "%uint = OpTypeInt 32 0",
        "%float = OpTypeFloat 32",
        "%uvec3 = OpTypeVector %uint 3",
        "%ptrInput = OpTypePointer Input %uvec3",
        "%Out = OpTypeStruct %float",
        "%ptrOut = OpTypePointer StorageBuffer %Out",
        "%ptrFloat = OpTypePointer StorageBuffer %float",
        "%gid = OpVariable %ptrInput Input",
        "%out = OpVariable %ptrOut StorageBuffer",
        "%zero = OpConstant %int 0",
        "%one = OpConstant %int 1",
        "%uzero = OpConstant %uint 0",
        "%f_one = OpConstant %float 1.0",
        "%f_two = OpConstant %float 2.0",
        "%f_three = OpConstant %float 3.0",
        "%f_four = OpConstant %float 4.0",
        "%main = OpFunction %void None %function",
        "%entry = OpLabel",
        "%loaded_gid = OpLoad %uvec3 %gid",
        "%idx = OpCompositeExtract %uint %loaded_gid 0",
        "%dynamic = OpIEqual %bool %idx %uzero",
        "OpSelectionMerge %outer None",
        "OpBranchConditional %dynamic %B %C",
        "%B = OpLabel",
    ]
    prev = "%one"
    for i in range(chain_length):
        new = f"%sum{i}"
        lines.append(f"{new} = OpIAdd %int {prev} %one")
        prev = new
    # sum > 0, so result is always true for >0; vary direct edge retained.
    if folded_condition_true:
        lines.append(f"%folded = OpINotEqual %bool {prev} %zero")
    else:
        lines.append(f"%folded = OpIEqual %bool {prev} %zero")
    lines.extend((
        "OpSelectionMerge %join None",
        "OpBranchConditional %folded %join %X",
        "%X = OpLabel",
        "OpBranch %join",
        "%join = OpLabel",
        "%innerphi = OpPhi %float %f_one %B %f_two %X",
        "OpBranch %outer",
        "%C = OpLabel",
        "OpBranch %outer",
        "%outer = OpLabel",
        "%outerphi = OpPhi %float %innerphi %join %f_three %C",
        "%storage = OpAccessChain %ptrFloat %out %zero",
        "OpStore %storage %outerphi",
        "OpReturn",
        "OpFunctionEnd",
    ))
    asm = outdir / (name + ".spvasm")
    spv = outdir / (name + ".spv")
    asm.write_text("\n".join(lines) + "\n")
    assembled = exec_cmd("spirv-as", "--target-env", "vulkan1.2", asm, "-o", spv)
    if assembled.returncode:
        print(f"NESTED_ASSEMBLY_FAIL {name}: {assembled.stderr[:700]}", flush=True)
        return None
    validated = exec_cmd("spirv-val", "--target-env", "vulkan1.2", spv)
    if validated.returncode:
        print(f"NESTED_VALIDATION_FAIL {name}: {validated.stderr[:700]}", flush=True)
        return None
    return spv



def make_outer_merge_prune(outdir, chain_depth, fold_true):
    """Try a legal structured branch that exits an inner construct early.

    Keep the outer merge reachable via independent C while the B->outer
    direct edge is folded off. Reject the fixture if spirv-val disallows
    this structured construct.
    """
    name = f"outer_merge_prune_{chain_depth}_{int(fold_true)}"
    lines = [
        "OpCapability Shader", "OpMemoryModel Logical GLSL450",
        'OpEntryPoint GLCompute %main "main" %out %gid',
        "OpExecutionMode %main LocalSize 1 1 1",
        "OpDecorate %out DescriptorSet 0", "OpDecorate %out Binding 0",
        "OpDecorate %Out Block", "OpMemberDecorate %Out 0 Offset 0",
        "OpDecorate %gid BuiltIn GlobalInvocationId",
        "%void = OpTypeVoid", "%func = OpTypeFunction %void",
        "%bool = OpTypeBool", "%int = OpTypeInt 32 1",
        "%uint = OpTypeInt 32 0", "%float = OpTypeFloat 32",
        "%uvec3 = OpTypeVector %uint 3",
        "%ptrInput = OpTypePointer Input %uvec3",
        "%Out = OpTypeStruct %float",
        "%ptrOut = OpTypePointer StorageBuffer %Out",
        "%ptrFloat = OpTypePointer StorageBuffer %float",
        "%gid = OpVariable %ptrInput Input",
        "%out = OpVariable %ptrOut StorageBuffer",
        "%zero = OpConstant %int 0", "%one = OpConstant %int 1",
        "%uzero = OpConstant %uint 0",
        "%fone = OpConstant %float 1.0", "%ftwo = OpConstant %float 2.0",
        "%main = OpFunction %void None %func",
        "%entry = OpLabel",
        "%g = OpLoad %uvec3 %gid", "%x = OpCompositeExtract %uint %g 0",
        "%dynamic = OpIEqual %bool %x %uzero",
        "OpSelectionMerge %outer None",
        "OpBranchConditional %dynamic %B %C",
        "%B = OpLabel",
    ]
    prev = "%one"
    for k in range(chain_depth):
        new = f"%d{k}"
        lines.append(f"{new} = OpIAdd %int {prev} %one")
        prev = new
    lines += [
        f"%fold = OpI{'NotEqual' if fold_true else 'Equal'} %bool {prev} %zero",
        "OpSelectionMerge %exit None",
        "OpBranchConditional %fold %outer %exit",
        "%exit = OpLabel", "OpReturn",
        "%C = OpLabel", "OpBranch %outer",
        "%outer = OpLabel",
        "%phi = OpPhi %float %fone %B %ftwo %C",
        "%ptr = OpAccessChain %ptrFloat %out %zero",
        "OpStore %ptr %phi", "OpReturn", "OpFunctionEnd",
    ]
    asm = outdir / (name + ".spvasm")
    spv = outdir / (name + ".spv")
    asm.write_text("\n".join(lines) + "\n")
    result = exec_cmd("spirv-as", "--target-env", "vulkan1.2", asm, "-o", spv)
    if result.returncode:
        print("OUTER_ASM_FAIL:", name, result.stderr[:600], flush=True)
        return None
    valid = exec_cmd("spirv-val", "--target-env", "vulkan1.2", spv)
    if valid.returncode:
        print("OUTER_VALIDATION_FAIL:", name, valid.stderr[:700], flush=True)
        return None
    return spv


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
    stress = make_phi_order_stress(work / "shaders")
    if stress is not None:
        inputs.append(stress)
    for depth in (0, 2, 16, 64, 128):
        for truth in (False, True):
            case = make_nested_phi_prune(work / "shaders", depth, truth)
            if case is not None:
                inputs.append(case)
    for depth in (0, 8, 64, 128):
        for truth in (False, True):
            case = make_outer_merge_prune(work / "shaders", depth, truth)
            if case is not None:
                inputs.append(case)
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
                if mode == "trace_final":
                    record["final_mutations"] = proc.stderr.count("PHI_FINAL_CHANGED")
                    record["final_trace"] = [
                        line for line in proc.stderr.splitlines() if "PHI_FINAL_CHANGED" in line
                    ][:25]
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
    mutated = [
        {"shader": case["shader"], "spec_id": case["spec_id"],
         "value": case["value"], "mutations": case.get("final_mutations", 0),
         "trace": case.get("final_trace", [])}
        for case in results if case.get("final_mutations", 0)
    ]
    print("PHI FINAL MUTATIONS:", json.dumps(mutated[:80]))

    print("PHI ORDERING:", json.dumps([
        {"shader": entry["shader"], "baseline_late": entry["baseline"]["late"],
         "ordered_late": entry["ordered-phi"]["late"]}
        for entry in order_probes if entry["baseline"]["late"]
    ]))
    (work / "summary.json").write_text(json.dumps({
        "counts": counts, "late_phi_cases": changes, "ordering": order_probes,
        "final_mutation_cases": mutated,
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
