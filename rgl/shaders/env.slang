// 环境 cubemap bindless 契约（Phase 5.5：IBL + 天空盒共享）
//
// binding 4 → resource heap 的 SAMPLED_IMAGE 槽（eCube view + mip 链，
//   Resources env 单例，texture_pool::uploadCubeMap 写入）；
// binding 5 → sampler heap 的 sky sampler 槽（mipmapMode=LINEAR + maxLod=L−1，
//   Resources::ensureSkySampler，偏移走 heapOffset——sampler.cppm 注释的 spec 字段规则）。
// 独立声明文件（不并入 scene.slang）：phong/pbr/unlit 不 import 本文件 → 它们的 SPV
// 不含 binding 4/5 资源，管线 mapping 多烘焙 = 空转无副作用（Phase 3/5 既证行为），
// 反向（声明未烘焙）才是问题——按需 import 保持精确。
//
// 参数字节布局真相源 = 头区契约字段 `SceneHeaderBlock::envParams`.xy（迁移批 S-5，
// binding 22；C++ 写入侧 SceneFrameData.envParams → writeSceneStaging 整块写，
// 偏移由 scene_forward 反射生成物钉死，本文件零字节数）：
//   envParams.x = envIntensity | envParams.y = envMipRange（= mipLevels − 1；0 = 无环境）

import scene;  // 头区 UBO（binding 22）的 readEnvIntensity / readEnvMipRange

[[vk::binding(4, 0)]]
TextureCube<float4> envMap;

[[vk::binding(5, 0)]]
SamplerState envSampler;

struct EnvParams
{
    // 环境强度（scene.lua environment.intensity）。**作用点在 GPU 端**：model_pbr_ibl.frag
    // 采样后乘（env 颜色只存在于 GPU，无法像定向光那样 CPU 预乘进 SceneLight::color）。
    // 与定向光 intensity（CPU 预乘，scene.slang SceneLight::color 注释）是同一字段的两种落地形态——
    // 调光时两条路线各只改自己 config 里的那一个 intensity（docs/archive/教程拆解发现_20260902.md ⑤）
    float intensity;
    float mipRange;   // 可采样 mip 跨度（L−1，roughness→LOD 的满量程）
};

EnvParams readEnvParams()
{
    EnvParams e;
    e.intensity = readEnvIntensity();
    e.mipRange  = readEnvMipRange();
    return e;
}
