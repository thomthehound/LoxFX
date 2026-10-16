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
        ("entry_store", "", ""),
    ]:
        assembly = ASSEMBLY.replace("__LEFT_STORE__", left).replace("__RIGHT_STORE__", right)
        if name == "entry_store":
            assembly = assembly.replace("%gid_value = OpLoad", "OpStore %v %replacement\n%gid_value = OpLoad")
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

    # Exact, on-demand reachability check: can the load block be reached
    # from the function entry *without* passing through the store block?
    # This is deliberately a correctness oracle, not the final fast design.
    start = code.index("    static bool optimizerDoesInstructionDominate(")
    end = code.index("    static bool optimizerRemoveUnusedVariables(", start)
    reachability = """    static bool optimizerDoesInstructionDominate(const Shader &shader, const Instruction &store, const Instruction &load) {
        if (store.blockIndex == load.blockIndex) {
            return store.wordIndex < load.wordIndex;
        }
        const uint32_t storeIndex = uint32_t(&store - shader.instructions.data());
        const uint32_t loadIndex = uint32_t(&load - shader.instructions.data());
        uint32_t entry = UINT32_MAX;
        uint32_t functionOrdinal = UINT32_MAX;
        for (uint32_t i = 0; i < shader.functions.size(); ++i) {
            const uint32_t begin = shader.functions[i].labelInstructionIndex;
            const uint32_t next = (i + 1 < shader.functions.size())
                ? shader.functions[i + 1].instructionIndex : UINT32_MAX;
            if (begin <= storeIndex && storeIndex < next) {
                functionOrdinal = i;
                entry = shader.instructions[begin].blockIndex;
                break;
            }
        }
        if (entry == UINT32_MAX) {
            return false;
        }
        if (functionOrdinal + 1 < shader.functions.size() &&
            loadIndex >= shader.functions[functionOrdinal + 1].instructionIndex) {
            return false;
        }
        const uint32_t *words = shader.inlinedSpirvWords.empty()
            ? shader.extSpirvWords : shader.inlinedSpirvWords.data();
        std::vector<uint8_t> seen(shader.blocks.size(), 0);
        std::vector<uint32_t> stack{entry};
        while (!stack.empty()) {
            const uint32_t block = stack.back();
            stack.pop_back();
            if (block == store.blockIndex || seen[block]) {
                continue;
            }
            if (block == load.blockIndex) {
                return false; // Found path bypassing the candidate store.
            }
            seen[block] = 1;
            const uint32_t ti = shader.blocks[block].terminatorInstructionIndex;
            const uint32_t wi = shader.instructions[ti].wordIndex;
            const SpvOp op = SpvOp(words[wi] & 0xFFFFU);
            const uint32_t wordCount = words[wi] >> 16U;
            uint32_t startWord, count, stride;
            if (!SpvHasLabels(op, startWord, count, stride, false)) {
                continue;
            }
            for (uint32_t k = 0; k < count && (startWord + k*stride) < wordCount; ++k) {
                const uint32_t label = words[wi + startWord + k*stride];
                const uint32_t instr = shader.results[label].instructionIndex;
                const uint32_t nextBlock = shader.instructions[instr].blockIndex;
                if (!seen[nextBlock]) {
                    stack.push_back(nextBlock);
                }
            }
        }
        return true; // Every path to the load traverses the store block.
    }

"""
    variants["exact_reachability"] = code[:start] + reachability + code[end:]
    filter_anchor = "        const uint32_t storeIndex = uint32_t(&store - shader.instructions.data());"
    if reachability.count(filter_anchor) != 1:
        raise RuntimeError("Expected one reachability check insertion point")
    filtered = reachability.replace(filter_anchor, """        // Cheap DFS-ancestry rejection: necessary but not sufficient for dominance.
        const uint32_t aPre = shader.blockPreOrderIndices[store.blockIndex];
        const uint32_t bPre = shader.blockPreOrderIndices[load.blockIndex];
        const uint32_t aPost = shader.blockPostOrderIndices[store.blockIndex];
        const uint32_t bPost = shader.blockPostOrderIndices[load.blockIndex];
        if (!(aPre < bPre && aPost > bPost)) {
            return false;
        }
""" + filter_anchor)
    variants["filtered_exact"] = code[:start] + filtered + code[end:]

    # Correct, reusable dominance metadata: derive CFG edges from *actual*
    # branch terminators, then construct each function's dominator tree.
    # A dominator-tree Euler interval gives O(1) comparisons per lookup;
    # parser cost is measured separately from repeated optimizer runs.
    header = (upstream / "re-spirv.h").read_text()
    new_header = header.replace(
        "        std::vector<uint32_t> blockPostOrderIndices;",
        "        std::vector<uint32_t> blockPostOrderIndices;\n"
        "        std::vector<uint32_t> blockDomPreOrderIndices;\n"
        "        std::vector<uint32_t> blockDomPostOrderIndices;"
    ).replace(
        "        bool sort(const void *pData, size_t pSize);",
        "        bool sort(const void *pData, size_t pSize);\n"
        "        bool buildDominatorTree(const void *pData, size_t pSize);"
    )
    if new_header == header:
        raise RuntimeError("Unable to attach dominator metadata to Shader")

    dom_function = """    bool Shader::buildDominatorTree(const void *pData, size_t pSize) {
        (void)pSize;
        const uint32_t *words = reinterpret_cast<const uint32_t *>(pData);
        const uint32_t n = uint32_t(blocks.size());
        blockDomPreOrderIndices.assign(n, UINT32_MAX);
        blockDomPostOrderIndices.assign(n, UINT32_MAX);
        std::vector<std::vector<uint32_t>> successors(n), predecessors(n);
        for (uint32_t b = 0; b < n; ++b) {
            const uint32_t ti = blocks[b].terminatorInstructionIndex;
            const uint32_t wi = instructions[ti].wordIndex;
            const SpvOp op = SpvOp(words[wi] & 0xFFFFU);
            const uint32_t wc = words[wi] >> 16U;
            uint32_t start, count, stride;
            if (!SpvHasLabels(op, start, count, stride, false)) {
                continue;
            }
            for (uint32_t j = 0; j < count && start + j*stride < wc; ++j) {
                const uint32_t label = words[wi + start + j*stride];
                if (label >= results.size() || results[label].instructionIndex == UINT32_MAX) {
                    return false;
                }
                const uint32_t target = instructions[results[label].instructionIndex].blockIndex;
                if (target >= n) {
                    return false;
                }
                successors[b].push_back(target);
                predecessors[target].push_back(b);
            }
        }
        struct Visit { uint32_t block, next; };
        std::vector<uint32_t> idom(n, UINT32_MAX), rank(n, UINT32_MAX);
        std::vector<uint8_t> seen(n, 0);
        std::vector<std::vector<uint32_t>> children(n);
        uint32_t time = 0;
        for (const auto &function : functions) {
            const uint32_t root = instructions[function.labelInstructionIndex].blockIndex;
            if (root >= n) {
                return false;
            }
            if (seen[root]) {
                continue;
            }
            std::vector<uint32_t> rpo;
            std::vector<Visit> frames{{root, 0}};
            seen[root] = 1;
            while (!frames.empty()) {
                Visit &f = frames.back();
                if (f.next < successors[f.block].size()) {
                    const uint32_t next = successors[f.block][f.next++];
                    if (!seen[next]) {
                        seen[next] = 1;
                        frames.push_back({next, 0});
                    }
                } else {
                    rpo.push_back(f.block);
                    frames.pop_back();
                }
            }
            std::reverse(rpo.begin(), rpo.end());
            for (uint32_t i = 0; i < rpo.size(); ++i) {
                rank[rpo[i]] = i;
            }
            idom[root] = root;
            bool changed;
            do {
                changed = false;
                for (uint32_t i = 1; i < rpo.size(); ++i) {
                    const uint32_t block = rpo[i];
                    uint32_t candidate = UINT32_MAX;
                    for (uint32_t pred : predecessors[block]) {
                        if (idom[pred] == UINT32_MAX) {
                            continue;
                        }
                        if (candidate == UINT32_MAX) {
                            candidate = pred;
                            continue;
                        }
                        uint32_t left = pred, right = candidate;
                        while (left != right) {
                            while (rank[left] > rank[right]) {
                                left = idom[left];
                            }
                            while (rank[right] > rank[left]) {
                                right = idom[right];
                            }
                        }
                        candidate = left;
                    }
                    if (idom[block] != candidate) {
                        idom[block] = candidate;
                        changed = true;
                    }
                }
            } while (changed);

            for (uint32_t b : rpo) {
                if (b != root && idom[b] != UINT32_MAX) {
                    children[idom[b]].push_back(b);
                }
            }
            frames.clear();
            frames.push_back({root, 0});
            blockDomPreOrderIndices[root] = time++;
            while (!frames.empty()) {
                Visit &f = frames.back();
                if (f.next < children[f.block].size()) {
                    const uint32_t child = children[f.block][f.next++];
                    blockDomPreOrderIndices[child] = time++;
                    frames.push_back({child, 0});
                } else {
                    blockDomPostOrderIndices[f.block] = time++;
                    frames.pop_back();
                }
            }
        }
        return true;
    }

"""
    parse_match = """        if (!sort(data, size)) {
            clear();
            return false;
        }

        return true;
    }"""
    if code.count(parse_match) != 1:
        raise RuntimeError("Unable to locate shader parse hook")
    precompute = code.replace(
        parse_match,
        """        if (!sort(data, size) || !buildDominatorTree(data, size)) {
            clear();
            return false;
        }

        return true;
    }"""
    )
    entry_marker = "    bool Shader::parse(const void *pData, size_t pSize, bool pInlineFunctions) {"
    if precompute.count(entry_marker) != 1:
        raise RuntimeError("Unable to locate Shader::parse")
    precompute = precompute.replace(entry_marker, dom_function + entry_marker)
    # Avoid accidentally substituting a region from ordinary DFS intervals:
    old = """            return (aPreIndex < bPreIndex) && (aPostIndex > bPostIndex);"""
    new = """            const uint32_t store = pInstructionA.blockIndex;
            const uint32_t load = pInstructionB.blockIndex;
            const uint32_t aPre = pShader.blockDomPreOrderIndices[store];
            const uint32_t bPre = pShader.blockDomPreOrderIndices[load];
            const uint32_t aPost = pShader.blockDomPostOrderIndices[store];
            const uint32_t bPost = pShader.blockDomPostOrderIndices[load];
            return aPre != UINT32_MAX && bPre != UINT32_MAX &&
                   aPre < bPre && aPost > bPost;"""
    if precompute.count(old) != 1:
        raise RuntimeError("Unable to locate dominance test")
    precompute = precompute.replace(old, new)
    variants["precomputed_domtree"] = precompute
    tools = {}
    for name, cpp in variants.items():
        folder = work / "variants" / name
        folder.mkdir(parents=True, exist_ok=True)
        source = folder / "re-spirv.cpp"
        source.write_text(cpp)
        if name == "precomputed_domtree":
            (folder / "re-spirv.h").write_text(new_header)
        for label, program in (("probe", probe), ("bench", bench)):
            binary = folder / label
            require(("c++", "-std=c++17", "-O2", "-DNDEBUG", 
                     "-I" + str(folder), "-I" + str(upstream),
                     "-I" + str(upstream / "external/SPIRV-Headers/include"),
                     source, program, "-o", binary), f"compile {name} {label}")
            tools[(name, label)] = binary
    return tools


