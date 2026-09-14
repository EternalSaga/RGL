# Phase 8 实施计划 — Bistro 打磨（HDR / HDR env / 纹理接缝 / 灯光）

状态：`现行`（2026-09-11 定案）。路线图摘要见 `docs/next_roadmap.md` Phase 8 条目；本文 = 实施批次、
文件级改动清单与验收标准的权威记录。开工前用户追加两条视觉问题（见 §0），据此把"灯光"升为显性主题。

## 0. 视觉问题诊断（2026-09-11，开工前基线截图 + 资产实查）

基线截图：`build/rgl/screenshots/llm_debug.png`（headless forward，远机位）。

| 用户观察 | 实查结论 |
|---|---|
| "bistro 很多面死黑" | 属实。单定向光（intensity 4）+ 占位 env（rainbow 8-bit 草地贴图，IBL 贡献极弱）→ 建筑背光面/屋檐下全部压黑。官方光照配方 = **HDR envMap（san_giuseppe_bridge_4k.hdr）@ intensity 10**（README.txt + BistroExterior.pyscene 石锤）+ **emissive surfaces**（串灯/路灯/霓虹） |
| "bistro 本身带有多光源" | **exterior FBX 无 punctual Light 节点**（strings 实查仅 LightMapUV 通道名；glb `KHR_lights_punctual` 不存在）。官方"多光源"的真实构成 = ① HDR 环境（主光源）② **10 个 emissive 材质**（StringLights 串灯×6 / Emissive_StreetLight / Spotlight_Glass / 药店·书店霓虹，全带 emissiveTexture，emissiveFactor=0.5）——引擎当前完全不消费 emissive |
| "天空盒之前似乎没有引入" | skybox pass **已接线**（forward 图 + environment.skybox=true 均生效，基线截图背景即 skybox），"没有"的实感 = 背景内容是**错误的占位草地贴图**（rainbow PNG），不是官方 HDR 环境 |

结论：死黑与天空盒是同一根因的两面——**主光源（HDR env）从未正确引入**。Phase 8 批次 ②b 直接治本；
批次 ③ 补 emissive 消费（官方多光源元素还原）+ forward 多灯循环（引擎侧能力，手写街灯增强）。

## 1. 用户决策（已拍板）

1. **mip 策略** = compute shader mip 链（新 `mip_gen.comp.slang`，LINEAR 采样，启动期单条命令缓冲批量生成全部纹理 mip；env cubemap 的 NEAREST blit 一并升级）。
2. **codec 栈** = 不追求单头库，vcpkg 方便引入即可。定案：**boost.gil（PNG/JPEG，链 libpng + libjpeg-turbo + zlib）+ FreeImage 仅作 .hdr 单一入口**，删除 stb。
   - FreeImage 3.18.0 实查：`PluginHDR.cpp` 含 `rgbe_ReadPixels_RLE`（专吃 `FORMAT=32-bit_rle_rgbe`，输出 float `FIRGBF`）；vcpkg port 保留 HDR 插件（仅禁 G3 fax）。代价 = 12 个传递依赖进安装图（一次性），引擎编译只多 1 静态库。
   - 排除项：libvips 的 "hdr" loader 实为 OpenEXR（不读 Radiance）；ffmpeg 太重；无主流单头库。
3. **offscreen 变体** = 保持 SDR 不动（DIAG 用途，Phase 8 不碰）。
4. **HDR env 面尺寸** = 2048（16F 含 mip ≈ 268MB 显存，接受）。

## 2. 批次计划

### 批次 ① HDR 累加链（scene_hdr + ACES tonemap）——**完成 2026-09-11**

落地记录：`shaders/tonemap.frag.slang`（Narkowicz ACES，**不带 pow**——sRGB swapchain 硬件编码）；
render_graph 新增契约常量 `kSceneHdrName/kSceneHdrFormat/kFullscreenPassStates`；内建图描述 +
selfCheck 拆出 `render_graph_builtin.cppm`（partition），`compileRenderGraph` 拆 5 helper
（style_check 四项债清零）；StateKit 加 tonemap 态；ShaderRegistry::addTonemapEntry
（mesh 复用 light.mesh.spv，binding 4=scene_hdr/5=共享 sampler）；Resources::ensureSceneHdrSlot；
Renderer `lightPipelineIdx` → `fullscreenPipelineIdx` map（state 名解析）；main 无条件装配
（offscreen 变体条目闲置无害）。验收：forward/deferred 冒烟零 error、VVL 零 error；
baseline `screenshots/baseline_p8_batch1_{forward,deferred}.png`（ACES 后整体略暗属预期，②b env 提亮）。

### 批次 ②a 解码工具链（stb → libpng/libjpeg + FreeImage）——**完成 2026-09-11**

