# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
"""Empirically compare re-SPIRV's DFS-ancestry test to real CFG dominance.

Independent research tools: do not modify production LoxFX or Godot code.
"""
import argparse
import difflib
import json
import subprocess
from pathlib import Path


def run(*args):
    return subprocess.run([str(x) for x in args], text=True, capture_output=True, check=False)


def require(cmd, name):
    p = run(*cmd)
    if p.returncode:
        raise RuntimeError(f"{name}: {p.stderr[-3200:]} {p.stdout[-500:]}")
    return p


ASSEMBLY = """OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %out %gid
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %gid BuiltIn GlobalInvocationId
OpDecorate %out DescriptorSet 0
OpDecorate %out Binding 0
OpDecorate %Out Block
OpMemberDecorate %Out 0 Offset 0
OpDecorate %array ArrayStride 4
%void = OpTypeVoid
%func = OpTypeFunction %void
%bool = OpTypeBool
%int = OpTypeInt 32 1
%uint = OpTypeInt 32 0
%uvec3 = OpTypeVector %uint 3
%array = OpTypeRuntimeArray %int
%Out = OpTypeStruct %array
%ptrOut = OpTypePointer StorageBuffer %Out
%ptrBufferInt = OpTypePointer StorageBuffer %int
%ptrInput = OpTypePointer Input %uvec3
%ptrLocal = OpTypePointer Function %int
%gid = OpVariable %ptrInput Input
%out = OpVariable %ptrOut StorageBuffer
%zero = OpConstant %uint 0
%member = OpConstant %int 0
%initial = OpConstant %int 7
%replacement = OpConstant %int 11
%main = OpFunction %void None %func
%entry = OpLabel
%v = OpVariable %ptrLocal Function %initial
%gid_value = OpLoad %uvec3 %gid
%index = OpCompositeExtract %uint %gid_value 0
%choose = OpIEqual %bool %index %zero
OpSelectionMerge %join None
OpBranchConditional %choose %left %right
%left = OpLabel
__LEFT_STORE__
OpBranch %join
%right = OpLabel
__RIGHT_STORE__
OpBranch %join
%join = OpLabel
%loaded = OpLoad %int %v
%dest = OpAccessChain %ptrBufferInt %out %member %index
OpStore %dest %loaded
OpReturn
OpFunctionEnd
"""


def source_variants(work):
    work.mkdir(parents=True, exist_ok=True)
    paths = []
    for name, left, right in [
        ("left_store", "OpStore %v %replacement", ""),
        ("right_store", "", "OpStore %v %replacement"),
    ]:
        assembly = ASSEMBLY.replace("__LEFT_STORE__", left).replace("__RIGHT_STORE__", right)
        asm = work / f"{name}.spvasm"
        output = work / f"{name}.spv"
        asm.write_text(assembly)
        require(("spirv-as", "--target-env", "vulkan1.2", asm, "-o", output), f"assemble {name}")
        require(("spirv-val", "--target-env", "vulkan1.2", output), f"validate {name}")
        paths.append(output)
    return paths


def compile_tools(upstream, probe, bench, work):
    code = (upstream / "re-spirv.cpp").read_text()
    original = """            return (aPreIndex < bPreIndex) && (aPostIndex > bPostIndex);"""
    if code.count(original) != 1:
        raise RuntimeError("Upstream dominance test changed: inspect before running audit")
    variants = {
        "baseline": code,
        "same_block_only": code.replace(original, "            return false; // Audit: reject unsafe cross-block forwarding."),
    }
    tools = {}
    for name, cpp in variants.items():
        folder = work / "variants" / name
        folder.mkdir(parents=True, exist_ok=True)
        source = folder / "re-spirv.cpp"
        source.write_text(cpp)
        for label, program in (("probe", probe), ("bench", bench)):
            binary = folder / label
            require(("c++", "-std=c++17", "-O2", "-DNDEBUG", 
                     "-I" + str(upstream), "-I" + str(upstream / "external/SPIRV-Headers/include"),
                     source, program, "-o", binary), f"compile {name} {label}")
            tools[(name, label)] = binary
    return tools


def disasm(path):
    return require(("spirv-dis", "--raw-id", path), f"disassemble {path.name}").stdout


def evaluate(probes, shaders, work, iterations):
    records = []
    for shader in shaders:
        entry = {"source": shader.name, "outputs": {}, "times_us": {}}
        original_text = disasm(shader)
        for name in ("baseline", "same_block_only"):
            target = work / "outputs" / name / shader.name
            target.parent.mkdir(parents=True, exist_ok=True)
            p = run(probes[(name, "probe")], shader, target)
            if p.returncode:
                entry["outputs"][name] = {"valid": False, "error": p.stderr[-900:]}
                continue
            valid = run("spirv-val", "--target-env", "vulkan1.2", target)
            optimized_text = disasm(target) if valid.returncode == 0 else ""
            entry["outputs"][name] = {
                "valid": valid.returncode == 0,
                "error": valid.stderr[-900:],
                "opload_count": optimized_text.count("OpLoad"),
                "opstore_count": optimized_text.count("OpStore"),
                "bytes": target.stat().st_size,
            }
            if name == "same_block_only":
                before = work / "outputs" / "baseline" / shader.name
                entry["differ"] = before.read_bytes() != target.read_bytes()
                if entry["differ"]:
                    entry["diff"] = list(difflib.unified_diff(
                        disasm(before).splitlines(), optimized_text.splitlines(),
                        fromfile="baseline", tofile="same_block_only", lineterm=""
                    ))[:125]
            b = require((probes[(name, "bench")], shader, str(iterations)), f"benchmark {name} {shader.name}")
            entry["times_us"][name] = float(b.stdout.strip())
        entry["source_loads"] = original_text.count("OpLoad")
        records.append(entry)
        print(json.dumps(entry), flush=True)
    return records


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--upstream", type=Path, required=True)
    p.add_argument("--probe", type=Path, required=True)
    p.add_argument("--bench", type=Path, required=True)
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--loxfx", type=Path, nargs=2, required=True)
    p.add_argument("--iterations", type=int, default=350)
    a = p.parse_args()
    work = a.work.resolve()
    upstream = a.upstream.resolve()
    paths = source_variants(work / "shaders")
    tools = compile_tools(upstream, a.probe.resolve(), a.bench.resolve(), work)
    records = evaluate(tools, paths + a.loxfx, work, a.iterations)
    (work / "dominance.json").write_text(json.dumps(records, indent=2) + "\n")


if __name__ == "__main__":
    main()
