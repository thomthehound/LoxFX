# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
"""Validate four proposed upstream overlays independently in the research branch.

Each candidate is applied to the same unmodified upstream revision. The
overlay files, not this downstream harness, are the proposed upstream changes.
"""
import argparse
import base64
import json
import shutil
import subprocess
import zlib
from pathlib import Path

from respv_dominance_audit import source_variants, make_many_load_stress, make_joined_distinct_store_blocks
from respv_phi_pass_bench import build_phi_flood
from respv_switch_selector_audit import make_switch_without_integer_constant
from respv_semantic_campaign import lavapipe_env


def call(argv, *, env=None, ok=True):
    result = subprocess.run([str(x) for x in argv], text=True, capture_output=True, env=env)
    if ok and result.returncode:
        raise RuntimeError(f"command: {argv}\nexit {result.returncode}\n{result.stdout[-1400:]}\n{result.stderr[-3000:]}")
    return result


def restore_source(upstream, dest, packed):
    dest.mkdir(parents=True, exist_ok=True)
    for filename in ("re-spirv.cpp", "re-spirv.h"):
        shutil.copy2(upstream / filename, dest / filename)
    if packed:
        raw = packed + "=" * (-len(packed) % 4)
        patch = zlib.decompress(base64.urlsafe_b64decode(raw))
        path = dest / "changes.patch"
        path.write_bytes(patch)
        call(("git", "init", "-q", dest))
        call(("git", "-C", dest, "apply", "--check", path))
        call(("git", "-C", dest, "apply", path))


def build(upstream, dest, probe, language):
    binary = dest / ("probe-" + language)
    call(("c++", "-std=" + language, "-O2", "-DNDEBUG",
          "-I" + str(dest), "-I" + str(upstream / "external/SPIRV-Headers/include"),
          dest / "re-spirv.cpp", probe, "-o", binary))
    return binary


def optimize(probe, shader, dest, *spec):
    dest.parent.mkdir(parents=True, exist_ok=True)
    result = call([probe, shader, dest, *spec], ok=False)
    valid = call(("spirv-val", "--target-env", "vulkan1.2", dest), ok=False) if not result.returncode else None
    return {"run": result.returncode == 0,
            "valid": valid is not None and valid.returncode == 0,
            "detail": (result.stderr if result.returncode else (valid.stderr if valid else ""))[-360:]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--patches", type=Path, required=True)
    args = parser.parse_args()
    upstream, work = args.upstream.resolve(), args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    variants = {"stock": None, **json.loads(args.patches.read_text())}
    probe_source = Path(__file__).with_name("respv_phi_probe.cpp").resolve()
    probes = {}
    for name, packed in variants.items():
        dest = work / name
        restore_source(upstream, dest, packed)
        probes[name] = build(upstream, dest, probe_source, "c++17")
        build(upstream, dest, probe_source, "c++20")
        print("COMPILE:", name, "C++17 and C++20 passed", flush=True)

    fixtures = work / "fixtures"
    sources = source_variants(fixtures)
    sources += [make_many_load_stress(fixtures, 32, 16), make_joined_distinct_store_blocks(fixtures, 32)]
    sources += [build_phi_flood(fixtures, n) for n in (16, 64, 128)]
    switch = make_switch_without_integer_constant(fixtures)
    sources.append(switch)
    loop_text = """#version 460
layout(local_size_x=1) in;
layout(binding=0, std430) buffer Output { float vals[]; };
layout(push_constant) uniform Parameters { int n; };
void main() { for (int x=0; x<n; ++x) vals[x]=1.0; }
"""
    loop_source = fixtures / "loop_phi.comp"
    loop_source.write_text(loop_text)
    loop = fixtures / "loop_phi.spv"
    call(("glslc", "-O", "--target-env=vulkan1.2", "-fshader-stage=compute", loop_source, "-o", loop))
    call(("spirv-val", "--target-env", "vulkan1.2", loop))
    sources.append(loop)

    results = {}
    for name, probe in probes.items():
        cases = {}
        for shader in sources:
            spec = ("2", "0") if shader == switch else ()
            cases[shader.name] = optimize(probe, shader, work / name / "outputs" / shader.name, *spec)
        results[name] = cases
        bad = [s for s, info in cases.items() if not info["valid"]]
        print("SHADERS:", name, len(cases), "valid:", len(cases)-len(bad), "not valid:", bad, flush=True)

    # Final-Phi cleanup: valid stock outputs remain identical without the extra pass.
    for shader in sources:
        if results["stock"][shader.name]["valid"]:
            candidate = results["01-final-phi-cleanup"][shader.name]
            before = work / "stock" / "outputs" / shader.name
            after = work / "01-final-phi-cleanup" / "outputs" / shader.name
            if not candidate["valid"] or before.read_bytes() != after.read_bytes():
                raise AssertionError("Final Phi cleanup changed valid stock output: " + shader.name)
    print("PHI_FINAL_EQUIVALENCE: valid baseline outputs identical", flush=True)

    if not results["02-loop-phi-backedges"][loop.name]["valid"]:
        raise AssertionError("Loop Phi fix failed dedicated loop regression")
    print("PHI_BACKEDGE: optimized loop validates", flush=True)

    if results["stock"][switch.name]["run"]:
        raise AssertionError("Stock switch negative control unexpectedly succeeded")
    if not results["03-switch-selector"][switch.name]["valid"]:
        raise AssertionError("Switch fix failed no-ordinary-constant regression")
    print("SWITCH: original rejected; corrected parses and validates", flush=True)

    # spirv-val cannot detect the real dominance bug: validate GPU results too.
    oracle = work / "vulkan_oracle"
    call(("c++", "-O2", "-std=c++17",
          Path(__file__).with_name("respv_vulkan_oracle.cpp").resolve(),
          "-lvulkan", "-o", oracle))
    expected = {
        "right_store.spv": "7,11,11,11,11,11,11,11",
        "left_store.spv": "11,7,7,7,7,7,7,7",
        "entry_store.spv": "11,11,11,11,11,11,11,11",
        "nested_single_store.spv": "7,11,7,7,7,7,7,7",
        "switch_duplicate_targets.spv": "11,7,11,7,7,7,7,7",
    }
    for shader, want in expected.items():
        raw = call((oracle, fixtures / shader), env=lavapipe_env()).stdout.strip()
        if raw != want:
            raise AssertionError(f"Oracle expectation mismatch on {shader}: {raw} != {want}")
        if not results["04-cfg-dominance"][shader]["valid"]:
            raise AssertionError("Invalid dominance output: " + shader)
        actual = call((oracle, work / "04-cfg-dominance" / "outputs" / shader),
                      env=lavapipe_env()).stdout.strip()
        if actual != raw:
            raise AssertionError(f"Dominance wrong-code on {shader}: {actual} != {raw}")
    stock = call((oracle, work / "stock" / "outputs" / "right_store.spv"),
                 env=lavapipe_env()).stdout.strip()
    if stock == expected["right_store.spv"]:
        raise AssertionError("Known upstream dominance bug did not reproduce")
    print("DOMINANCE_VULKAN: five semantic matches; negative control reproduced", flush=True)

    (work / "qualification.json").write_text(json.dumps(results, indent=2) + "\n")
    print("OVERLAY_QUALIFICATION: success", flush=True)


if __name__ == "__main__":
    main()
