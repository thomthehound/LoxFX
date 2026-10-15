// SPDX-FileCopyrightText: Copyright (c) 2019 Advanced Micro Devices, Inc.
// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: MIT
//
// Adapted from AMD TressFX 4.1 UpdateFollowHairVertices:
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
    uint reserved;
};
#ifdef LOXFX_SPIRV
    [[vk::push_constant]] ConstantBuffer<Parameters> params;
    #define LOXFX_BINDING(index) [[vk::binding(index,0)]]
#else
    ConstantBuffer<Parameters> params : register(b0,space0);
    #define LOXFX_BINDING(index)
#endif

LOXFX_BINDING(0) RWStructuredBuffer<float4> positions : register(u0,space0);
// Guide rest geometry remains useful as a fallback for degenerate tangents.
LOXFX_BINDING(4) StructuredBuffer<float4> rest_positions : register(t4,space0);
// The compact static buffer carries guide lengths and per-strand offsets.
LOXFX_BINDING(5) ByteAddressBuffer guide_static_data : register(t5,space0);
LOXFX_BINDING(6) RWStructuredBuffer<float4> dynamic_tangents : register(u6,space0);

[numthreads(64,1,1)]
void UpdateFollowHairVertices(uint local_id : SV_GroupIndex,
                              uint3 group_id : SV_GroupID) {
    const uint per_group=64u/params.vertices_per_strand;
    const uint local_guide=local_id%per_group;
    const uint vertex=local_id/per_group;
    const uint guide=group_id.x*per_group+local_guide;
    if(guide>=params.padded_guides) return;
    const uint strand=guide*(params.followers_per_guide+1u);
    const uint base=strand*params.vertices_per_strand;
    const uint index=base+vertex;
    const float4 guide_position=positions[index];
    const uint tangent_index=vertex+1u<params.vertices_per_strand?index:index-1u;
    float3 delta=positions[tangent_index+1u].xyz-positions[tangent_index].xyz;
    float squared=dot(delta,delta);
    // Only guide rest vertices are consulted in the degenerate case.
    if(squared<=1e-16f) {
        delta=rest_positions[tangent_index+1u].xyz-rest_positions[tangent_index].xyz;
        squared=dot(delta,delta);
    }
    const float4 tangent=float4(squared>1e-16f?
        delta*rsqrt(squared):float3(0,1,0),0);
    dynamic_tangents[index]=tangent;
    // The static segment stores one compact Float4 per strand rather than
    // N rest vertices for each follower (OPT-001).
    const uint offset_base=params.padded_guides*params.vertices_per_strand*4u;
    const float vertex_fraction=float(vertex)/float(params.vertices_per_strand);
    for(uint follow=1u;follow<=params.followers_per_guide;++follow) {
        const uint follower_strand=strand+follow;
        const uint follower_index=follower_strand*params.vertices_per_strand+vertex;
        float4 destination=positions[follower_index];
        const float4 offset=asfloat(guide_static_data.Load4(
            offset_base+follower_strand*16u));
        const float factor=1.0f+offset.w*vertex_fraction;
        destination.xyz=guide_position.xyz+factor*offset.xyz;
        positions[follower_index]=destination;
        dynamic_tangents[follower_index]=tangent;
    }
}
