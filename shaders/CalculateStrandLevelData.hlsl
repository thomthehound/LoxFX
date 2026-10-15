// SPDX-FileCopyrightText: Copyright (c) 2019 Advanced Micro Devices, Inc.
// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: MIT
// Derived from AMD TressFX 4.1 TressFXSimulation.hlsl
// CalculateStrandLevelData; original MIT notice in the integration shader.
//
// Copyright (c) 2019 Advanced Micro Devices, Inc.
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.

struct MotionParameters {
    uint vertices_per_strand;
    uint followers_per_guide;
    uint padded_guides;
    uint reserved;
    float vsp_coefficient;
    float acceleration_threshold;
    float pad0;
    float pad1;
};
#ifdef LOXFX_SPIRV
    [[vk::push_constant]] ConstantBuffer<MotionParameters> params;
    #define LOXFX_BINDING(index) [[vk::binding(index, 0)]]
#else
    ConstantBuffer<MotionParameters> params : register(b0, space0);
    #define LOXFX_BINDING(index)
#endif

struct StrandLevelData {
    float4 skinningQuat;
    float4 vspQuat;
    float4 vspTranslation; // xyz: translation; w: VSP coefficient
};
LOXFX_BINDING(0) StructuredBuffer<float4> positions : register(t0, space0);
LOXFX_BINDING(1) StructuredBuffer<float4> previous_positions : register(t1, space0);
LOXFX_BINDING(2) StructuredBuffer<float4> root_previous_previous : register(t2, space0);
LOXFX_BINDING(3) RWStructuredBuffer<StrandLevelData> strand_data : register(u3, space0);

float4 between_vectors(float3 old_dir, float3 new_dir) {
    float old_len2 = dot(old_dir, old_dir);
    float new_len2 = dot(new_dir, new_dir);
    // Source assumes two noncoincident attached vertices; keep the
    // experimental runner deterministic for degenerate imported roots.
    if (old_len2 < 1e-16f || new_len2 < 1e-16f)
        return float4(0, 0, 0, 1);
    float3 a = old_dir * rsqrt(old_len2);
    float3 b = new_dir * rsqrt(new_len2);
    float dot_ab = clamp(dot(a,b), -1.0f, 1.0f);
    if (dot_ab < -0.9999f) {
        float3 axis = abs(a.x) < 0.8f ? float3(1,0,0) : float3(0,1,0);
        axis = normalize(cross(a, axis));
        return float4(axis, 0);
    }
    float4 q = float4(cross(a, b), 1.0f + dot_ab);
    return q * rsqrt(max(dot(q,q), 1e-16f));
}
float3 rotate_vec(float4 q, float3 v) {
    // Hamilton quaternion (xyz vector, w scalar).
    float3 t = 2.0f * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz,t);
}

[numthreads(64,1,1)]
void CalculateStrandLevelData(uint local_id : SV_GroupIndex,
                              uint3 group_id : SV_GroupID) {
    uint guide = 64u * group_id.x + local_id;
    if (guide >= params.padded_guides) return;
    uint base = guide * (params.followers_per_guide + 1u)
                       * params.vertices_per_strand;
    float3 old0 = previous_positions[base].xyz;
    float3 old1 = previous_positions[base+1u].xyz;
    float3 new0 = positions[base].xyz;
    float3 new1 = positions[base+1u].xyz;
    float3 older1 = root_previous_previous[guide*2u+1u].xyz;
    float4 rot = between_vectors(old1-old0,new1-new0);
    float3 trans = new0-rotate_vec(rot,old0);
    float3 second_difference = new1 - 2.0f*old1 + older1;
    float coeff = dot(second_difference,second_difference) >
        params.acceleration_threshold*params.acceleration_threshold
        ? 1.0f : params.vsp_coefficient;

    StrandLevelData data;
    // Full source derives this from ApplyVertexBoneSkinning; attachment
    // inputs are presently translation-only, so the skinning rotation
    // is exactly identity, not an invented rotation from the hair tangent.
    data.skinningQuat = float4(0,0,0,1);
    data.vspQuat = rot;
    data.vspTranslation = float4(trans,coeff);
    strand_data[guide] = data;
}
