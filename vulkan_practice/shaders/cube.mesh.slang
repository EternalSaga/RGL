import types;

[shader("mesh")]
[numthreads(32, 1, 1)]
[outputtopology("triangle")]
void main(
    uint ti : SV_GroupIndex,
    out vertices VertexOutput verts[8],
    out indices uint3 triangles[12])
{
    SetMeshOutputCounts(8, 12);

    // Hardcoded cube vertices for testing
    static const float3 positions[8] = {
        float3(-0.5, -0.5, -0.5), float3( 0.5, -0.5, -0.5),
        float3( 0.5,  0.5, -0.5), float3(-0.5,  0.5, -0.5),
        float3(-0.5, -0.5,  0.5), float3( 0.5, -0.5,  0.5),
        float3( 0.5,  0.5,  0.5), float3(-0.5,  0.5,  0.5)
    };
    static const float3 colors[8] = {
        float3(1,0,0), float3(0,1,0), float3(0,0,1), float3(1,1,0),
        float3(1,0,1), float3(0,1,1), float3(1,1,1), float3(0,0,0)
    };
    static const uint indices[36] = {
        0,1,2, 2,3,0, 1,5,6, 6,2,1,
        5,4,7, 7,6,5, 4,0,3, 3,7,4,
        3,2,6, 6,7,3, 4,5,1, 1,0,4
    };

    if (ti < 8) {
        verts[ti].position = mul(push.mvp, float4(positions[ti], 1.0));
        verts[ti].color = colors[ti];
    }

    if (ti < 12) {
        uint baseIdx = ti * 3;
        triangles[ti] = uint3(indices[baseIdx], indices[baseIdx+1], indices[baseIdx+2]);
    }
}