落地记录：`rgl/image_codec.cppm/_impl.cppm`（libpng 简单 API + libjpeg-turbo JCS_EXT_RGBA +
FreeImage FIF_HDR；未用 gil——libpng/libjpeg 直读更简单可靠，内存解码免临时文件）。
texture_pool / llm_debug 迁移；tinygltf `NO_STB_IMAGE` + no-op SetImageLoader（v3 无
resolve_image_dependencies 参数的等价替代）；stb_impl.cpp 删除、vcpkg.json −stb +freeimage
+libpng +libjpeg-turbo +zlib。验收：395/395 像素级全等（stb v2.30@f75e8d1 vs 新解码器，
一次性 harness 复用真实模块代码）；forward 截图与批次 ① baseline **像素全等**；
forward/deferred 冒烟 + VVL 零 error；decodeHdr 实测 4096×2048 float RGB（0..9.8e4 无 NaN，
②b 排雷）。附带修两个工具链鲁棒性洞：doccheck 加 `-fno-implicit-modules` + BMI 预检 + 300s
超时（主构建失败时 dump 退化为全链隐式重建 = 本次 25 分钟"卡死"真凶）；stylecheck 加
BrokenProcessPool 兜底（worker 猝死不再 traceback 阻断 ninja）。

- 新模块 `rgl/image_codec.cppm`（+`_impl`）：`DecodedImage` 值类型（width/height/通道/字节）隔离第三方；
  `decodePng`/`decodeJpeg`（gil）、`decodeHdr`（FreeImage，保留 float；注意 FreeImage 行序 bottom-up 需翻转）。
- 替换点：`texture_pool.cppm`（stb 解码→gil）、`llm_debug_impl.cppm`（`stbi_write_png`→gil png save）；
  核查 `tinygltf_impl.cpp` 的 TINYGLTF_STB 关系后处置（tinygltf 内嵌 stb 则自包含，与本项无关）。
- `vcpkg.json`：+libpng +libjpeg-turbo +zlib +freeimage，−stb；根 CMake link 对应调整。
- 验收（关键）：glb 内嵌 395 张 PNG **stb vs gil 解码像素级逐字节对比**（同会话双构建；PNG 解码确定性 → 应精确相等）；
  换 PNG 编码器后截图文件 md5 必变 → baseline 在 ②a 重建。

### 批次 ②b HDR env 路径（治"死黑+天空盒"主根因；含 loadSceneConfig 拆债）

- CMake：`if(EXISTS Bistro_v5_2/san_giuseppe_bridge_4k.hdr)` → copy 到 `build/rgl/env/`（.hdr 是 gitignore 本地资产，
  缺失时引擎告警 + 回落 legacy 六面 PNG 路径——rainbow 保留为 fallback，干净 checkout 可运行）。
- `configs/scene.lua`：`environment.hdr = "env/san_giuseppe_bridge_4k.hdr"` + `intensity = 10.0`（官方配方；曝光微调配合 ①）。
- `rgl/scene_config.cppm/_impl`：`EnvironmentConfig.hdr` 可选字段；`loadSceneConfig`（175 行）拆债。
- `rgl/texture_upload.cppm/_impl`：`uploadCubeMap` 16F 路径；**CPU equirect→6 面转换**（球面射线 + 双线性采样，
  4096×2048 float → 6×2048² RGBA16F，启动期一次性 ~1-3s）；mip 链走 ③ 的 compute mip 管线（先 LINEAR blit 过渡）。
- 验收：截图目检 = 官方 HDR 街道环境上屏（不再是草地占位）、无极点/±180° 撕裂、暗部显著提亮；
  fallback 路径（删 env/ 文件）回归正常。

**②b 完成（2026-09-11）**：scene.lua `environment.hdr/hdr_size`（缺失告警回落 faces PNG 实测）；
loadSceneConfig 拆段 helper（175 行债清零）；`uploadCubeMap` 泛化核心（CubeFormat）+ 新
`uploadCubeMap16F`（LINEAR mip，float 免 VUID-00239）；texture_upload_impl 超 500 行 → 再拆
`texture_upload_cubemap.cppm`（partition）；texture_pool `loadEnvCubeMapHdr`（CPU 等距柱状→6 面
2048² RGBA16F 双线性重投影，实测 2.12s）；CMake 可选拷贝 .hdr → build env/。16F 金丝雀 PASS、
VVL 零 error、skybox 已换官方 San Giuseppe 桥环境、intensity=10 官方配方。

