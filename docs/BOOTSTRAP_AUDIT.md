<!-- SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Bootstrap architecture and implementation audit

Audit date: 15 October 2026. Scope: standalone LoxFX bootstrap on PR #1.

## Verified decisions and corrections

| Area | Finding | Implemented correction |
| --- | --- | --- |
| Language | An initial C++17 requirement conflicted with the new SDK target. | C++23 required for the library and exported to consumers. Public import uses `std::span` and `std::expected`. |
| Tooling | CMake 3.21 and checkout v4 were older than needed for a new C++23 SDK. | CMake 3.28+; checkout v7; `windows-latest` and Ubuntu 24.04 CI. |
| Public packaging | The original static target could not be consumed through an installed CMake package. | Relocatable `LoxFX::Core` target, install rules, package config/version files, and independent downstream build test on Windows and Linux. |
| Import representation | Legacy TressFX pads guide strands to GPU workgroup sizes while importing. | Preserve authored counts and keep GPU packing/derived follower creation outside the file loader. |
| Integer safety | Multiplying untrusted strand and vertex counts before testing overflow can wrap on 32-bit `size_t`. | Check multiplication bounds first; reject inconsistent sections and excessive lengths. |
| Binary validity | Parsed positions alone were checked for NaN/Infinity; malformed optional attributes could propagate invalid values. | Check finite UV, thickness, and color components; require documented eight-byte section offsets. |
| Byte decoding | Ad hoc floating-point byte reconstruction via memory copies was unnecessary. | Explicit little-endian scalar decode followed by `std::bit_cast`; no packed-struct reinterpretation or unaligned pointer access. |
| Verification | Synthetic happy-path tests did not exercise malformed-file memory behavior or downstream installation. | Add invalid-section tests, address/undefined-behavior sanitizer CI, and installed-package consumer tests. |
| Documentation | The earlier research draft assumed the reference code lived inside LoxFX and included obsolete links. | Preserve the complete research analysis under `MODERNIZATION_RESEARCH.md` with corrected source locations and independent project scope. |

## Remaining acceptance gaps

- **Real data:** synthetic tests do not prove compatibility with exported production `.tfx` data. Test actual licensed reference assets and multiple exporters, including optional attributes and boundary variants.
- **Format coverage:** `.tfxbone`, `.tfxmesh`, skeleton bindings, unknown attributes, and lossless source-data preparation are not implemented. No claim of full TressFX asset support.
- **CPU behavior:** import and inspection are operational, but there is no working CPU hair solver yet; nor is there a GPU simulator.
- **GPU baseline:** native Vulkan/D3D12 execution, shader compilation, synchronization, external render handoff, feature admission and physical hardware performance evidence are pending.
- **Compiler coverage:** Ubuntu and Windows production jobs are green, but Clang/libc++, multiple library implementations, and 32-bit behavior are not independently qualified.
- **Robustness:** sanitizer tests are deterministic synthetic cases. Add actual-corpus regression tests and optional fuzzing before accepting arbitrary external assets at scale.
- **SDK lifecycle:** public C++ headers and install targets function, but binary ABI stability, versioned groom serialization and resource-handling APIs remain intentionally undecided.
- **Legacy reference:** source-to-port dispatch mapping, original per-stage performance and host/GPU ownership traces must be established before lifting simulation code.
- **Performance:** CI on hosted runners is a functional acceptance gate, not a consumer-GPU throughput measurement.

## Standards for the next milestone

Keep authored data and GPU-specific packing separate. Preserve the simulation's source-derived dispatch dependencies and storage owners, rather than introducing an unrelated pipeline. Require meaningful measurements of GPU time, memory, barrier count, data transfers and visible quality. Defer hardware-specific optimizations until they can be compared against a fast baseline. Public simulation data contracts must remain engine-neutral; adapter-specific ownership and synchronization belong behind explicit integration interfaces.

The implementation status in [ROADMAP.md](ROADMAP.md) remains authoritative. Engineering options and literature comparisons are preserved in [MODERNIZATION_RESEARCH.md](MODERNIZATION_RESEARCH.md).
