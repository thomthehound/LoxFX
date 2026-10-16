# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
"""Differential Vulkan semantics: input SPIR-V vs re-SPIRV with/without final Phi pass.

All shaders and specialization combinations are deterministic. Both optimizer
variants are compiled from the same tested upstream tree (with PR #30 liveness
patch), differing ONLY at the last optimizerCompactPhis call.
"""
from __future__ import annotations
import argparse
import json
import os
import subprocess
from pathlib import Path

HEADER = """#version 460
layout(local_size_x=8) in;
layout(set=0,binding=0,std430) buffer Result { int data[]; } output_data;
layout(constant_id=0) const bool A=true;
layout(constant_id=1) const bool B=false;
layout(constant_id=2) const int MODE=2;
layout(constant_id=3) const int COUNT=3;
"""
TAIL = """
void main() {
  int j = int(gl_GlobalInvocationID.x);
  int x = 5 + 2*j;
  run_case(x,j);
  output_data.data[j] = x;
}
"""
SHAPES = {
    "branch": """void run_case(inout int x, int j) {
        if (A) x += 11; else x -= 7;
        if ((j & 1) != 0) {
            if (B) x = x * 2; else x += 5;
        } else {
            if (A != B) x += 4; else x -= 3;
        }
    }""",
    "nested": """void run_case(inout int x, int j) {
        if (j % 3 == 0) {
            if (A) {
                x += 3;
                if (B) x -= 12; else x = x * 2;
            } else x += 17;
        } else if (j % 3 == 1) {
            if (B) x -= 13; else x += 8;
        } else {
            x = (A ? x + 6 : x - 4);
        }
        if (A != B) x += 1;
    }""",
    "switch": """void run_case(inout int x, int j) {
        switch (MODE) {
            case 0: x += 21; break;
            case 1: x -= 9; break;
            case 2: x = 3*x + 2; break;
            default: x ^= 17; break;
        }
        switch ((j + MODE) & 3) {
            case 0: x += 1; break;
            case 1: x += (A ? 8 : 2); break;
            case 2: x -= (B ? 3 : 6); break;
            default: x += 12; break;
        }
    }""",
    "loops": """void run_case(inout int x, int j) {
        for (int i=0; i<COUNT; ++i) {
            if (i == (j & 3)) x += 7; else x += (A ? 1 : 2);
            if (B && (i & 1) != 0) x -= 5;
        }
        int z=0;
        while (z < ((j & 1) + 1)) { x += (A != B ? 2 : 5); ++z; }
    }""",
    "phi_chains": """void run_case(inout int x, int j) {
        for(int n=0;n<4;n++) {
            if ((j+n) % 3 == 0) x += (A ? 7 : -7);
            else if ((j+n) % 3 == 1) x -= (B ? 9 : 1);
            else x ^= (MODE + n);
            if (COUNT > n && ((j+n)&1) != 0) x += 6;
            else x -= 2;
        }
    }""",
    "functions": """int helper(int x, int j) {
        if ((j & 1) == 0) return (A ? x + 11 : x - 8);
        if (B) return x * 2;
        return x + j;
    }
    int helper2(int x, int j) {
        for(int k=0;k<COUNT;k++) x = helper(x,j+k);
        return x;
    }
    void run_case(inout int x, int j) {
        x = helper2(x,j);
        if(MODE>0) x = helper(x,j+1);
    }""",
    "early_return": """void run_case(inout int x, int j) {
        if (A && (j & 1) == 0) {x += 4;return;}
        if (B) {x += 17;return;}
        for (int i=0;i<COUNT;i++) {
            if (i == j) { x -= 20; break; }
            else x += i+1;
        }
        x += MODE;
    }""",
    "switch_loop": """void run_case(inout int x, int j) {
        for(int k=0;k<COUNT;k++) {
            switch ((k + j + MODE) & 3) {
                case 0: x += (A ? 2 : 3); break;
                case 1: x -= (B ? 3 : 2); break;
                case 2: if ((k & 1) == 0) x ^= 5; else x += 9; break;
                default: x += 4;break;
            }
        }
    }""",
}
SPECS = [
    {0:1,1:0,2:2,3:3},
    {0:0,1:1,2:0,3:0},
    {0:1,1:1,2:1,3:5},
    {0:0,1:0,2:3,3:2},
]

def run(args, env=None):
    return subprocess.run([str(x) for x in args], text=True, capture_output=True, env=env)

def checked(args, label, env=None):
    p=run(args,env)
    if p.returncode:
        raise RuntimeError(f"{label} failed ({p.returncode}): {p.stderr[-2000:]} {p.stdout[-1000:]}")
    return p.stdout

def prepare(upstream, folder, probe):
    folder.mkdir(parents=True, exist_ok=True)
    original=(upstream/"re-spirv.cpp").read_text()
    anchor="""        if (!optimizerCompactPhis(context)) {
            return false;
        }"""
    if original.count(anchor)!=1:
        raise RuntimeError("Final Phi hook changed; inspect manually")
    variants={
        "with-final":original,
        "without-final":original.replace(anchor,"""        // Research only: omit second Phi-compaction pass.
        if (false && !optimizerCompactPhis(context)) {
            return false;
        }"""),
    }
    binaries={}
    for name,cpp in variants.items():
        target=folder/name;target.mkdir(exist_ok=True)
        src=target/"re-spirv.cpp";src.write_text(cpp)
        exe=target/"probe"
        checked(["c++","-O2","-std=c++17","-DNDEBUG","-I"+str(upstream),
                 "-I"+str(upstream/"external/SPIRV-Headers/include"),
                 src,probe,"-o",exe], "compile optimizer "+name)
        binaries[name]=exe
    return binaries