**关键发现（死黑真因，③ 直接依据）**：glb 131/132 材质 `metallicFactor=1.0`（glTF 默认值，
bistro specular workflow 未写 metallicRoughness 贴图）→ `(1-metallic)=0` 杀死全部 diffuse
（IBL + 直射）→ 即使 intensity 50 立面仍全黑。真实 metallic 在 Specular 贴图 B 通道
（石材 ≈0 电介质）——**批次 ③ ORM 接线后死黑根治**。

### 批次 ③ 纹理接缝 + emissive + forward 多灯（含两函数拆债）

- `rgl/gltf_loader.cppm/_impl`：解析 `normalTextureIndex`、`occlusionRoughnessTextureIndex`（Specular 贴图通道约定
  README 石锤 = **R=Occlusion / G=Roughness / B=Metalness**）、`emissiveTextureIndex`；`loadGltfModel`（209 行）拆债。
- 上传四元组：base + normal + orm + emissive；无 normal 的 primitive 挂**中性法线纹理槽**（(0.5,0.5,1,1)，flip-G 解码不动点）。
- heap：`kUserSlotMargin` 1024 → **4096**（四元组后总槽 ~1200+）。
- `rgl/shaders/model.slang` + `model_schema.lua`：binding 6/7/8（normalTexture/ormTexture/emissiveTexture）
  + descriptorSlots + **`just reflect`**。
- 新 `shaders/normalmap.slang`：屏幕空间**导数 TBN**（bistro 无 TANGENT，0/1591，免重打包）+ **G 翻转**（DX 法线，README 石锤）
  + **Z 重建** `sqrt(max(1-|xy|²,0))`（BC5 无 Z 补偿）；`model_pbr_ibl.frag` 消费：orm 取 G=roughness B=metallic
  （AO 采样不应用）+ **emissive 直接发光项**（`color += emissiveTexture × emissiveFactor`，串灯/霓虹还原——官方"多光源"元素）。
- **forward 多灯循环**：`model_pbr_ibl.frag` 从单光 `readSceneLight()` 改循环 `readLightCount()`/`readGpuLight()`
  （基建已就绪：GpuLight dir+point、kMaxLights=8、@262272；n==1 定向光路径与原公式逐位一致保 diff0）；
  `scene.lua` 手写 street lamp 点光列表（依 emissive 材质位置近似，增强氛围，可选开关）。
- `rgl/shader_registry.cppm`：去重键 (spec, vSlot, base) → (spec, vSlot, base, normal, orm, emissive)，条目 ≤1591 不爆；
  main.cpp C1 循环同步。
- mip 链：新 `shaders/mip_gen.comp.slang` + 启动期批量 CB（LINEAR；全部 2D 纹理 + env cubemap）；
  `ensureSharedSampler` maxLod 0 → 链长。
- `rgl/render_list_impl.cppm`：`renderListSystem`（139 行）拆债。
- deferred 路 emissive：`deferred_geometry.frag` MRT 无空道——当时不做，forward 先行（后于剩余批次 B 以第 5 G-buffer 通道接入，见 §8）。
- 验收：截图目检贴图接缝消失 + 串灯/霓虹发光 + 街灯点光氛围；重建 baseline；heap 容量契约过。

**③a 完成（2026-09-11）**：loader 解析 normal/metallicRoughness 索引（loadGltfModel 215 行债清零）；
`ModelTextureIds{base,normal,orm}` + `ensureNeutralNormalTexture`（(128,128,255) flip-G 不动点，无 normal
primitive 零特判）；model.slang binding 6/7 + schema descriptorSlots + `just reflect`；新 `normalmap.slang`
（`decodeNormalDx` flip-G + Z 重建 + `applyNormalMap` 导数 TBN 退化回落）；model_pbr_ibl + deferred_geometry
消费 ORM（G=rough B=metal，R=AO 采样不应用）；addModelEntry 六参 + 五元组去重（264 条目←1591）；
kUserSlotMargin 4096。**附带修真实 bug**：数据贴图（normal/ORM/中性法线）误按 sRGB 上传 → 硬件解码污染
线性值（G-buffer roughness 恒 0.502 = 0.737 的 sRGB 解码）；uploadTexture 加 `linear` 参走 UNORM。
死黑量化：暗像素 76.5%→33%、均值亮度 0.062→0.568。

**②b 曝光定稿**：env `intensity` 10→**1.0**（官方 Falcor 的 10 是其 irradiance 卷积归一后的校准；简化 IBL
直采 top-mip 平均，env 线性均值 ≈0.56，×10 必过曝）。forward/deferred 曝光现正常。

