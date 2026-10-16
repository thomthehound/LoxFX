# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
"""Investigate OpSwitch selector DCE on O0 input.

Source-based A/B: original current re-SPIRV (known invalid), and an
experimental OpSwitch-to-OpBranch collapse when all case edges are removed.
This is a research control, not a production fix.
"""
from pathlib import Path
import argparse
import json
from respv_semantic_campaign import SPECS, checked, run, lavapipe_env

def make_fix(src):
    start=src.index("        else if (opCode == SpvOpSwitch) {", src.index("static void optimizerEvaluateTerminator("))
    mid=src.index("            // Make the final label the new default case and reduce the word count.", start)
    end=src.index("\n        }\n\n        if (terminatorReduced) {",mid)
    fragment=src[mid:end]
    if "defaultSwitchOpConstantInt" not in fragment or "remainingCaseWords.size()" not in fragment:
        raise RuntimeError("Upstream OpSwitch replacement region no longer matches")
    replacement="""            const uint32_t extraWords = uint32_t(remainingCaseWords.size());
            if (terminatorReduced) {
                // No cases remain. A genuine unconditional branch needs no
                // synthetic integer selector with a separate DCE lifetime.
                // SPIR-V requires SelectionMerge immediately before
                // OpBranchConditional or OpSwitch, never before OpBranch.
                // Reuse the existing conditional-branch reduction technique.
                const uint32_t mergeInstructionIndex = pInstructionIndex - 1;
                const uint32_t mergeWordIndex = rContext.shader.instructions[mergeInstructionIndex].wordIndex;
                const SpvOp mergeOp = SpvOp(optimizedWords[mergeWordIndex] & 0xFFFFU);
                uint32_t patchIndex = wordIndex;
                if (mergeOp == SpvOpSelectionMerge) {
                    optimizerReduceLabelDegree(mergeInstructionIndex, optimizedWords[mergeWordIndex + 1], rContext);
                    patchIndex = mergeWordIndex;
                }
                optimizedWords[patchIndex] = SpvOpBranch | (2U << 16U);
                optimizedWords[patchIndex + 1] = newDefaultLabelId;
                for (uint32_t j = patchIndex + 2; j < wordIndex + wordCount; ++j) {
                    optimizedWords[j] = UINT32_MAX;
                }
            }
            else {
                optimizedWords[wordIndex] = SpvOpSwitch | ((3U + extraWords) << 16U);
                optimizedWords[wordIndex + 1] = operatorId;
                optimizedWords[wordIndex + 2] = newDefaultLabelId;
                for (uint32_t j = 0; j < extraWords; ++j) {
                    optimizedWords[wordIndex + 3 + j] = remainingCaseWords[j];
                }
                for (uint32_t j = wordIndex + 3 + extraWords; j < wordIndex + wordCount; ++j) {
                    optimizedWords[j] = UINT32_MAX;
                }
            }"""
    return src[:mid]+replacement+src[end:]

def keep_original_selector(src):
    """Keep the switch's existing selector use instead of synthesizing one.

    The switch already has a dependency on operatorId in the parsed graph.
    Leaving the operand and its DCE use intact avoids adding a reference to
    an arbitrarily selected integer constant whose lifetime may be shorter.
    """
    start = src.index("        else if (opCode == SpvOpSwitch) {",
                      src.index("static void optimizerEvaluateTerminator("))
    end = src.index("\n    static bool optimizerCompactPhi(", start)
    section = src[start:end]
    old = "optimizedWords[wordIndex + 1] = terminatorReduced ? rContext.shader.defaultSwitchOpConstantInt : operatorId;"
    if section.count(old) != 1:
        raise RuntimeError("Upstream switch selector anchor changed")
    section = section.replace(old, "optimizedWords[wordIndex + 1] = operatorId;")
    old_degree = """            if (terminatorReduced) {
                // Increase the degree of the default constant that was chosen so it's not considered as dead code.
                uint32_t defaultConstantInstructionIndex = rContext.shader.results[rContext.shader.defaultSwitchOpConstantInt].instructionIndex;
                rContext.instructionOutDegrees[defaultConstantInstructionIndex]++;
            }
"""
    if section.count(old_degree) != 1:
        raise RuntimeError("Upstream synthesized selector lifetime anchor changed")
    section = section.replace(old_degree, "")
    old_release = """        if (terminatorReduced) {
            // The condition operator can be discarded."""
    if section.count(old_release) != 1:
        raise RuntimeError("Upstream terminal release anchor changed")
    section = section.replace(old_release, """        if (terminatorReduced && opCode == SpvOpBranchConditional) {
            // The conditional branch was replaced; its selector is no longer used.""")
    return src[:start] + section + src[end:]


