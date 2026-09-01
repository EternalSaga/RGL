# Phase 5.5 实施计划与记录：IBL（简化版）+ 天空盒

> **状态**：现行（2026-09-01 开工；实施记录与决策权威，行号以当时代码为准）

## 范围决策（开工前用户确认）

1. **档位 = 简化版**（用户拍板）：单张环境贴图 + roughness→LOD 采样替换 `model_pbr` 的
   常 ambient 项 + 天空盒段。**不阻塞未来升级**——cubemap 上传路径 + mipmap 链 + IBL 采样
   入口 + 天空盒段 + 参数 slot 全部可复用到完整版（pre-filtered/BRDF LUT/HDR）；唯一
   重做的是 frag 能量补偿常数（几行 shader，非架构债）。
2. **mipmap 链要补**（用户拍板）：环境贴图生成 mip 链 + skybox 专用 sampler
   （`mipmapMode=LINEAR` + maxLod），roughness→LOD 采样质量正确。
3. **shadow map 暂不做**（用户拍板）：forward shadow 不在 Phase 7 范围（Phase 7 = 延迟
   渲染独立分支），是项目第一个每帧多 pass 带依赖——留待 Phase 7 后再议（或届时作为
   Phase 6 RenderGraph 的多 pass 用例）。本 phase 不排。
4. **Phase 6 不顶替**：Phase 6 主题 = Lua 渲染意图（RenderGraph），IBL/天空盒是光照内容
   问题（Phase 5 延伸），主题归属不同；天空盒段显式留 Phase 6 `Pass` 抽象接缝。
5. **phong 线不动**：基础 Phong 路线保留常 ambient（延续 Phase 5"双路线"定位），IBL 只
   接 pbr 线。
6. **钉死边界**：无 HDR/tone mapping（截图维持 8-bit UNORM）；无 pre-filtered/BRDF LUT
   （完整版留 Phase 7 延迟 light pass 时做）；单张环境贴图；equirect→cubemap CPU 转换
   砍掉（素材为直接 6 面格式，见下素材定案）。

## 素材定案（2026-09-01 像素接缝分析，子 agent Pearson 相关实测）

两套 6 面 cubemap PNG，均 8-bit RGB 无 alpha（stb 现成路径解码 RGB→RGBA8）。
Vulkan 层序 0-5 = `+X, −X, +Y, −Y, +Z, −Z`。

### assets/nightSky/（2048²/面，真实夜景，IBL + mipmap 链验证）

| 文件 | 面 | 文件 | 面 |
|---|---|---|---|
| 1.png | +X 右 | 4.png | −Z 后 |
| 2.png | +Z 前 | 5.png | +Y 上 |
| 3.png | −X 左 | 6.png | −Y 下 |

**上传顺序：`1, 3, 5, 6, 2, 4`**（全部原样，无翻转/旋转）。定案唯一、无歧义：
4 侧面环序 `1→4→3→2` 边缘相关和 3.904（次优环 2.845，反向序 −0.481 = 无镜像歧义）；
8 条上下底面接缝全 ≥ +0.92（和 7.716 vs 次优假设 0.972）。

### assets/rainbow/（512²/面，彩色测试图案，面序/UV 朝向验证）

`up`/`dn` 命名正确（+Y/−Y），但 **4 个侧面文件名整体偏 90°**（像素环向
`bk→rt→ft→lf` 与命名暗示的环向是镜像关系）。

**上传顺序（主方案 B）：`ft, bk, up, dn, rt, lf`**（全部原样，无变换）。
余 180° 方位歧义（B 与 A 为 180° 孪生，12 条接缝数据完全平手，256 行条带 B 弱优
3.2%）——数据无法消除，**批次 1 验收 llm 截图视觉确认**；若发现方位错 180° 只需改
scene.lua 面文件列表顺序（配置显式列表 = 零代码改动）。

### 充分性结论

够用：rainbow（快速迭代 + 面映射一眼验证，mip 链 10 级）+ nightSky（真实 IBL 氛围 +
2048 mipmap 12 级，96MB 全 mip ≈128MB，实测 RADV 正常）。**缺亮场景 HDRI**（验证 IBL 亮条件 + 带太阳特征可
暴露方位错误）——不阻塞 5.5，留后续补。