**③b 完成（2026-09-11）**：compute mip 链。**关键约束发现**：RADV 对 `R8G8B8A8_SRGB + STORAGE_IMAGE` 返回
FORMAT_NOT_SUPPORTED，且 v357 ImageViewCreateInfo 无 usage 成员 → sRGB 图不能作 compute storage 目标。
故**双路径**：UNORM（normal/ORM）走新 `mip_gen.comp.slang` compute 链（传统 descriptor set + 经典 compute
管线，与 bindless 渲染隔离）；sRGB（baseColor）走 **LINEAR blit 链**（Vulkan 1.4 已废 VUID-00239，sRGB blit
现 sRGB-aware，质量不降）。`ensureSharedSampler` maxLod 0→20 + mipmapMode LINEAR。坑记：raii create-info
指针目标须成员化（helper 返回后解引用 = 首跑 segfault）。噪点消除实证（屋顶 speckle 全消）。启动 +~1.5s。
新 baseline `baseline_p8_batch3b_{forward,deferred}.png`。

**③ 剩余（未做，留后续）**：emissive（需扩 ObjectDraw 32→48B 放 emissiveFactor + binding 8 + forward 多灯
循环 + scene.lua 手写街灯）；renderListSystem 139 行拆债。

### 批次 ④ 收尾

- 文档：`next_roadmap.md` 销账、`调试历史.md` 时间线、AGENTS.md（image_codec/normalmap/mip_gen 新模块、
  vcpkg 变更、.hdr 可选资产流、emissive/多灯契约）。
- 全量验证矩阵跑通 + 最终 baseline 定稿。

## 3. 验证矩阵（每批次执行）

1. `just build`：编译零错 + doc_check 零警告 + style_check 相关条目警告清零。
2. headless 冒烟：`cd build/rgl && RADV_EXPERIMENTAL=heap timeout 8 ./rgl` forward + deferred 零 `[error]`。
3. VVL 零 error（含 scene_hdr fp16、16F cubemap、DGC 路径日志正常）。
4. llm_debug 截图：①/②a/③ 各批次重建 baseline；像素级对比仅在 codec 批次用（换编码器文件 md5 失效）。
5. ②a 专项：395 张 PNG 新旧解码器像素级逐字节一致。

## 4. 风险

| 风险 | 缓解 |
|---|---|
| RADV fp16 renderable / 16F 采样行为 | VVL + 冒烟首检；scene_hdr 格式词表已支持 rgba16_sfloat |
| FreeImage 12 传递依赖 → vcpkg 安装时长 | 一次性成本；仅链 1 静态库不进编译关键路径 |
| equirect 极点/环绕双线性边缘瑕疵 | 采样方向向量化 + 环绕 clamp；目检 ±180° 缝 |
| 导数 TBN 屏幕边缘精度 | 无 TANGENT 前提下的已知近似（bistro_pack 重打包留后手） |
| forward 多灯循环改变 n==1 输出 | 循环体定向光分支与原公式逐位对齐 + diff0 回归 |
| baseline 多次重建造成对比链断裂 | 每批次存档截图（同会话双构建方法学见 调试历史 2026-09-08） |

---

## 5. 批次 ④：完整版 IBL（诊断驱动新增，2026-09-11）

### 5.1 诊断结论（实验石锤，非猜测）
用户报"背阴面全局发黑 + 高光油光"。三组受控实验定位根因：
- **normal 贴图**：`model_pbr_ibl` 注释掉 `applyNormalMap` → 屋顶/背阴面**依旧黑** ⇒ 非 normal。
- **metallic**：强制 `metallic=0` → **依旧黑**（NdotL≈0.75 定向光仍打不亮）⇒ 非金属塌缩。
- **envDiffuse 直出**：`return float4(envDiffuse,1)`（ACES 前）→ 全屏 mean_r≈71/255、74% 像素点亮
  ⇒ **环境项本身健康**，不是 env/mip 坏。
- albedo G-buffer（dump_gbuf=0）：墙米白/顶深灰/树黄，明亮饱满 ⇒ 非贴图/loader。

**根因 = 简化 IBL 的漫反射用 top-mip box 平均（方向无关），把明亮天空与暗地面平均在一起**：
上向面（屋顶）与背阴面拿不到"上方是亮天空"的余弦加权辐照度，只拿到全局均值 ≈ 偏暗；
再叠加单定向光 intensity=4 与 env intensity=1 的**大动态范围** → ACES 把暗部压成近黑。
油光 = specular 用 box-blit mip（非 GGX 预滤波）+ `(1-roughness)` 能量 hack（非 BRDF split-sum）。

### 5.2 设计（分两步，先做低成本高收益的 ④a）
**④a（本批，低风险，无新 descriptor/无新 compute 管线）**
- **漫反射辐照度 = CPU SH9**：env 加载时从等距柱状源（`sampleEquirect`）余弦加权投影 9 个
  float3 系数（含辐照度余弦卷积 A0=1, A1=2π/3, A2=π/4 → 输出即 irradiance）。