def main():
    p=argparse.ArgumentParser()
    p.add_argument("--upstream",required=True,type=Path)
    p.add_argument("--input",required=True,type=Path)
    p.add_argument("--probe",required=True,type=Path)
    p.add_argument("--driver",required=True,type=Path)
    p.add_argument("--work",required=True,type=Path)
    a=p.parse_args()
    upstream=a.upstream.resolve()
    original=(upstream/"re-spirv.cpp").read_text()
    variants={"current":original,"switch-as-branch":make_fix(original),
              "keep-original-selector":keep_original_selector(original)}
    a.work.mkdir(parents=True,exist_ok=True)
    compiled={}
    for name,source in variants.items():
        cpp=a.work/(name+".cpp");cpp.write_text(source)
        exe=a.work/(name+".exe")
        checked(["c++","-std=c++17","-O2","-DNDEBUG","-I"+str(upstream),
                 "-I"+str(upstream/"external/SPIRV-Headers/include"),
                 cpp,a.probe.resolve(),"-o",exe],name)
        compiled[name]=exe
    cases=[]
    repaired=0
    for family in ("switch", "switch_loop"):
        for opt in ("O0","O","Os"):
            shader=a.input.resolve()/(family+"."+("O0-stripped" if opt=="O0" else opt)+".spv")
            for inline in ((0,1) if opt=="O0" else (0,)):
                for values in SPECS:
                    args=sum(([str(id),str(v)] for id,v in sorted(values.items())),[])
                    expected=checked([a.driver.resolve(),shader,*args],"original GPU",lavapipe_env()).strip()
                    record={"shader":shader.name,"inline":inline,"spec":values,"expected":expected,"variants":{}}
                    for name,exe in compiled.items():
                        output=a.work/(name+"-"+shader.stem+"-"+str(inline)+"-"+str(values[2])+"-"+str(values[3])+".spv")
                        process=run([exe,shader,output,inline,*args])
                        entry={"processed":process.returncode==0,"error":process.stderr[-400:]}
                        if not process.returncode:
                            valid=run(["spirv-val","--target-env","vulkan1.2",output])
                            entry["valid"]=valid.returncode==0
                            entry["validation_error"]=valid.stderr[-500:]
                            if entry["valid"]:
                                executed=run([a.driver.resolve(),output],lavapipe_env())
                                entry["gpu_ok"]=executed.returncode==0
                                entry["gpu_result"]=executed.stdout.strip()
                                if not entry["gpu_ok"] or entry["gpu_result"]!=expected:
                                    raise RuntimeError("GPU mismatch in "+str((name,record,entry)))
                        record["variants"][name]=entry
                    if record["variants"]["current"].get("valid") is False:
                        if record["variants"]["switch-as-branch"].get("valid") is True:
                            repaired+=1
                    print("SWITCH_AUDIT:",json.dumps(record),flush=True)
                    cases.append(record)
    known_invalid = [
        c for c in cases if c["variants"]["current"].get("valid") is False
    ]
    remaining = {
        name: sum(c["variants"][name].get("valid") is not True for c in cases)
        for name in variants if name != "current"
    }
    repaired_by_variant = {
        name: sum(c["variants"][name].get("valid") is True for c in known_invalid)
        for name in remaining
    }
    expected_invalid = [
        c for c in known_invalid
        if not (c["shader"] == "switch.O0-stripped.spv"
                and c["spec"][2] in (1, 2, 3))
    ]
    summary={"cases":len(cases),"invalid_current":len(known_invalid),
             "repaired_by_variant":repaired_by_variant,
             "remaining_invalid":remaining,
             "unexpected_invalid_cases":len(expected_invalid)}
    (a.work/"report.json").write_text(json.dumps({"summary":summary,"cases":cases},indent=2)+"\n")
    print("SWITCH_AUDIT_SUMMARY:",json.dumps(summary),flush=True)
    if (len(known_invalid) != 6 or expected_invalid or any(remaining.values())
            or any(n != 6 for n in repaired_by_variant.values())):
        raise SystemExit("Switch-selector experimental variants failed acceptance")

if __name__=="__main__":main()
