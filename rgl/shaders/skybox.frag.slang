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
    // **显式 mip0**（2026-10-05）：env 立方图 mip 1..L−1 是 GGX 预滤波（按 roughness 糊，
    // env_prefilter.comp.slang:25），不是分辨率金字塔 ⟹ 隐式 `Sample()` 会按方向向量偏导
    // 选到非零 mip，把天穹按粗糙度糊掉（朝天机位全图梯度 5.939 → 3.024，−49%）。
    // 与 deferred light.frag.slang 的天穹分支**同步改**——两路经 shader_registry 共用同一个
    // sky sampler，只改一路会破坏 D1 两路对等（那条对等是这条改动的判据）。
    float3 color = envMap.SampleLevel(envSampler, dir, 0.0).rgb;
    return float4(color, 1.0);
}