- **scene data 新尾区**：`kSceneDataIrradianceOffset = kSceneDataDrawTableOffset + 4096*32`，
  容量 9×vec3=108B（对齐到 128B 区）。`kSceneDataBytes` 随之 +128。SH 静态，随每帧 staging
  mirror 一起拷（`SceneFrameData.irradianceSH[9]`，renderListSystem 从 Resources 取）。
- **scene.slang `irradiance(N)`**：SH9 求值（Preetham 闭式或 9 系数显式）。
- **BRDF split-sum = 解析近似（Karis/Epic 拟合，免 LUT 纹理）**：
  `envAmbientSpec = prefiltered(R, rough) * (F0*A(NoV,rough) + B(NoV,rough))`。
- forward `model_pbr_ibl` + deferred `light.frag` 同步替换 ambient 段（同源，保 D1 等价）。
- 能量：先只换公式看效果，再调 `scene.lua` env/directional intensity（纯配置，无重建）。
- **不新增 descriptor binding**（envMap/envSampler 已有，specular 复用它现有 mip 链）；
  specular 暂用现有 LINEAR box mip 作 roughness→LOD 近似（油光部分靠 split-sum 能量修）。

**④b（可选后续，成本高）**：GGX importance-sampled **prefiltered specular env** compute
（新建 compute 管线，cube storage 逐 face/mip，读 mip0 写 mip≥1，同 MipGen 模式）替换 box mip。
仅当 ④a 后油光仍不可接受再做（对"发黑"无关，只影响高光形状）。

### 5.3 接线点（④a）
- CPU SH：`texture_pool` 新 `computeIrradianceSH9(src)`（loadEnvCubeMapHdr 调用，含 PNG 回落
  路径以 faces 近似）→ `Resources::setEnvIrradianceSH(std::array<glm::vec3,9>)` + getter。
- scene data 常量：`vkresources.cppm` 新 `kSceneDataIrradianceOffset` + `kSceneDataBytes` 扩容
  + static_assert；`frame_draw_list.cppm` `SceneFrameData` 加 `std::array<glm::vec3,9> irradianceSH{}`；
  `vkrenderer_impl` writeSceneStaging 加一处 memcpy；`render_list_impl` 从 Resources 填。
- shader：`scene.slang` `readIrradianceSH()` + `irradiance(N)`；`model_pbr_ibl.frag` /
  `light.frag` ambient 段改 SH + split-sum。
- 契约面 = scene data 字面量（scene.slang ↔ vkresources 常量），**无 schema/reflect/heap 改动**。

### 5.4 ④a 完成记录（2026-09-11）
实现：SH9 CPU 投影（`texture_pool::projectRadianceSH9` equirect + `projectFacesSH9` PNG 回落）
→ `Resources::envIrradianceSH`（9×vec4）→ `SceneFrameData.irradianceSH` → scene data 尾区
@656016（`kSceneDataIrradianceOffset`，`kSceneDataBytes` 656016→656144）→ `scene.slang
irradianceSH(N)`（辐照度卷积 π·l0+2π/3·l1+π/4·l2，基函数与 CPU `shBasis` 逐式一致）+
`envBrdfFactor`（Karis 解析 split-sum，免 LUT）→ forward `model_pbr_ibl` + deferred `light.frag`
ambient 段同源替换（旧 envDiffuse box-mip + `(1-roughness)` hack 全删）。

关键坑：ByteAddressBuffer `Load3` 须 16B 对齐 → SH 区改 9×**vec4**（144B）而非 9×vec3（108B）。
scene.slang 是 reflection 输入（scene_forward 管线）→ 改文件触发 reflection_check，
`just reflect` 重跑确认 SceneLight 仍 48B（新增纯函数不动布局）。

效果：forward mean_r 71→80、deferred 80.1（D1 逐像素等价维持）；背阴墙面/背向屋顶全部显色显形、
金属锌顶反射天空不再塌黑、整体未洗白。基线 `baseline_p8_batch4a_forward.png` / `_deferred.png`。
env.intensity 保持 1.0（SH 物理辐照度下曝光合理，未再调参）。

**遗留（可选 ④b）**：specular 仍用 box-LINEAR-blit mip 作 roughness→LOD 近似（非 GGX importance-
sampled prefilter）；金属顶反射偏饱和来自此近似 + 暖色 env。仅当需更准高光形状再做 GGX prefilter
compute。当前观感可接受，暂不做。

## 6. 批次 ③：前向多灯（2026-09-11，完成）

