// 环境 cubemap 的 **binding 4/5 声明**（Phase 5.5：IBL 与天穹共享的采样源）
//
// binding 4 → resource heap 的 SAMPLED_IMAGE 槽（eCube view + mip 链，
//   Resources env 单例，texture_pool::uploadCubeMap 写入）；
// binding 5 → sampler heap 的 sky sampler 槽（mipmapMode=LINEAR + maxLod=L−1，
//   Resources::ensureSkySampler，偏移走 heapOffset——sampler.cppm 注释的 spec 字段规则）。
//
// 独立声明文件（不并入 scene.slang）：不 import 本文件的 frag → 其 SPV 不含 binding 4/5 资源，
// 管线 mapping 多烘焙 = 空转无副作用（Phase 3/5 既证行为），反向（声明未烘焙）才是问题——
// 按需 import 保持精确。反例 = 需要把 G-buffer 放在 4/5 的 pass（light.frag / ssr_trace.frag
// 因此各自本地声明自己的 env 绑定，不 import 本文件）。
//
// 🔴 **本文件自此只有两枚绑定声明**：env 参数（intensity / mipRange）的读取面 2026-10-18
// 随透明混合管线刀0 移入 pbr.slang 的 `EnvParams` / `readEnvParams()`（谁算 IBL 谁读参数），
// 原 `EnvParams` 结构体与 `readEnvParams()` 已从本文件删除——**别再搬回来**：两侧同名函数
// 同时可见时，调用点写 `readEnvParams().x` 会 ambiguous overload（scene.slang 注释记过这条）。
// 参数字节布局真相源 = 头区契约字段 `SceneHeaderBlock::envParams`.xy（迁移批 S-5，binding 22；
// C++ 写入侧 SceneFrameData.envParams → writeSceneStaging 整块写，偏移由 scene_forward
// 反射生成物钉死，本文件零字节数）：
//   envParams.x = envIntensity | envParams.y = envMipRange（= mipLevels − 1；0 = 无环境）

[[vk::binding(4, 0)]]
TextureCube<float4> envMap;

[[vk::binding(5, 0)]]
SamplerState envSampler;