def disasm(path):
    return require(("spirv-dis", "--raw-id", path), f"disassemble {path.name}").stdout


def evaluate(probes, shaders, work, iterations):
    records = []
    for shader in shaders:
        entry = {"source": shader.name, "outputs": {}, "times_us": {}, "parse_run_us": {}}
        original_text = disasm(shader)
        for name in ("baseline", "same_block_only", "exact_reachability", "filtered_exact", "precomputed_domtree"):
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
            if name != "baseline":
                before = work / "outputs" / "baseline" / shader.name
                entry["differ_" + name] = before.read_bytes() != target.read_bytes()
                if entry["differ_" + name]:
                    entry["diff_" + name] = list(difflib.unified_diff(
                        disasm(before).splitlines(), optimized_text.splitlines(),
                        fromfile="baseline", tofile=name, lineterm=""
                    ))[:125]
            b = require((probes[(name, "bench")], shader, str(iterations)), f"benchmark {name} {shader.name}")
            entry["times_us"][name] = float(b.stdout.strip())
            total = require((probes[(name, "bench")], shader, str(max(100, iterations // 2)), "parse-run"),
                            f"parse-run benchmark {name} {shader.name}")
            entry["parse_run_us"][name] = float(total.stdout.strip())
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
    by_name = {rec["source"]: rec for rec in records}
    # Assertions describe observable semantics, not merely whether SPIR-V validates.
    for name in ("right_store.spv", "entry_store.spv"):
        record = by_name[name]
        if any(not record["outputs"][variant]["valid"] for variant in
               ("baseline", "same_block_only", "exact_reachability", "precomputed_domtree")):
            raise AssertionError(f"Invalid SPIR-V in {name}: {record['outputs']}")
    right = by_name["right_store.spv"]
    entry = by_name["entry_store.spv"]
    if not right["differ_precomputed_domtree"]:
        raise AssertionError("Dominator tree failed to prevent unsafe cross-branch forwarding")
    if entry["differ_precomputed_domtree"]:
        raise AssertionError("Dominator tree unexpectedly prevented a valid forwarding")
    if not right["differ_exact_reachability"]:
        raise AssertionError("Reference reachability check did not identify the unsafe forwarding")
    (work / "dominance.json").write_text(json.dumps(records, indent=2) + "\n")


if __name__ == "__main__":
    main()