def lavapipe_env():
    env=os.environ.copy()
    icds=sorted(Path("/usr/share/vulkan/icd.d").glob("lvp_icd*.json"))
    if icds: env["VK_ICD_FILENAMES"]=str(icds[0])
    return env

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--upstream",type=Path,required=True)
    p.add_argument("--work",type=Path,required=True)
    p.add_argument("--optimizer",type=Path,required=True)
    p.add_argument("--vulkan",type=Path,required=True)
    a=p.parse_args()
    upstream=a.upstream.resolve();work=a.work.resolve();work.mkdir(parents=True,exist_ok=True)
    binaries=prepare(upstream,work/"compilers",a.optimizer.resolve())
    driver=work/"vulkan_oracle"
    checked(["c++","-O2","-std=c++17",a.vulkan.resolve(),"-lvulkan","-o",driver],"compile Vulkan oracle")
    inputs=work/"inputs";inputs.mkdir(exist_ok=True)
    outputs=work/"outputs";outputs.mkdir(exist_ok=True)
    env=lavapipe_env()
    records=[];failures=[];known_switch_bugs=[];accepted=0;executed=0;inlined=0
    for kind,body in SHAPES.items():
        glsl=inputs/(kind+".comp");glsl.write_text(HEADER+body+TAIL)
        for opt in ("O","Os","O0"):
            raw=inputs/(kind+"."+opt+".spv")
            checked(["glslc","--target-env=vulkan1.2","-fshader-stage=compute","-"+opt,glsl,"-o",raw],
                    "compile "+kind+" "+opt)
            if opt=="O0":
                stripped=inputs/(kind+".O0-stripped.spv")
                checked(["spirv-opt","--strip-debug",raw,"-o",stripped],"strip debug")
                raw=stripped
            checked(["spirv-val","--target-env","vulkan1.2",raw],"validate input")
            for inline in ((0,1) if opt=="O0" else (0,)):
                for spec in SPECS:
                    tag=f"{kind}.{opt}.inline{inline}.sp"+"-".join(str(spec[k]) for k in range(4))
                    overrides=sum(([str(k),str(spec[k])] for k in sorted(spec)),[])
                    variants={};statuses={}
                    for name,exe in binaries.items():
                        target=outputs/name/(tag+".spv")
                        target.parent.mkdir(exist_ok=True)
                        proc=run([exe,raw,target,inline,*overrides])
                        statuses[name]={"ok":proc.returncode==0,"error":proc.stderr[-400:] if proc.returncode else ""}
                        if proc.returncode==0:
                            val=run(["spirv-val","--target-env","vulkan1.2",target])
                            statuses[name]["valid"]=val.returncode==0
                            if val.returncode:
                                statuses[name]["validation_error"]=val.stderr[-650:]
                            variants[name]=target
                    record={"kind":kind,"opt":opt,"inline":inline,"spec":spec,"status":statuses}
                    if statuses["with-final"]["ok"]!=statuses["without-final"]["ok"]:
                        failures.append((tag,"optimizer acceptance differs",statuses))
                    if len(variants)==2:
                        accepted+=1
                        valid=all(statuses[x]["valid"] for x in variants)
                        if not valid:
                            issue=(tag,"invalid transformed shader",statuses)
                            if (kind=="switch" and opt=="O0"
                                and all(x.get("validation_error","").find("OpSwitch")>=0
                                        for x in statuses.values())):
                                known_switch_bugs.append(issue)
                            else:
                                failures.append(issue)
                        record["binary_equal"]=variants["with-final"].read_bytes()==variants["without-final"].read_bytes()
                        if not record["binary_equal"]:
                            failures.append((tag,"final Phi pass changed binary"))
                        if not valid:
                            records.append(record)
                            continue
                        original=run([driver,raw,*overrides],env)
                        if original.returncode:
                            failures.append((tag,"original Vulkan failed "+original.stderr[-600:]))
                        else:
                            expected=original.stdout.strip()
                            record["expected"]=expected
                            for name,path in variants.items():
                                result=run([driver,path],env)
                                executed+=1
                                if result.returncode:
                                    failures.append((tag,name+" Vulkan failed "+result.stderr[-600:]))
                                elif result.stdout.strip()!=expected:
                                    failures.append((tag,name+" GPU mismatch: "+expected+" != "+result.stdout.strip()))
                            if inline: inlined+=1
                    records.append(record)
                    if len(failures)>15:break
                if len(failures)>15:break
            if len(failures)>15:break
        if len(failures)>15:break
    summary={"attempted":len(records),"accepted":accepted,"executed":executed,
             "inlined_accepted":inlined,"shapes":len(SHAPES),
             "spec_combinations":len(SPECS),"failures":failures,"known_switch_bugs":known_switch_bugs,
             "support":{k:sum(int(x["status"][k]["ok"]) for x in records) for k in binaries}}
    (work/"results.json").write_text(json.dumps({"summary":summary,"records":records},indent=2)+"\n")
    print("SEMANTIC_CAMPAIGN:",json.dumps(summary),flush=True)
    if failures or accepted<40 or executed<80 or not known_switch_bugs:
        raise SystemExit("Semantic campaign failed: "+str(failures[:5]))

if __name__=="__main__":
    main()
