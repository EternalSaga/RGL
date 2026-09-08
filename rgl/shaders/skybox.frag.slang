// 天空盒 fragment shader（Phase 5.5 批次 4）：像素 NDC → 世界射线 → 采环境 cubemap
//
// 射线重建见 sky.slang（near/far 两点差，免 resolution 依赖）；envMap/envSampler
// binding 4/5 见 env.slang。输出 alpha=1（天空恒最远背景，透明段在其上正常混合）。
// 深度状态（LEQUAL + 不写深度）由 CPU 段状态控制（vkrenderer skyboxState）：
// 大三角 z=1 只在未被几何占据的像素（depth=clear 1.0）通过测试。

import sky;
import env;

[shader("fragment")]
float4 main(SkyVertexOutput input) : SV_Target
{
    float4 far = skyClipToWorld(float4(input.ndc, 1.0, 1.0));
    float4 near = skyClipToWorld(float4(input.ndc, 0.0, 1.0));
    float3 dir = normalize(far.xyz / far.w - near.xyz / near.w);
    float3 color = envMap.Sample(envSampler, dir).rgb;
    return float4(color, 1.0);
}
