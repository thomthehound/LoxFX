// SPDX-FileCopyrightText: Copyright (c) 2019 Advanced Micro Devices, Inc.
// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: MIT
//
// Adapted from AMD TressFX 4.1 LocalShapeConstraints:
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

struct Parameters {
    uint vertices_per_strand;
    uint followers_per_guide;
    uint padded_guides;
    uint shader_iterations;
    float local_stiffness;
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
    float4 vspTranslation;
};
LOXFX_BINDING(0) RWStructuredBuffer<float4> positions : register(u0, space0);
LOXFX_BINDING(3) RWStructuredBuffer<StrandLevelData> strand_data : register(u3, space0);
LOXFX_BINDING(4) StructuredBuffer<float4> rest_positions : register(t4, space0);

float3 rotate_vec(float4 q, float3 v) {
    float3 t = 2.0f * cross(q.xyz, v);
    return v + q.w*t + cross(q.xyz, t);
}

// Stable between-vector rotation, including near-antiparallel root segments.
float4 between_vectors(float3 old_dir, float3 new_dir) {
    float old_len2 = dot(old_dir, old_dir);
    float new_len2 = dot(new_dir, new_dir);
    if (old_len2 < 1e-16f || new_len2 < 1e-16f)
        return float4(0,0,0,1);
    float3 a = old_dir * rsqrt(old_len2);
    float3 b = new_dir * rsqrt(new_len2);
    float d = clamp(dot(a,b),-1.0f,1.0f);
    float3 cross_ab = cross(a,b);
    if (d < -0.9999f) {
        float cross_len2 = dot(cross_ab,cross_ab);
        if (cross_len2 <= 1e-20f) {
            float3 helper = abs(a.x) < 0.8f ? float3(1,0,0) : float3(0,1,0);
            return float4(normalize(cross(a,helper)),0);
        }
        float3 sum = a+b;
        float half_cos = 0.5f*sqrt(dot(sum,sum));
        float half_sin = sqrt(max(0.0f,1.0f-half_cos*half_cos));
        float4 q = float4(cross_ab*rsqrt(cross_len2)*half_sin,half_cos);
        return q*rsqrt(dot(q,q));
    }
    float4 q = float4(cross_ab,1.0f+d);
    return q*rsqrt(max(dot(q,q),1e-16f));
}

[numthreads(64,1,1)]
void LocalShapeConstraints(uint local_id : SV_GroupIndex,
                           uint3 group_id : SV_GroupID) {
    uint guide = 64u*group_id.x+local_id;
    if (guide >= params.padded_guides || params.local_stiffness <= 0.0f)
        return;
    uint base = guide*(params.followers_per_guide+1u)*params.vertices_per_strand;
    float4 bone_rotation = strand_data[guide].skinningQuat;
    // Preserve reference stiffness normalization (0.95 cap, half strength).
    float stiffness = 0.5f*min(params.local_stiffness,0.95f);

    // A strand is owned by one invocation; dependent sequential corrections
    // do not race other guides. The 64-vertex path can loop internally.
    for (uint iteration=0;iteration<params.shader_iterations;++iteration) {
        for (uint v=1;v+1u<params.vertices_per_strand;++v) {
            uint index = base+v;
            float4 current = positions[index];
            float4 next = positions[index+1u];
            float3 previous = positions[index-1u].xyz;
            float3 bind_current = rotate_vec(bone_rotation,rest_positions[index].xyz);
            float3 bind_next = rotate_vec(bone_rotation,rest_positions[index+1u].xyz);
            float3 bind_previous = rotate_vec(bone_rotation,rest_positions[index-1u].xyz);
            float4 rotation = between_vectors(bind_current-bind_previous,
                                              current.xyz-previous);
            float3 wanted = current.xyz+
                            rotate_vec(rotation,bind_next-bind_current);
            float3 delta = stiffness*(wanted-next.xyz);
            if (current.w > 0.0f) current.xyz -= delta;
            if (next.w > 0.0f) next.xyz += delta;
            positions[index] = current;
            positions[index+1u] = next;
        }
    }
}