`model_pbr_ibl.frag` direct light 段重构：抽 `shadeDirect`（单灯 GGX，返回 NdotL·(diffuse+specular)，
与 deferred `light.frag shadeLight` 逐式同源）→ `import lights` → `readLightCount`/`readGpuLight`
循环（dir/point 分支 + 线性衰减，同 light.frag）。`count==0` 回落 @0 头单灯（无 Light entity 的
默认场景旧行为，保直接光不丢）。无新 binding（lights 与 scene 共用 sceneData binding 3）。

回归：bistro 单灯 → 多灯循环输出 = ④a 基线**逐像素一致**（3.7M 字节仅 1 处 1-LSB FP 重排噪声），
零回归。多灯基础设施就位；bistro glb 无 punctual 灯故当前无视觉变化，为后续手写灯（scene.lua
lights 多条目）与 deferred 对齐铺路。

### 6.2 ③ emissive（forward 完成，2026-09-11）

glTF 自发光接入 forward 主路径（bistro 102 primitive 带 emissive）：
- **数据流**：`GltfPrimitive.emissive`（loader 解析 `emissiveFactor`）+ `emissiveTextureIndex` →
  `MaterialAsset.emissive`（末尾新字段，去重比较纳入）→ `ObjectDrawGpu.emissive`（@32 float4，
  stride 32→**48**）→ scene data per-object 表 → `scene.slang readObjectDraw().emissive`。
  stride 32→48 令 `kSceneDataIrradianceOffset` 自动后移 656016→**721552**（buffer 721696），
  scene.slang 两处字面量手工同步（stride + irradiance base）。
- **纹理**：emissive 走 sRGB 路（同 baseColor），`ModelTextureIds` 扩四元组（+emissive），
  白纹理回落（白×factor，factor=0=黑）；`addModelEntry` 扩 `emissiveSlot`（六元组去重）烘焙
  **binding 8**（`model.slang` + `model_schema.lua` 各加 emissiveTexture@8，复用 baseColorSampler）→ `just reflect`。
- **消费**：`model_pbr_ibl.frag` return 前 `color += od.emissive.rgb * emissiveTexture.Sample(...)`
  （HDR 叠加，tonemap 链收）。
- **验证**：build 零 error；headless 冒烟 102 emissive primitive、binding 8 VVL-clean 零 error；
  对 ④a 基线仅 **43/921600 像素变（0.005%）**、bbox 落右侧窗/灯区、max Δ157 = 局部自发光显形、零回归。
- **deferred emissive（后续）**：当时 deferred 路径未接（geometry pass 无 emissive、G-buffer 仅 4 通道）。
  → 已于剩余批次 B 完成（第 5 G-buffer attachment，见 §8）。

### 6.3 ④b GGX 预滤波 env（完成，2026-09-12）

金属锌顶锐利镜面条纹 = env 镜面用 box-blit mip（非 GGX 卷积）。治本 = **GGX importance-sample
预滤波 compute**（`env_prefilter.comp.slang` + `env_prefilter.cppm`/`_impl.cppm`，mip_gen 的 cube 变体，
**descriptor heap + shader object、零 pool**）：Karis GGX 采样、256 样本（Hammer 低差异）、src=mip0
cube、dst=第 level 级 `RWTexture2DArray`（2D_ARRAY view，GENERAL）、`roughness=level/(L-1)` 对齐采样端。
接入 `texture_upload_cubemap` 16F 分支（替 LINEAR blit；8-bit sRGB 分支不动；16F cube 图加 `eStorage`）。
验证：RADV heap 可写 storage image 再验可行、VVL 零 error；before/after 金属顶镜面条纹→随粗糙度模糊渐变、
方向正确、非金属逐像素不变。commit `c55c111`。

## 7. Phase 8 剩余 / 后续批次（截至 2026-09-12）

已完成批次：① HDR tonemap、②a codec、②b HDR env、③a ORM+normal、③b mip（→heap）、③ forward 多灯、
③ emissive（forward）、④a 完整版 IBL（SH9 + split-sum）、④b GGX 预滤波 env。descriptor pool 全项目清零
（见 mip_gen 迁 heap，commit `a79b16f`）。

**未做（按优先级）**（1–3 已于 2026-09-13 完成，见 §8；3 = 植被杂色实为蓝染，已由 envBrdf 重拟合 + 光配比重校准收口，见调试历史 2026-09-13 及续条）：
4. **env 存储 RGBA16F→RGBA32F（可选）**：GGX 预滤波后金属反射色带已大减；若仍见半精度量化带，env 改
   32F 存储彻底消除（GGX src 也受益）。低优先。
5. **NVIDIA/Intel 复测**：mip_gen 迁 heap、env_prefilter GGX 均本机 RADV（STRIX_HALO）验证；reserved-range
   跨驱动（NVIDIA reservedSlots≈3024）与 heap storage image 在 NV/anv 未实测，需上机复跑（见 nvidia_compatibility_report）。

