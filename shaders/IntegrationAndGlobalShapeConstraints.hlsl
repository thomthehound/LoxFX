// SPDX-FileCopyrightText: Copyright (c) 2019 Advanced Micro Devices, Inc.
// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: MIT
//
// Derived from integration/indexing in AMD TressFX 4.1:
// https://github.com/thomthehound/TressFX/blob/master/src/Shaders/TressFXSimulation.hlsl
//
// Copyright (c) 2019 Advanced Micro Devices, Inc.
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
// THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// This experimental first stage tests pinned/moving translated attachments,
// Verlet integration, and global rest-shape matching. It is NOT skeletal
// skinning or the complete six-stage simulation.

struct Parameters {
    uint vertices_per_strand;
    uint followers_per_guide;
    uint padded_guides;
    uint reserved;
    float time_step;
    float gravity;
    float damping;
    float global_stiffness;
    float global_range;
    float root_translation_x;
    float root_translation_y;
    float root_translation_z;
};
// Identical integration maths for DXIL/D3D12 and SPIR-V/Vulkan. The parameter
// transport differs: Vulkan push constants vs. D3D12 CBV/root constants.
// A real D3D12 runtime must supply b0 and u0/u1/t2 root bindings.
#ifdef LOXFX_SPIRV
    [[vk::push_constant]] ConstantBuffer<Parameters> params;
    #define LOXFX_BINDING(index) [[vk::binding(index, 0)]]
#else
    ConstantBuffer<Parameters> params : register(b0, space0);
    #define LOXFX_BINDING(index)
#endif

LOXFX_BINDING(0) RWStructuredBuffer<float4> current_positions : register(u0, space0);
LOXFX_BINDING(1) RWStructuredBuffer<float4> previous_positions : register(u1, space0);
LOXFX_BINDING(2) StructuredBuffer<float4> rest_positions : register(t2, space0);
// Two proximal vertices per guide supply the previous-previous history
// needed by CalculateStrandLevelData. Do not allocate follower history.
LOXFX_BINDING(3) RWStructuredBuffer<float4> root_previous_previous : register(u3, space0);

[numthreads(64, 1, 1)]
void IntegrationAndGlobalShapeConstraints(
    uint local_id : SV_GroupIndex, uint3 group_id : SV_GroupID) {
    // TressFX 4.1 CalcIndicesInVertexLevelMaster, with guide-only groups.
    uint guides_per_group = 64u / params.vertices_per_strand;
    uint local_guide = local_id % guides_per_group;
    uint local_vertex = local_id / guides_per_group;
    uint guide = group_id.x * guides_per_group + local_guide;
    if (guide >= params.padded_guides) return;
    uint index = (guide * (params.followers_per_guide + 1u))
        * params.vertices_per_strand + local_vertex;

    float4 current = current_positions[index];
    float4 previous = previous_positions[index];
    float4 rest = rest_positions[index];
    rest.xyz += float3(params.root_translation_x,
                       params.root_translation_y,
                       params.root_translation_z);
    float4 next = current;
    if (current.w > 0.0f) {
        float decay = exp(-params.damping * params.time_step * 60.0f);
        next.xyz = current.xyz + decay * (current.xyz - previous.xyz)
            + float3(0.0f, -params.gravity, 0.0f)
                * (LOXFX_STEP_SECONDS * LOXFX_STEP_SECONDS);
        if (params.global_stiffness > 0.0f &&
            (float)local_vertex <
                params.global_range * (float)params.vertices_per_strand) {
            next.xyz += params.global_stiffness * (rest.xyz - next.xyz);
        }
    } else {
        next = rest; // pinned scalp attachment follows the host translation
    }
    // Reference UpdateFinalVertexPositions saves the older previous state
    // before updating it. Only the two guide-root vertices are consumed by
    // the forthcoming strand-level pass.
    if (local_vertex < 2u)
        root_previous_previous[guide * 2u + local_vertex] = previous;
    previous_positions[index] = current;
    current_positions[index] = next;
}
