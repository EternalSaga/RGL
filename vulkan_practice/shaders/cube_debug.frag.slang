import types;

// DIAG（RGL_CUBE_TEX）：cube 硬编码 UV + 纹理采样——隔离"dmabuf BO 存在 + 纹理采样"
// 组合触发（GPUVM TCP 权限 fault，2026-08-28 排查）：无模型 BDA、无模型 mesh 管线，
// 仅 cube mesh shader + 4 绑定（0=vertexBuffer 1=indexBuffer 2=texture 3=sampler，
// RenderPipeline texturedKeepIndex 形态）。UV = clip 空间 xy/w 的 abs（0..1，
// CLAMP_TO_EDGE），仅用于肉眼确认采样发生，无几何意义。
[[vk::binding(2, 0)]]
Texture2D<float4> cubeDebugTexture;

[[vk::binding(3, 0)]]
SamplerState cubeDebugSampler;

[shader("fragment")]
float4 main(VertexOutput input) : SV_Target
{
    float2 uv = abs(input.position.xy / input.position.w);
    float4 texel = cubeDebugTexture.Sample(cubeDebugSampler, uv);
    return float4(texel.rgb, 1.0);
}