## 8. Phase 8 剩余批次 A/B/C（完成，2026-09-13）

**前置——资产元数据回填（石锤：丢失点在 assimp FBX importer）**：Source2 自定义 blend shader
（MASK/双面）无 FBX 标准属性可映射，assimp 导出即丢；bistro 官方亦无 emissive strength 源值
（pyscene 仅 envMap.intensity=10）。`tools/bistro_pack.py` 升级为**修复层**按标准字段回填：
MASK ×10（PIL alpha 直方图扫描判 cutout，`alphaCutoff=0.5`）、doubleSided ×6（名含 `.DoubleSided`）、
`KHR_materials_emissive_strength` ×10（look-dev 常量 4.0）、`extensionsUsed` 按材质实际使用重建
（顺带修 assimp over-declare）。重转新 glb：几何段 + 395 张 PNG 与旧 glb **逐字节一致**，仅 JSON 变。

- **A = strength loader**：`gltf_loader_impl` 新增 `extNumber`（vcpkg tinygltf 老 API：
  `IsObject/Has/Get(key)/IsReal/IsInt`，无 `ObjectFind`），CPU 端把 strength 预乘进 emissiveFactor
  ——shader/ObjectDraw 布局零改动。
- **B = deferred emissive**：`kGBufferAttachments` 4→5（`gbuffer_emissive` RGBA16F）+
  `ensureDeferredGBufferSlots` 5 槽 + geometry pass SV_Target4 = `od.emissive.rgb × emissiveTexture` +
  light pass 相加。**binding 实际布局：4-7 = G-buffer 前 4、8 = 共享 sampler、9/10 = env、
  11 = emissive**（emissive 排尾——4-10 被旧布局占用，勿按 kGBufferAttachments 序推断）。
- **C1 = MASK alpha-test**：ObjectDraw.emissive 的 **w 复用为 alphaCutoff**（0 = 不测，ObjectDraw 仍
  48B、反射 layout 不变）→ `scene.slang ObjectDraw.alphaDiscard(alpha)`；model_pbr_ibl /
  deferred_geometry 两 frag 消费（discard + `SV_IsFrontFace` 法线翻向）。MASK 留 opaque 桶
  （shader 内 discard），不新建 pipeline spec。discard → `OpDemoteToHelperInvocation` →
  features_config.json 增 `Vulkan13::shaderDemoteToHelperInvocation`。
- **C2 = doubleSided**：loader 期**背面副本复制**（绕序翻转 b/c + 法线取反，doubleSided 三角形数 ×2）
  ——动态 cull 状态零改动（盲执行器 / DGC 捕获 / cull 候选全不知情）；与 C1 的 frontFace 翻向
  互补冗余（剔除开时后者恒 false）。
- **验证**：总览机位 D1 = **0（逐位全等）**；街景 D1 = 19px（纯相机时间漂移，背靠背 run 可比性见
  调试历史当日条）；forward/deferred 冒烟零 error；植被 cutout + 树背面目检通过；
  G-buffer 日志 5 槽（albedo/normal/position/metalrough/emissive）。

## 9. 批次 D：掠射角法线质量（方案 A+C，完成 2026-09-13）

**问题**：湿地面 / 植被表面掠射角出白色椒盐噪点。G-buffer 通道分解：position/albedo 平滑
（排除 z-fighting），仅 normal 通道噪声 → normal map 法线源。根因 = `applyNormalMap` 用屏幕
空间导数 Jacobian 造 TBN，掠射角 `det→0` 时 invDet 放大浮点误差 → world normal 逐像素椒盐。

**Filament 标准实现基准**：lit 材质编译期强制 TANGENTS 属性（MaterialBuilder），导入期 CPU 预烘
MikkTSpace，FS 只做一次 `normalize(TBN·n_ts)`，**零 dFdx 造 TBN**；dFdx 唯一正当用途 = specular
AA `normalFiltering()`（Kaplanyan 2016，刻意喂几何法线，glTF 默认 variance 0.15 / threshold 0.2）。

- **A = CPU 烘切线 + 标准 TBN**：assimp 三路（默认 / WEBCGL_GLTF_TANGENTS / via-FBX）均不导出切线
  → `bistro_pack.py` 用 numpy Lengyel 平滑切线（+ Frisvad 退化兜底）生成 glTF 2.0 核心 `TANGENT`
  （VEC3 float32，|len-1|≤4e-8、|T·N|≤7e-7）；loader 读 TANGENT + **逐三角形按 UV Jacobian det
  符号推手性 W**（bistro UV 92% det<0 = 镜像 UV，W 硬编码 +1 会翻错）+ 无属性时 Frisvad 兜底；
  ModelVertex 32→48B（tangent vec4@32，`just reflect` 重生 static_assert）；mesh shader 直通
  tangentOS；`applyNormalMapTBN`（Gram-Schmidt 正交化 + `cross(T,N)·w` 手性）。
