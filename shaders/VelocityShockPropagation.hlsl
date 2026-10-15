// SPDX-FileCopyrightText: Copyright (c) 2019 Advanced Micro Devices, Inc.
// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: MIT
//
// Adapted from TressFX 4.1 src/Shaders/TressFXSimulation.hlsl,
// VelocityShockPropagation. See NOTICE.md for AMD attribution.

struct Parameters {
    uint vertices_per_strand;
    uint followers_per_guide;
    uint padded_guides;
    uint reserved;
};
#ifdef LOXFX_SPIRV
    [[vk::push_constant]] ConstantBuffer<Parameters> params;
    #define LOXFX_BINDING(index) [[vk::binding(index, 0)]]
#else
    ConstantBuffer<Parameters> params : register(b0, space0);
    #define LOXFX_BINDING(index)
#endif

struct StrandLevelData {
    float4 skinningQuat;
    float4 vspQuat;
    float4 vspTranslation; // xyz: translation, w: propagation coefficient
};
LOXFX_BINDING(0) RWStructuredBuffer<float4> positions : register(u0, space0);
LOXFX_BINDING(1) RWStructuredBuffer<float4> previous_positions : register(u1, space0);
LOXFX_BINDING(3) RWStructuredBuffer<StrandLevelData> strand_data : register(u3, space0);

float3 rotate_vec(float4 q, float3 p) {
    float3 t = 2.0f * cross(q.xyz, p);
    return p + q.w * t + cross(q.xyz, t);
}

[numthreads(64, 1, 1)]
void VelocityShockPropagation(uint local_id : SV_GroupIndex,
                              uint3 group_id : SV_GroupID) {
    // Original master-guide vertex indexing; never simulate follower lanes.
    uint guides_per_group = 64u / params.vertices_per_strand;
    uint guide = group_id.x * guides_per_group + local_id % guides_per_group;
    uint vertex = local_id / guides_per_group;
    if (guide >= params.padded_guides || vertex < 2u) return;

    uint index = guide * (params.followers_per_guide + 1u)
                       * params.vertices_per_strand + vertex;
    StrandLevelData motion = strand_data[guide];
    float coefficient = motion.vspTranslation.w;
    float3 translation = motion.vspTranslation.xyz;

    float4 current = positions[index];
    float4 previous = previous_positions[index];
    current.xyz = lerp(current.xyz,
                       rotate_vec(motion.vspQuat, current.xyz) + translation,
                       coefficient);
    previous.xyz = lerp(previous.xyz,
                        rotate_vec(motion.vspQuat, previous.xyz) + translation,
                        coefficient);
    positions[index] = current;
    previous_positions[index] = previous;
}
