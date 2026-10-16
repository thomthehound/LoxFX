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
    start=src.index("        else if (opCode == SpvOpSwitch) {")
    mid=src.index("            // Make the final label the new default case and reduce the word count.", start)
    end=src.index("\n        }\n\n        if (terminatorReduced) {",mid)
    fragment=src[mid:end]
    if "defaultSwitchOpConstantInt" not in fragment or "remainingCaseWords.size()" not in fragment:
        raise RuntimeError("Upstream OpSwitch replacement region no longer matches")
    replacement="""            const uint32_t extraWords = uint32_t(remainingCaseWords.size());
            if (terminatorReduced) {
                // No cases remain. A genuine unconditional branch needs no
                // synthetic integer selector with a separate DCE lifetime.
                optimizedWords[wordIndex] = SpvOpBranch | (2U << 16U);
                optimizedWords[wordIndex + 1] = newDefaultLabelId;
                for (uint32_t j = wordIndex + 2; j < wordIndex + wordCount; ++j) {
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
    variants={"current":original,"switch-as-branch":make_fix(original)}
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
    for opt in ("O0","O","Os"):
        shader=a.input.resolve()/("switch."+("O0-stripped" if opt=="O0" else opt)+".spv")
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
    summary={"cases":len(cases),"invalid_current_repaired":repaired,
             "remaining_invalid":sum(not c["variants"]["switch-as-branch"].get("valid",False) for c in cases)}
    (a.work/"report.json").write_text(json.dumps({"summary":summary,"cases":cases},indent=2)+"\n")
    print("SWITCH_AUDIT_SUMMARY:",json.dumps(summary),flush=True)
    if not repaired or summary["remaining_invalid"]:
        raise SystemExit("Experimental switch-collapsing variant failed acceptance")

if __name__=="__main__":main()