## 批次与验收

### 批次 1 — cubemap + mipmap 纹理基建

- `texture_pool.cppm` 新增 `uploadCubeMap`（module-private）+ `loadEnvCubeMapFiles`（export，
  stb 解码 + 方形/2 幂/同尺寸校验）：目标图 `e2D + eCubeCompatible flag + arrayLayers=6`
  （Vulkan cubemap 无独立 imageType——`VK_IMAGE_TYPE_CUBE` 不存在，2026-09-01 踩坑修正认知）
  + `mipLevels=1+⌊log2 N⌋`（2048→**12**、512→**10**）+ usage `eSampled|eTransferDst|
  eTransferSrc`；mipmap 链逐 level × 6 面 `vkCmdBlitImage` **filter=NEAREST**（sRGB 格式 blit
  LINEAR 违 VUID-vkCmdBlitImage-filter-00239，驱动直接拒绝——线性空间 box 降采样留作质量
  接缝）；view `eCube` + `subresourceRange={eColor, 0, L, 0, 6}`；SAMPLED_IMAGE 描述符写
  heap slot。
- **env 存储 = Resources 场景级单例**（`std::optional<TextureAsset> envAsset` +
  `emplaceEnvTexture/getEnvTexture`），**不进 AssetTable::textures**——env 绑场景不绑材质，
  不占纹理 id 空间（白纹理 id 0/首槽契约零影响）。`TextureAsset` +`mipLevels` 字段。
- **skybox 专用 sampler**：`Resources::ensureSkySampler(mipLevels)`（sampler heap
  reserved+1 槽，min/magFilter=LINEAR + mipmapMode=LINEAR + maxLod=L−1 + CLAMP_TO_EDGE，
  create info 稳定存储成员），不动 `ensureSharedSampler`（模型纹理路径零影响）。
  uploadCubeMap 尾部自动调用（mip 数与 maxLod 天然同步）。
- **linear staging 三坑（RADV 实测 2026-09-01，最终方案 = 6 张独立 2D linear staging）**：
  ① VUID-VkImageCreateInfo-tiling-02260 linear 必须 mipLevels=1；② CUBE_COMPATIBLE flag
  linear 不支持（cube-ness 只需目标图有）；③ **RADV linear 图 maxArrayLayers=1**
  （arrayed linear FORMAT_NOT_SUPPORTED）→ 单 cube staging 方案整体不可行，per-face 独立
  staging + 6 次单 region `vkCmdCopyImage`。
- 内建**拷贝金丝雀**（texelCanaryCheck 模式）：逐面回读 mip0 (0,0)/中心 texel 比对 CPU 源，
  不符 = ERROR 日志（"时好时坏"→"启动必炸"）。
- CMake：12 张 PNG `copy_if_different` 进 `build/vulkan_practice/skybox/<dir>/`
  （`copy_skybox` ALL target，`SKYBOX_DIRS` 列表驱动）。
- main 临时验证门 `RGL_TEST_SKYBOX=nightSky|rainbow`（面序映射表内嵌，批次 3 正式接线后删）。
- **验收（2026-09-01 实装 PASS）**：rainbow 512²×6 mips=10 + nightSky 2048²×6 mips=12
  金丝雀均 PASS、VVL 零 error、env res slot 3045 + sky sampler slot 17（sampler heap
  reserved+1，共享 sampler 之后）。面序/降采样**视觉**验收留批次 4（skybox 渲染路径）。

### 批次 2/3 — IBL frag + 参数 + 管线接线（2026-09-01 实装 PASS，合并实施）

两批次在实现上强耦合（frag 采样 = binding + 参数消费），合并为一批落地。

- **`shaders/env.slang`（新）**：环境 cubemap bindless 契约——`TextureCube<float4> envMap`
  （binding 4，resource heap SAMPLED_IMAGE 槽）+ `SamplerState envSampler`（binding 5，
  sampler heap sky sampler 槽）+ `readEnvParams()`（读 scene data @48 envIntensity /
  @52 envMipRange）。独立文件（不并入 scene.slang）：phong/pbr/unlit 不 import → 其 SPV
  不含 binding 4/5，管线多烘焙 = 空转（Phase 3/5 既证行为），反向（声明未烘焙）才危险——
  按需 import 精确隔离。