- **C = specular AA**：`normalFiltering`（Kaplanyan 2016，喂几何法线 geoN）forward lighting /
  deferred geometry 两 pass 接入。
- **验证**：白噪点随机成分消失（目测：椒盐 → 连续高光）；forward/deferred 冒烟零 error。
- **D1 剪影边缘容差（本批接受项）**：总览 D1 = 0→**29px（0.0031%，全剪影边缘，肉眼不可见）**。
  隔离 = 19px（A 标准 TBN）+ 10px（C normalFiltering）。性质 = **forward（float N）↔deferred
  （oct RG16_SNORM 量化 N）固有精度差在掠射角 Fresnel→1 被 reflect 方向放大**（排除 LINEAR halo：
  G-buffer 1:1 映射 LINEAR 退化成 NEAREST；diff 像素 deferred position 非零 = 覆盖一致、差在
  specular）；**非本批新引入 bug，是 deferred 路线一直存在的固有灵敏度**（street 长期 19px 同属
  此类，其旧"相机漂移"结论系误诊，见调试历史续 5）。强行归零 = normal G-buffer 升 RGBA16F，
  独立批次量级，暂不做。

## 10. 批次 E：植被透射 SSS + 光源本色（forward-only，完成 2026-09-14）

**动机**：全场景共用 `model_pbr_ibl`——植被太平、灯具镀铬球。bistro 材质调研（132 个，金属粗糙度全同质、
无扩展 PBR）：植被 = MASK+doubleSided 6 种（Foliage 叶卡，余为枝干），灯具 = emissive 非零 10 种；
**MASKDS 6 种全是植被**（窗帘/锻铁是 MASK 无 DS）→ 两条路由判据纯数据驱动、零名字匹配。

**路由基础设施现成、零 D1 风险**：`pipelineIndex` 是 per-primitive（=材质粒度）+ `bindPipelineToAsset(name,
assetId)`；deferred **无视 styleMap 恒走 `deferred_geometry`**（main.cpp:435）→ 新 spec 只在 forward 路
按风格派生（`!deferredMode` 门控），deferred 逐位不动。

- **`model_vegetation.frag`（L2 背光透射）**：pbr_ibl 派生（binding/import 严格一致，复用 model.mesh.spv
  + 4 纹理槽），metallic 恒 0；逐灯 `shadeTransmit`：出射向 `T=normalize(-L+0.2N)`、`backLit=pow(dot(V,T),3)`、
  `thickness=0.45+0.55·lum(albedo)` 保底（椴树叶贴深棕区否则死黑剪影），透光色用 `albedo`（albedo² 把深叶
  压得几乎不透光），strength 1.5；**`shadeDirect` 漫反射 = half-Lambert wrap**（`pow(dot(N,L)·0.5+0.5, 2)`）——
  标准 saturate(NdotL) 下单层叶片卡背阴面直接光恰为 0 = 死黑面（真实树冠靠冠内多层散射补亮，单层卡无此
  信息），wrap 把阳光绕到背光侧廉价模拟冠内散射（HF2/UE vegetation 同款），specular 保持标准 saturate。
- **`model_light.frag`（光源本色）**：**数据根因 = 路灯 `baseColor` 纯黑、绿 100% 来自 `emissiveTexture`
  绿玻璃纹理**，`emissiveFactor(0.5)×strength(4.0)=2.0` 乘绿纹理在 emission 主导下 = 纯绿霓虹——金属球版
  是黑金属底把绿压暗才藏住。修复 = emission × `(1/strength)` 还原 glTF 原值 + 保留 F0=0.04 玻璃反射 →
  **暗绿玻璃灯罩**（用户拍板：白天暗绿玻璃，既不霓虹也不熄灭）。
- **接线**：scene.lua/scene_config_impl 2 spec + styles 植被/light 映射；env 契约检查扩两 spec（import env）；
  CMake compile_slang + add_executable 汇总。
- **验证**：forward 建植被条目 12 / 光源条目 19；黄昏逆光 demo（临时压 sun 到镜头正逆光）树冠金黄透亮 =
  **透射机制正确**；白天高 sun `backLit≈0` 透射微弱（物理正确：透光朝正下出射、仰视镜头不在出射下游）；
  绿光霓虹 → 暗绿玻璃确认。

**方法论**：植被参数两轮 strength 调整零响应，手算符号发现 `dot(V, 透光方向)<0`——**调参无效先验证驱动项
是否为 0，别盲调幅度**；"效果不出"一次点积验算比反复迭代便宜一个数量级（详见调试历史续 6）。
