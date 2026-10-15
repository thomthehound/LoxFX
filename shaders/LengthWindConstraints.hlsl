// SPDX-FileCopyrightText: Copyright (c) 2019 Advanced Micro Devices, Inc.
// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: MIT
//
// Adapted from TressFX 4.1 LengthConstriantsWindAndCollision:
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
    uint length_iterations;
    float time_step;
    float maximum_position_delta;
    float reserved0;
    float reserved1;
    float4 wind0;
    float4 wind1;
    float4 wind2;
    float4 wind3;
};
#ifdef LOXFX_SPIRV
    [[vk::push_constant]] ConstantBuffer<Parameters> params;
    #define LOXFX_BINDING(index) [[vk::binding(index, 0)]]
#else
    ConstantBuffer<Parameters> params : register(b0, space0);
    #define LOXFX_BINDING(index)
#endif

LOXFX_BINDING(0) RWStructuredBuffer<float4> positions : register(u0,space0);
LOXFX_BINDING(1) RWStructuredBuffer<float4> previous_positions : register(u1,space0);
LOXFX_BINDING(5) StructuredBuffer<float> guide_rest_lengths : register(t5,space0);

groupshared float4 shared_positions[64];
groupshared float shared_lengths[64];

void constrain_pair(uint first, uint second, float target) {
    float4 a=shared_positions[first];
    float4 b=shared_positions[second];
    float3 difference=b.xyz-a.xyz;
    float distance=max(length(difference),1e-7f);
    float3 correction=difference*(1.0f-target/distance);
    if (a.w>0.0f && b.w>0.0f) {
        a.xyz += correction*0.5f;
        b.xyz -= correction*0.5f;
    } else if (a.w>0.0f) {
        a.xyz += correction;
    } else if (b.w>0.0f) {
        b.xyz -= correction;
    }
    shared_positions[first]=a;
    shared_positions[second]=b;
}

[numthreads(64,1,1)]
void LengthWindConstraints(uint local_id : SV_GroupIndex,
                           uint3 group_id : SV_GroupID) {
    const uint per_group=64u/params.vertices_per_strand;
    const uint local_guide=local_id%per_group;
    const uint vertex=local_id/per_group;
    const uint guide=group_id.x*per_group+local_guide;
    // Prepared guides are padded to complete groups; do not return
    // before group-wide barriers.
    const uint index=(guide*(params.followers_per_guide+1u))
                     *params.vertices_per_strand+vertex;
    const uint rest_index=guide*params.vertices_per_strand+vertex;
    const float4 old=previous_positions[index];
    shared_positions[local_id]=positions[index];
    shared_lengths[local_id]=guide_rest_lengths[rest_index];
    GroupMemoryBarrierWithGroupSync();

    if(vertex>=2u && vertex+1u<params.vertices_per_strand &&
       shared_positions[local_id].w>0.0f) {
        const float a=float((guide*(params.followers_per_guide+1u))%20u)/20.0f;
        const float3 wind=a*(params.wind0.xyz+params.wind2.xyz)+
                          (1.0f-a)*(params.wind1.xyz+params.wind3.xyz);
        const float3 segment=shared_positions[local_id].xyz-
                             shared_positions[local_id+per_group].xyz;
        shared_positions[local_id].xyz +=
            -cross(cross(segment,wind),segment)*params.time_step*params.time_step;
    }
    GroupMemoryBarrierWithGroupSync();

    for(uint iteration=0;iteration<params.length_iterations;++iteration) {
        if(vertex < params.vertices_per_strand/2u) {
            const uint first=2u*vertex*per_group+local_guide;
            constrain_pair(first,first+per_group,shared_lengths[first]);
        }
        GroupMemoryBarrierWithGroupSync();
        if(vertex < (params.vertices_per_strand-1u)/2u) {
            const uint first=(2u*vertex+1u)*per_group+local_guide;
            constrain_pair(first,first+per_group,shared_lengths[first]);
        }
        GroupMemoryBarrierWithGroupSync();
    }

    float4 final_position=shared_positions[local_id];
    if(params.maximum_position_delta>0.0f) {
        float3 velocity=final_position.xyz-old.xyz;
        float distance=length(velocity);
        if(distance>params.maximum_position_delta) {
            // Clamp the *magnitude* instead of the squared-ratio
            // overshoot in the original implementation.
            previous_positions[index].xyz=final_position.xyz-
                velocity*(params.maximum_position_delta/distance);
        }
    }
    positions[index]=final_position;
}