- **`shaders/model_pbr_ibl.frag.slang`（新）**：model_pbr 全套 direct light（GGX）不变，
  仅 ambient 项替换为 IBL：`envDiffuse`（最高 mip ≈ 环境平均，irradiance 近似）× albedo
  ×(1−metallic) + `envSpec`（反射向量 + roughness×mipRange LOD）× Schlick-F ×(1−roughness)
  能量补偿，× envIntensity。无 pre-filtered/BRDF LUT（完整版留 Phase 7）。
- **参数通道**：`SceneFrameData.envParams`（glm::vec2）→ drawFrame staging 写 @48
  （`kSceneDataEnvOffset = sizeof(LightDataGpu)`，static_assert 钉死 ≤64 头内）；
  `SceneEnv` registry context（scene.cppm，仿 Light）；`renderListSystem` 每帧填
  （本地 entt 3.16 ctx **无 try_get**，契约固定 main 恒 emplace + 直接 get，踩坑记录）。
- **scene.lua `environment` 段**（scene_config EnvironmentConfig：dir/faces[6]/intensity/
  skybox，缺段 = 不启用）+ pipelines 表 model_pbr_ibl 条目 + defaultSceneConfig/findSpec
  回落表同步。
- **管线接线**：`RenderPipeline` 构造参数聚合为 `PipelineBindings` struct（env 追加后
  13 参膨胀，数据驱动 cube/model/IBL 形态）+ mappings 4→6（binding 4/5，env sampler 走
  heapOffset 字节偏移规则同 binding 2）；`addModelEntry` 查 `res.getEnvTexture()` 恒烘焙
  （env 槽全局单例不进组合去重 key；未上传 = 不烘焙）。
- **main 接线 + 两条契约**：① **时序契约**——env 上传必须在模型顶点槽 + 纹理槽分配完
  之后、addModelEntry 之前（allocSlot 单调游标，插队挤占白纹理契约槽 → 启动报错，
  2026-09-01 实测踩坑后定）；② **风格契约**——pipelines/styleMap 引用 model_pbr_ibl 但
  无 environment 段 = 启动明确报错（否则 VVL 抓 "no mapping for envSampler"，运行时未定义
  采样）。
- **验收（全 PASS）**：cottage 392 primitive 全 pbr→pbr_ibl 渲染正常；IBL on/off
  （model_pbr ↔ model_pbr_ibl）diff=**57650px**（6.3%，恰模型区域）；intensity 1.0↔0.0
  diff=**48999px**（参数响应实锤）；env 关 + pbr_ibl → 启动契约报错（main.cpp:275）；
  VVL 零 error；cubemap 金丝雀 PASS。

### 批次 4 — 天空盒（2026-09-01 实装 PASS）

- **`shaders/sky.slang`（新）**：`SkyVertexOutput`（position + ndc 插值器）+
  `skyClipToWorld(clip)`（读 scene data @64 invViewProj 四列，列组合变换）。射线重建
  免 resolution——mesh 端把大三角 NDC xy 作插值器透传（重心插值 = 像素 NDC），frag 取
  near(z=0)/far(z=1) 两点差 = 世界视线方向。
- **`shaders/skybox.mesh.slang`（新）**：单 group 输出覆盖全屏的大三角
  （NDC (-1,-1)/(3,-1)/(-1,3)，z=1=far），不读任何 buffer。
- **`shaders/skybox.frag.slang`（新）**：`import sky + env`，射线采 envMap，alpha=1。
- **scene data 头 64→128**：`kSceneDataSkyOffset=64`（invViewProj mat4 列主序），对象矩阵表
  自 @128（`scene.slang` readObjectMatrix base 64u→128u，双源字面量同步）。static_assert
  钉死 `skyOffset + sizeof(mat4) == headerBytes`。
- **`SceneFrameData.skyInvViewProj`**（renderListSystem 每帧 `glm::inverse(proj·view)`，
  ecs.cppm GMF +`<glm/gtc/matrix_inverse.hpp>`）；`FrameDrawList.skyboxStart` 三段边界
  （`[opaque | skybox | transparent]`，与 Phase 6 pass 序列同构）；`SceneEnv` +skybox/
  skyboxPipeline（main 回填）；`RenderListState.skyboxArgs`（skybox 不声明 push，推占位）。
- **`ShaderRegistry::addSkyEntry()`**：单条目（幂等，env 已上传契约），binding 0/1 占位 +
  3 scene + 4/5 env。`findSpec`/`defaultSceneConfig`/scene.lua pipelines 三处接线。
- **`RenderPipeline` mapping 顺序填充修正（关键 bug）**：原固定下标 `mappings[0..5]` 假设
  textured=true——天空盒首个 `textured=false + hasSceneData + hasEnv` 组合下 `mappings[2]`
  留空却计入 count → VVL "sType must be..." + envSampler 被挤出 → "no mapping for binding 5"。
  改 running index `add(firstBinding, offset, stride)` lambda 按实际启用 binding 连续填、
  firstBinding 显式携带。模型条目（恒 textured）行为不变。
- **skybox 段状态**：`skyboxState`（depthWrite=false + depthCompareOp=LEQUAL，大三角 z=1
  只在空像素通过）；drawFrame 三段各段首 applyDynamicState 一次；llm_debug 离屏三段同步。
- **验收（全 PASS）**：rainbow 背景渲染连续（绿地/紫蓝天空/彩虹弧，面序/UV 朝向实锤）；
  nightSky 2048 mips=12 夜景星空 + 星云正确（面序 1,3,5,6,2,4 实锤）；skybox on/off
  diff=863550px（93.7%，整背景切换、模型区不变），OFF 角落回暗 clear (25,25,25)、ON
  蓝天 (84,109,177)；模型不被天空盒遮挡（LEQUAL 深度正确）；VVL 零 error。

### 批次 5 — 资产 + 验收矩阵 + 文档销账

- 完整验收矩阵：rainbow/nightSky × IBL on/off × skybox on/off × 风格切换
  （pbr/pbr_ibl/phong）× cottage 回归。
- AGENTS.md 模块表（vk_resources/texture_pool/shader/shader_registry/vk_renderer/ecs/
  scene_config + shaders/scene.slang 行）+ 已知问题 + 关键文件位置；next_roadmap.md
  Phase 5.5 完成情况销账。

## 验证矩阵（全 PASS，2026-09-01）

| 项 | 判据 | 结果 |
|---|---|---|
| 拷贝正确性 | cubemap 逐面 mip0 (0,0)/中心回读比对 CPU 源 | rainbow + nightSky 均 PASS |
| 面映射 | rainbow/nightSky 背景接缝连续（无断裂/镜像） | 目检 PASS（面序定案实锤） |
| mipmap 链 | 512→10 级 / 2048→12 级，远处无走样 | 目检 PASS |
| IBL 生效 | model_pbr ↔ model_pbr_ibl 截图 diff > 0 | 57650px（6.3%，模型区） |
| env 参数响应 | intensity 1.0 ↔ 0.0 截图 diff > 0 | 48999px |
| 风格契约 | pbr_ibl 无 environment 段 → 启动报错 | main.cpp:275 明确报错 |
| 天空盒生效 | skybox on/off 截图 diff | 863550px（93.7%，整背景） |
| 天空盒深度 | 模型不被 skybox 遮挡 | 目检 PASS（LEQUAL） |
| mapping 顺序填充 | skybox textured=false+scene+env 组合 VVL 干净 | 修固定下标 bug 后零 error |
| VVL | 全流程零 validation error | PASS |

## 工作量与风险

预估 4-8 人天。风险：① CPU 接缝数据与 GPU 采样朝向不一致（以 rainbow 截图先验，错则按
spec 表调 UV 反转变体）；② mipmap blit 在 RADV 下的 cubemap 采样（Phase 3 spike 未覆盖
mipmap——批次 1 验收单测）；③ 三段 drawFrame 状态切换 + llm 同步（Phase 5 双段模式已
验证，增量小）。
