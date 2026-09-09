# Phase 7 开发日志（滚动更新：D1 单灯 → D3 多灯 → culling → 重构批次）

> **状态**：现行（滚动更新，批次 0–5（5=7b GPU culling/DGC，2026-09-09）+ 勘误 + 重构批次 3–5 已入册；测试主场景 Bistro Exterior；D1 = 单灯 G-buffer→light→swapchain 2-pass，D3 = 多灯延迟光照）。
> 实施计划见 [archive/phase7_plan.md](archive/phase7_plan.md)；本文件记过程、踩坑、决策与当前态，供连续性交接。

---

## 2026-09-07 — Bistro FBX→glb 转换

### 素材与目标
`Bistro_v5_2/`（Amazon Lumberyard Bistro ORCA）：Exterior/Interior/Interior_Wine 三个 FBX +
633 张贴图（622 `.dds` + 11 tga）+ 一张等距柱状 HDR env + `.pyscene`（Falcor 场景文件，Blender 不读）。
README 关键：贴图其实是 **metal/rough 工作流**——`*_BaseColor.dds`（RGB=色 A=opacity）、
`*_Specular.dds`（**R=Occlusion G=Roughness B=Metalness**，命名误导）、`*_Normal.dds`（DirectX）、`*_Emissive.dds`。
→ `Specular.dds` 逐通道**正好等于** glTF `occlusionTexture`(R) + `metallicRoughnessTexture`(G,B)。这是转换能"近乎无损"的根因。

### 工具链决策（踩坑记录）
- **Blender 直接导**：失败。老 io_fbx 读不动这份 120MB 二进制 FBX（"解析不了好多数据"）。
- **assimp CLI**：`assimp info`/`export` **能完整读**（1306 节点 / 1513 mesh / 2.8M 三角 / 132 材质，4.2s）。
  但 `export ... .glb` 把贴图**以 dds 原样嵌入**（mimeType dds）+ 材质塌成 `KHR_materials_specular` + 标量 rough/metal；
  `export ... .gltf`（分离式）**只按文件名引用 dds**（`Textures\X_BaseColor.dds`）不转码。都不达标（项目 stb 只解 png/jpg）。
- **FBX2glTF**（godotengine fork v0.13.1，`/tmp/opencode/fbx2gltf/`）：几何/层级/属性最干净
  （POSITION+NORMAL+UV 齐、层级保留），但贴图嵌成**无头 BCn 裸块**（mimeType image/unknown，stb/Blender 都不认），
  metal/rough 塌成统一 0.4/0.3。不达标。
- **最终管线（达标）= assimp 分离 .gltf 骨架 + 自写 ffmpeg 打包**：
  1. `assimp export BistroExterior.fbx out.gltf -f gltf2` → 几何(out.bin) + 按文件名引用 dds 的 .gltf；
  2. `tools/bistro_pack.py`：ffmpeg 解 BCn dds→png（`ffmpeg -i x.dds -vf scale=..:force_original_aspect_ratio=decrease x.png`，
     ffmpeg dds decoder 支持 BCn），按文件名 stem 重接：BaseColor→baseColorTexture(sRGB)、
     Normal→normalTexture（**绿通道翻 + BC5 Z 重建留 Phase 8**）、Specular→metallicRoughnessTexture+occlusionTexture（同图）；
     删 `KHR_materials_specular`；丢弃未引用图；重建**单 buffer** .glb。
  - 产物 `assets/bistro_exterior.glb`：**424MB**（1024 贴图，原生 2K 传参 `0` 重跑约 1GB）。
  - 校验：395 图全 image/png；131/132 材质 base+normal+metalRough+occlusion 齐、零扩展、factor=1（1 个酒瓶标签材质无 ORM 源图，留 assimp 标量 factor 0.0/0.553）；
    1591 primitive 全有 POS+NORMAL；节点层级保留未烘；bufferView 无越界。**Blender 材质预览目检通过**（贴图/位置正确）。
- 未做：法线绿通道翻 + BC5 Z 重建（引擎采样法线时补）；Interior/Wine 同法可转。

### 关键认知
- "FBX 读不了" = 工具问题不是资产问题；Blender 老 io_fbx 不行，assimp/FBX-SDK 系可行。
- tinygltf 和 gltf_loader **根本不碰 FBX**；转换是上游工具的事。
- dds→png 体积暴涨（BCn 2K→PNG）；1024 是体积/性能档，与"渲染能力"无关，可一键回 2K。

---

## 2026-09-07 — 批次 0：loader 节点 TRS + Bistro 进引擎（forward 测通）

### loader 节点 TRS（不烘焙）
- 动机：`gltf_loader` 旧逻辑只取"首个引用 mesh 的 node 的 translation"，丢 rotation/scale/父链（Phase 2 遗留）。
  Bistro 1300 节点 / 深 6 层 + matrix 节点（Sketchfab 轴旋转 + 平移散布），不补则散架。
- 改动 `gltf_loader.cppm`：
  - `GltfPrimitive.translation(vec3)` → `nodeWorld(mat4)`；
  - 解析后**先算全节点世界矩阵**：localTRS（`node.matrix` 列主序 `make_mat4` 优先，否则 T·R·S，rotation 是 glTF xyzw→`glm::quat(w,x,y,z)`）；
    `nodeParent` 由 children 反查；world[n]=world[parent]·local[n]，**迭代加深**求解（避免递归栈/std::function）。
  - 包围球按 nodeWorld 变换顶点。
  - GMF 补 `#include <glm/gtc/{matrix_transform,quaternion,type_ptr}.hpp>` + **`#include <fmt/format.h>`**
    （原 fmt 靠传递可见性蹭到，加 glm include 断链 → clang 报"missing #include"；用则 include，教训）。
- `model_importer_impl.cppm`：`local = prim.nodeWorld`（原 translate(translation)），`world = rootTransform · local` 不变。
- **踩坑（已修）**：迭代解析器全解析完成后，下一趟无 progress 会**误报 cycle**（没区分"全完成"vs"卡住"）→ 加 `resolved` 计数，
  全完成正常 break，仅停滞且有未决才算环。kyoto/grass 首跑即触发此假警告，暴露 bug。
- **正确性验证**：grass_variations radius 81→264、kyoto center 微移——查实是**修 bug**：grass 4 丛草本在模型内
  Z 轴 ±164 分布（node3/5/9 平移 matrix），旧 loader 全压原点重叠；现按节点摆位=真实散布。kyoto "Model" 节点 matrix≈旋转对消 identity，基本不变。

### heap 容量
- 每 primitive 一个 object 矩阵槽；Bistro 1591 > `kSceneMatrixCapacity(1024)` → 启动契约报错。
- **上调 `kSceneMatrixCapacity` 1024→4096**（`vkresources.cppm`）；scene data 缓冲随之 256KB（128 头 + 4096×64），staging 同步。无压力。

### forward 冒烟（Bistro）
- scene.lua：entities 单条 Bistro `scale=0.01`（厘米→米）；kyoto + 36 grass 全注释保留便于恢复。
- CMakeLists `MODEL_GLB_SOURCES` 加 `bistro_exterior.glb`（改 CMake 后 build 自动 reconfigure）。
- 结果：**1591 primitive 正确拼装**（楼体/窗/拱门/屋顶/树/路灯/环形路面对位），零 error 零 warn，llm_debug 干净退出。
  17 块拼接顶点 buffer + 395 纹理槽（<1026 heap capacity，1.5GB 纹理运行时上传 ~15s）。
- **config 同步坑**：只编辑源码 `configs/` 不 `just build` → build 目录仍是旧 config（copy_if_different 需构建触发）；改 config 必先 build 再跑。

### 关键发现：Bistro 材质在 forward 下偏暗（正当化 D1）
转换把 metallic/roughness **factor 设 1.0**（让 ORM 贴图说话），但 forward 着色器**只吃标量、不采 ORM 贴图**
→ 全 Bistro 当纯金属、无漫反射 → 画面暗。**这是 Bistro 需要延迟管线的实证**：deferred light pass 采样
metallicRoughnessTexture(G/B) + normal 才正确。forward 暗=预期非 bug。

### 相机
- Bistro 世界包围球 center≈(0.14,0.09,0.02) r≈1.17（scale 0.01 后）；但包围球被大地面撑大，
  建筑簇相对小——机位需贴近建筑（见 camera.lua 注释）。trackball 用 position/target 原值。

---

## 2026-09-07 — 批次 1a：offscreen 显式格式贯通（子 agent 实现 + 主 agent 验收）

- 从此处起改「主 agent 定规格 + 派 worker 子 agent 实现 + 主 agent 独立验收」串行模式（全局规范要求子 agent 串行，勿并行）。
- 核实：`ensureRtAttachment` 早已吃任意 `vk::Format`（usage 含 SAMPLED）、`RgAttachmentDesc.format` 字符串全链路传递——
  **唯一硬阻塞**是 vkrenderer_impl.cppm:370 的「非 inherit 即拒绝」。改动面极小。
- 落地（子 agent，4 文件）：render_graph 词表 `isKnownRgFormat`/`resolveRgFormat`（表驱动 12 项 RGBA/RGB/RG/R 精度）+
  compile 校验（offscreen 查词表、swapchain/depth 强制 inherit）+ 执行器删拒绝改 resolver + self-check 两用例
  （显式格式 compile OK / 未知格式报错）+ 词表值回读断言（守拼写错，好补充采纳）+ render_graph_config Lua format 前置校验。
- **主 agent 独立验收**：复核 diff 属实；自跑 `just build` 绿；headless 冒烟 EXIT=0 零 `[error]`/零 VVL、self-check（含新例）全过。AGENTS.md 边界 ③ 已同步。

---

## 2026-09-07 — 批次 1b/2 核心设计定案（主 agent 亲自，读契约后）

**MRT 地基**：执行器 `beginRendering` 已逐条吃 `pass.colors`（vkrenderer_impl.cppm:452-462）；shader object 无
VkPipeline 级 colorFormats 锁定 → 多 RT 天然支持，管线创建零改。`PipelineSpec = {name, meshSpv, fragSpv}`
（scene_config_impl.cppm 默认表）——deferred_geometry 复用 `model.mesh.spv`，只加写 MRT 的 frag，绑定面
（binding0 顶点/1 baseColorTex/2 sampler/3 sceneData）与 model_pbr 完全相同（`model.slang` + `scene.slang`）。

**材质现实（重定 D1 判据）**：引擎材质只带 **baseColor 贴图 + 标量 metal/rough + 顶点法线**（`model.slang` 仅声明
binding1 baseColorTexture，`MaterialAsset` 单 textureId；无 ORM/normal 贴图绑定）。采样 metalRough/normal **贴图 = Phase 8
纹理管线活**。→ deferred 与 forward 用同一套材质数据 → **D1 判据 = forward/deferred 单灯着色等价性回归**，非"调好看"。
Bistro 偏暗 = metallic 标量=1 的正交问题（packer CPU 取 ORM 均值写标量可修，两路同受影响，不阻塞 D1）。

**最硬整合点的优雅解（forward/deferred 互斥）**：几何 pass 让每资产用 deferred_geometry 管线，
**不需要 per-draw 管线变体机制**——deferred 模式下 opaque 的 pipeline 规格从 `model_pbr` 换成 `deferred_geometry`
（经 styleMap / startup 模式），`MeshAsset.pipelineIndex` 天然指向 G-buffer frag；执行器仍盲执行同一条
`FrameDrawList`（`DrawCall.pipeline` 已是表下标，frame_draw_list.cppm:20）。

**D1 最小形状 = 2 pass**（省独立 lit RT / composite）：
1. **geometry pass**：drawSource=opaque，4 color RT（RT0 albedo `rgba8_srgb` / RT1 world-normal oct `rg16_snorm` /
   RT2 worldPos `r32g32b32_sfloat` / RT3 (rough,metal) `rg8_unorm`）+ depth。deferred_geometry.frag 从 model_pbr 派生、
   不算光照只写 4 目标（albedo=baseColor.rgb·baseColorTexture.Sample；normal=oct(worldNormal)；
   worldPos=worldPos(M,positionOS)；metalRough=drawArgs.roughnessMetallic 标量）。
2. **light pass**：全屏 draw（仿 skybox 单 group 大三角，新增 `light` PipelineSpec），sampleRead 采 4 RT + 单灯 GGX +
   tonemap → 直写 swapchain（1 color，present 转换由图 presentTransition 派生）。透明段 D1 暂跳（deferred+alpha 单独议题）。

**唯一真瓶颈 = AGENTS 边界 ④「sampleRead RT 描述符未接 heap」**（light pass 采样 G-buffer 的前提）——核心，主 agent 设计。

**分工**（用户确认「核心亲自写，只把干净邻接片派子 agent」）：核心（sampleRead 接线 + 各 frag + 图选路）我写；
干净邻接片攒到不冲突时机派 worker——**① Bistro packer 标量修正**（`tools/bistro_pack.py` CPU 取 ORM 均值写
metallic/roughness 标量、可只 patch 材质不重编码贴图）、② AGENTS/devlog 文档同步、③（deferred 跑通后）forward/deferred
截图 diff harness。

---

## K2 进度（主 agent 写，均编译验证、行为中性）
- ✅ **K1**（keystone 采样接线）：`RtAttachment` 增 SAMPLED_IMAGE 描述符链（heapSlot 首建 allocSlot、跨 resize
  复用；`imageDesc.pView→sampleViewCreate`、`resourceDesc.data.pImage→imageDesc` 指针链存 map 节点）+
  `ensureRtAttachment` create/rebuild 分支写入 + `rtHeapSlot(name)` 暴露。验证：forward 零 error（不建 RT）/
  `RGL_OFFSCREEN=1`（建 scene_rt 走写入）EXIT=0 零 VUID。
- ✅ **K2a 着色器 + CMake**：`deferred_geometry.frag.slang`（4 MRT：albedo rgba8_srgb / normal rg16_snorm oct /
  worldpos r32g32b32_sfloat / metalrough **rgba8_unorm** .r=metal .g=rough；复用 model.mesh + scene 对象矩阵）、
  `light.mesh.slang`（全屏大三角带 uv）、`light.frag.slang`（binding 4-7 采 4 RT + 8 sampler + 3 sceneData，
  octDecode，GGX 逐式同 model_pbr，无 tonemap 保可比）。CMake `compile_slang` ×3 + `add_executable` 登记。
  踩坑：frag 漏 `[shader("fragment")]`→E57004 no exported symbols（补）；oct 的 `?:` 向量条件 E30056→`select()`。
- ✅ **K2b-extra 机制**：`PipelineBindings.extra`（`std::vector<PipelineExtraBinding>{binding,sampler,heapSlot,
  samplerHeapOffset}`）+ `RenderPipeline` base mapping 后追加循环（`mappings[6]`→`[16]`）。light 管线据此映射
  4 RT 采样槽 + sampler，不动既有 cube/model/skybox 管线（零风险）。

## D1 打通（2026-09-07，`RGL_DEFERRED=1` 端到端跑通）
K2b-core 全部落地（render_graph drawLight+deferredRenderGraphDesc+self-check / shader_registry addLightEntry+
findSpec light+deferred_geometry / 执行器 drawLight 分支+lightState+lightPipelineIdx+validate / main 选路+
RT ensure 下移 Renderer+opaque spec 换 deferred_geometry / Resources ensureDeferredGBufferSlots）。**结果**：
`RGL_DEFERRED=1` headless 主渲染 **零 error**，截图 = Bistro 建筑群正确拼装 + **方向光照**（立面受光/屋顶背光/
地面/秋树），deferred 2-pass（geometry→G-buffer→light→swapchain）成立；forward 默认路径 diff 0（零 warn/error）。

**两处 VVL 修复**（deferred 首跑暴露）：
1. worldPos RT `r32g32b32_sfloat` **不支持 SAMPLED_IMAGE**（3 通道 float 格式采样位缺失，VK_ERROR_FORMAT_NOT_SUPPORTED）
   → 换 `rgba32_sfloat`（desc/ensureSlots 同步；deferred_geometry SV_Target2 改 float4，light.frag positionTex 改 float4 .xyz）。
2. `applyDynamicState` 只对 color attachment **0** 设 blend enable/write mask → MRT 4 附件触发 VVL 07476/07478
   → 加 `colorAttachmentCount` 尾参（默认 1，forward/llm 不变），blend/mask 覆盖 `[0,count)`；geometry pass 传
   `pass.colors.size()`。

**已知非致命项（留后续，非 D1 核心缺陷）**：
- **llm_debug captureFrame forward-only**：deferred 模式它把 4-MRT deferred_geometry 管线渲进**单附件**离屏 RT
  → VVL WARNING「Location 1/2/3 写入无 pColorAttachments」（severity=warning、非 error、不污染主 drawFrame，
  headless 截图实读 swapchain 后缓冲 = 正确 deferred 输出）。正解 = captureFrame 在 deferred 下不重渲（走 swapchain）。
- **forward↔deferred 精确等价未比**：deferred 截图观感比 forward（Bistro metallic 标量=1 偏暗）亮且受光好，
  疑似 metalRough RT 往返或 light 读取与 forward 标量路径有差 → **K3 同机位 forward vs deferred 逐区比对**定位。

## 当前态 / 下一步
- ✅ D1 核心：deferred 管线端到端跑通（geometry→G-buffer→light→swapchain），主渲染零 error，forward diff 0。
- ✅ **K3** 已完成（见"K3 定位与修复"节）：同机位 forward↔deferred 单灯截图逐区比对查出 2 真 bug（light 全屏大三角
  uv 翻转、light pass 深度全剔），均已修；背景缺失 = 图无 skybox pass、整体偏暗 = metallic 内容问题，均证实非渲染 bug。
- ✅ captureFrame deferred 路径（不重渲/读 swapchain）退役 = 重构批次 5 帧内回读（见"重构批次 5"节）。
- 待办：Bistro 法线翻绿通道 + BC5 Z 重建 + ORM 贴图采样（Phase 8 纹理管线）；Interior/Wine 转换（按需）；
  worker 干净邻接片（Bistro packer 标量修正 / forward-deferred 截图 diff harness）待不冲突时派；
  deferred 图 skybox/env 背景合成（K3 下一步选项 (a)，按需）。

## K3 定位与修复（2026-09-07，forward↔deferred 比对查出 2 真 bug + 2 非 bug）
先修 captureFrame 让它 deferred-aware（`renderer.isDeferred()`）：**自包含 2-pass** 截图——同一命令缓冲内
geometry pass 渲入 4 G-buffer RT（capture 尺寸 == swapchain extent 命中已建 GB，名字/格式与
`ensureDeferredGBufferSlots` 契约一致），再 light pass 全屏采样 GB 渲入单捕获 8-bit RT（前向/延迟同 RT 可比）。
**不能跨命令缓冲读持久 GB**（实测上帧 GB 对独立提交读回黑——内容未建立跨提交可见性）。加 `RGL_DUMP_GBUF=N`
descriptor-无关直读第 N 个 GB RT（copyImageToBuffer）。

逐层 dump 定位（albedo GB 完美 = Bistro 全 albedo 正确入缓冲）：
1. ✅ **翻转 bug**（`light.mesh.slang`）：light 全屏三角 `uv.y = ndc.y*0.5+0.5` 与 geometry 的负高 viewport
   （framebuffer Y 翻转）写入 GB 的取向反 → 采样上下颠倒。改 `uv = float2(ndc.x*0.5+0.5, 0.5 - ndc.y*0.5)`
   （uv.y = framebuffer_y/height，uv.x 符号本就对）。
2. ✅ **light pass 全剔 bug**（`vkrenderer_impl` lightState）：`RenderState` 默认 `depthTest=true + eLess`，
   lightState 只改了 depthWrite/cullMode → 截图路径 light pass 绑了清 1.0 的深度附件 + 大三角 z=1.0 →
   `1.0<1.0` 全深度剔除 → 截图纯 clear（0.1 灰）。加 `lightState.depthTest = false`（全屏终图本就无深度；
   主图 light pass 无深度附件本不受影响，此改让两路一致 + 语义正确）。
3. ⚠️ **非 bug — 缺 skybox 背景**：forward 的"绿色山丘"经 [K3-DIAG] 段计数（opaque=1591=全部 Bistro 几何、
   transparent=0）+ 直读 `skybox/rainbow/rainbow_ft.png` 证实 = **env cubemap 背景**（草地+湖+彩虹），非 Bistro 几何。
   deferred 图无 skybox pass → 背景黑。D1 核心（G-buffer + 单灯）只渲 opaque，背景/透明 deferred 化属后续。
4. ⚠️ **非 bug — 整体偏暗**：Bistro 材质 metallic 标量=1（packer 把因子设 1 让 ORM 贴图驱动、但两路都还没采 ORM）
   → diffuse 被 `(1-metallic)=0` 杀掉 → 两路都暗（forward 建筑同样暗）。正交内容问题（Phase 8 采 ORM / packer 标量）。

**净结果**：deferred 建筑本体光照与 forward 一致（同 GGX 抄自 model_pbr）；两张差异 = skybox 背景缺失（图未含
skybox pass）+ metallic 暗（内容）。**下一步选项**：(a) deferred 图加 skybox/env 背景合成（视觉补齐，需处理
light pass 与背景的深度/合成顺序）；(b) 换亮 diffuse 场景（cube）跑 forward↔deferred 像素比对证等价；(c) 先提交 D1。

---

## 批次 3（2026-09-07）：多灯 + 灯数据并入反射管线（D3）

**设计核心决策**：scene data **@0-128 头逐字节不动**（`LightDataGpu@0` 单灯 = 主光、env@48、sky@64、
对象矩阵表@128），灯数组**追加于对象矩阵表之后** @`128 + 4096*64 = 262272`。收益 = forward/env/sky 三 shader
**零改动 diff0**（forward 仍 `readSceneLight()` 读 @0 主光），只新增延迟灯路径读取。代价 = scene data 缓冲
+528B（`kSceneDataBytes` 派生自动扩容，buffer alloc + descriptor range + staging + copy region 全用常量，无硬编码）。

**第 4 条反射管线（lights.slang + lights_schema.lua → lights_types_generated）**：
- `GpuLight`（4×vec4=64B：direction/position/color(×intensity 预乘)/params(.x=range .y=type 0dir/1point)）
  经 schema `vertexLayouts` 生成（`vertexLayouts` = 纯 schema 声明的 struct 通道；反射 JSON `parameters` 只出全局
  资源不出函数返回 struct，故无重复）+ schema `constants` 生成 `kMaxLights=8`/`kGpuLightStride=64`/
  `kSceneDataLightArrayBase=262272`。
- **三方同步 static_assert**：`vkresources.cppm` 计算 `kSceneDataLightArrayOffset = kSceneDataHeaderBytes +
  kSceneMatrixCapacity*kSceneMatrixStride` == 生成常量 `kSceneDataLightArrayBase`（任一侧漂移编译失败）。
- **CMake 三处登记**（漏一处即 "module not found"）：`reflect` 管线 PRE_BUILD 生成 + `REFLECTION_PAIRS` 校验 +
  **`target_sources FILE_SET CXX_MODULES` 加 `${LIGHTS_CPPM}`**（新增生成模块必须登记，否则 importer 找不到 BMI——
  本次踩过）。
- **Slang 全局常量必须 `static const`**（`const uint X = ..` 报 E31224）。

**灯数据通路（CPU→GPU）**：`SceneFrameData` +`lightCount` +`std::array<GpuLight,kMaxLights>`；`render_list_impl`
从 `SceneLights` context（`lights[0]`=主光写 @0 头保 forward，全数组写尾区，clamp≤kMaxLights）；`vkrenderer_impl`
staging 写 `count@base` + `GpuLight[n]@base+16`（防御 lightCount>kMaxLights）。`light.frag` 改 `readLightCount()` +
逐灯 `readGpuLight(i)`：定向 `L=-direction` atten=1，点光 `L=(pos-P)/dist` atten=`clamp(1-dist/range)`（range=0→0），
共享 `eyePos@12`/`ambient@36` 仍走 `readSceneLight()`。

**配置（scene.lua `lights` 列表，可选回落 `[light]` 单灯）**：逐字段可选（direction/position/color/intensity/
range/type）；缺段 = 旧单灯兼容合成（`lights[0]=cfg.light`）→ **forward diff0 结构保证**（无 lights 段时行为不变）。

**3 灯截图验证**（Bistro，主定向 + 红点光 + 青点光，range 2.5）：
- deferred：红光洗近处建筑、青光染左上楼顶、**右侧远处建筑灰白**（出 2.5 衰减范围只剩定向+ambient）
  → 多灯累加 + per-light 颜色 + **距离衰减**三者正确，headless 零 VVL error。
- forward（同 3 灯配置）：**无红/青点光**（只渲 lights[0] 定向）→ forward 只读主光、diff0 语义确认。

**债清偿程度**：`GpuLight` 灯数组入反射（schema 生成布局常量，替 LightDataGpu 双源模式的**新增灯数据部分**）；
但 forward `LightDataGpu@0-48` 头仍手写双源（为 diff0 刻意不并），灯仍 `SceneLights` context 未 entity 化
（ECS 规范 3 留后续）。AGENTS.md 同步：反射管线 4→5、新增 `lights_types_generated` 模块行 + `lights.slang`
关键文件行、`LightDataGpu` 已知问题改"灯数组已入反射/前向头仍双源"。

**下一步**：批次 4（CPU frustum + AABB culling，`AabbBounds` 组件 + 逐 entity 剔除 + 可见统计）。

---

## 批次 4（2026-09-07）：CPU frustum + AABB culling

**包围盒数据源决策**：复用包围球（用户定）——但查现状发现 `Bounds`（包围球）**只挂在模型根 entity**
（`model_importer_impl:227`），且 `gltf_loader` **只算一个模型级** `BoundingSphere`（over 全部顶点）；
逐图元 entity（1591 个 draw 单位）**无任何包围数据**。逐图元剔除（有价值的粒度）需先造逐图元球。

**落地四步**：
1. `gltf_loader`：`BoundingSphere` 定义上移到 `GltfPrimitive` 之前 + 加 `GltfPrimitive::bounds`
   成员；顶点循环里**逐图元**算 node-local 球（over raw `vertices.position`，**不含 nodeWorld**——
   registerScene 存 `CullSphere`，render_list `modelMatrix = spin·root·nodeWorld` 每帧再乘 nodeWorld）。
2. `scene.cppm`：新增 `CullSphere` 组件（node-local 球，与 `Bounds` 分离——Bounds 是模型根世界聚合球供
   boundsSystem union 算 near/far，CullSphere 是逐 draw 图元局部球供剔除，互不污染聚合）+ `Frustum`
   （Gribb-Harman 6 面提取）+ `extractFrustum` + `sphereInFrustum`（inline，scene 域无 Vulkan 依赖）。
   **Gribb-Harman Vulkan 坑**：`GLM_FORCE_DEPTH_ZERO_TO_ONE` → clip z∈[0,1]，near 面 = **第 2 行**
   （z≥0），**不是** [-1,1] 约定的 `row3+row2`；far = `row3−row2`；其余 4 面同标准；6 面按法线长度归一化。
3. `registerScene` + `registerCubeEntity`：每图元/cube entity 挂 `CullSphere`（node-local）——
   **view 用必需组件**（非 optional，本仓 entt 3.16 无 `entt::optional` 且 `.each` lambda 只收组件不传
   entity handle → 拿不到 entity 做 try_get；cube 也补 CullSphere 保证 view 全覆盖，不丢实体）。
4. `render_list_impl`：帧首 `extractFrustum(proj·view)` 一次；entity 循环内 `modelMatrix` 算出后
   `worldCenter = modelMatrix·(localCenter,1)`、`worldRadius = localRadius·maxScale`（三列长度 max，
   非均匀缩放保守不剔）测 6 面；`culled++`/`visible = opaque+transparent.size()`，**仅计数变化时**
   `RLLogger::getInstance()->info` 打统计（相机移动才刷，静止不刷屏）；`RenderListState` 加
   `visibleCount/culledCount` 存跨帧比较基线。

**验证**（Bistro 1591 图元）：
- 默认机位：`1571 visible / 20 culled`（背后/侧面出视锥），零 VVL error（forward + deferred）。
- 背向场景（target 打到远处）：`0 visible / 1591 culled`（全剔，覆盖正确）。
- **正确性金标准**：开剔除（1571 visible）默认机位截图与批次 3 全量渲染**像素一致**（建筑无缺失）
  → 保守球测只剔出视锥者、绝不误剔可见几何。

**净结果**：CPU 侧逐图元 frustum culling 跑通（球近似，够视锥剔除；真 AABB 更紧留后续）。
数据流 ECS 合规：包围球是 entity 组件（`CullSphere`）、剔除在 render_list system、产出仍是抽象
draw list（规范 4），无全局状态。**下一步**：批次 5 = GPU culling / DGC（归 Phase 7b）；批次 6 文档销账。

## 勘误（2026-09-08，重构清理批次 1）

- 上文 VVL 修复①"worldPos RT `r32g32b32_sfloat` 不支持 SAMPLED_IMAGE（3 通道 float 格式采样位缺失）"
  **归因有误**：Vulkan spec 中 R32G32B32_SFLOAT 是 sampled-capable 格式（Format Properties 表含
  SAMPLED_IMAGE_BIT）。实测 `VK_ERROR_FORMAT_NOT_SUPPORTED` 来自 **RADV 驱动实现**（未给 RGB32 float
  格式配采样路径），非 spec/格式本身限制。换 RGBA32_SFLOAT 的处置不变（驱动现实约束），但后续维护者
  勿以"RGB32 不可采样"为前提做决策（换驱动/换平台该前提不成立）。`deferred_geometry.frag.slang` 头注释已同步改。

---

## 重构批次 3（2026-09-08）：环境变量 → 配置文件（用户偏好：不用 env）

全仓 18 处 getenv 调用点（16 个不同 env 名，TEXTURE_FORCE_UNORM 两处）清点处置（驱动级 RADV_EXPERIMENTAL / VK_DRIVER_FILES / prime 保留——
由 Justfile run-* 注入，非应用配置）：

- **configs/runtime.lua**（新，`runtime_config` 模块）：headless / headless_size / win_size /
  show_after + **llm_debug/llm_shot 自 camera.lua 迁入**（截图属运行模式，camera.lua 回归纯相机；
  LlmDebugConfig 类型随迁 runtime_config，ecs/llm_debug 改 import）。
- **configs/diagnostics.lua**（新，`diagnostics` 模块）：no_vvl / no_hdr / no_present /
  vtx_canary / texel_canary / dump_gbuf / style_dist / tex_dist / texture_force_*（共 11 门，
  默认全关，启动打一行汇总）。**承载方式 = 单例**（仿 RLLogger::getInstance）——这些门散在
  低层深处（vkinstance/sdl3window/vkrenderer/texture 上传/ecs 帧循环/model_importer/llm_debug），
  逐个加参数要穿 uploadTexture 等一串签名；单例让 diagnostics 成为叶子配置模块（零反向依赖，
  main 在任何 GPU 对象构造前 setDiagnostics）。数值门负数 = 显式"关"哨兵（-1 直观）。
- **configs/render_graph.lua 顶层 `variant`**（"forward"|"deferred"|"offscreen"）：
  loadRenderGraphConfig 返回 `RenderGraphConfig{variant, desc}`，deferred/offscreen 直返内建图
  （exclusive）；main 的 deferredMode 由 variant 求值——顺账清偿 roadmap"批次 2 剩余②选路转正"。
- 注释同步：活跃触发名注释全部改指配置路径；历史溯源注记（"原 env"）保留；
  顺手修 vkinstance 注释 RGL_EXT_headless_surface 笔误（→ VK_EXT）。

**验证**：getenv 全仓清零；variant=deferred 编译 6att/2pass + G-buffer 槽 ✓；headless+llm
截图与基线 **BIT-IDENTICAL** ✓；style_dist/texel_canary 非默认值实锤生效（frame 5 金丝雀
PASS + 1591 prim 分布行）✓；forward/deferred 双路零 VVL error。

---

## 重构批次 4（2026-09-08）：legacy 删除 + vulkan_practice → rgl 转正

- **legacy/ 删除**（146 文件，全文本、不参与构建、无 LFS）：项目已全面超越旧 OpenGL 工程，
  思路参考回查 git history。连带清 vcpkg.json imgui（legacy 遗留依赖，主工程零使用）+
  .gitignore 两行失效条目。代码内 4 处 legacy 溯源注释保留（设计出处记录）。
- **vulkan_practice → rgl**（目录 + CMake target + 可执行名全转正）：产物 `build/rgl/rgl`，
  CWD 契约 `build/rgl/`。改动面 = 根/子 CMakeLists、Justfile 6 处、.zed/debug.json、
  ReflectionManifest 文案、tools/generator 注释、configs 注释、AGENTS + 4 篇现行 docs +
  min_mesh_crash README；`just reflect` 重写 manifest 路径；窗口标题 "Vulkan Practice"→"RGL"。
  依据 = 结构调研（方案 1 最小动作；"提升到根 + 分组子目录"方案否决——CWD/binary dir 连锁
  成本大于收益；render_graph 家族目录化亦否决——modules 下目录不改变编译图）。

**验证**：just config/reflect/build 全绿；headless+llm forward/deferred 截图与改名前基线
**BIT-IDENTICAL**、零 VVL error；`vulkan_practice` 字样在代码/配置/活跃文档清零（有意保留 2 处：调试历史结构性变更备查注记、.opencode/plans 旧计划）。

---

## 重构批次 5（2026-09-08）：captureFrame 退役——llm 截图改帧内回读（不图化，去平行渲染器）

**动机**（用户定调"不图化但不能成负担"）：负担本质不是"没走图"而是 **captureFrame 是第二个迷你渲染器**
（自造离屏 RT + 手写 2-pass/barrier，图每演进一次截图路径就要人工跟随一次，且分歧不报错——用
错误画面做验收比崩溃更危险）。解法 = 不图化、删重渲：截图 = drawFrame 本帧真实输出的回读。

**实现**：
- `vkrenderer`：`requestFrameCapture(gbufChannel)`（drawFrame 前）→ ⑪.5 在 presentTransition 前录
  barrier(追踪布局→TransferSrc) + `vkCmdCopyImageToBuffer`（后缓冲或 GB RT 直读=dump_gbuf）→
  后缓冲追踪表推进为 TransferSrc（presentTransition 天然转 PresentSrc，**零图改动**）；submit 后
  同步等本帧 inFlight fence 搬进结果（仅请求帧停顿）→ `takeFrameCapture()`。swapchain image 补
  `TRANSFER_SRC` usage（VVL 实锤）。`llm_debug` 缩成 `writeCapturePng`（take→格式转 RGBA8→stb），
  ~330 行重渲器删除；8 个 captureFrame 专用访问器退役。
- `ecs`：llmCapture 回调签名 `(Renderer&)`；wantCapture 帧请求、`hasFrameCapture()` 防 acquire
  超时跳帧误消费（请求保持 pending 重试）。
- headless+llm：main 契约令 headless_size 跟随 llm_shot.size（截图尺寸 = swapchain extent）。

**顺带揪出两个真实输出 bug（旧重渲路径一直在撒谎）**：
1. **deferred 背景灰**：图里 GB 附件吃 `RgClearValue` 默认 0.1 灰 → 空像素 albedo=0.1 → light 输出
   染灰；旧 captureFrame 自己把 GB 清 0 所以截图一直是"假干净"。修 = deferredRenderGraphDesc
   GB outputs 显式清黑。
2. **旧截图恒暗于真实上屏**：离屏 RT 用 R8G8B8A8_UNORM（线性值直存），丢了硬件 sRGB 编码——
   新回读拿真实 SRGB 后缓冲，同场景截图明显变亮（= 屏幕本来就这么亮）。**旧 BIT-IDENTICAL 基线
   全部作废，新基线 = 本批次回读截图**（fwd/def/offscreen 三 variant 已重验确定性 + offscreen
   blit 与 forward 直渲位级一致）。

**验证**：三 variant 零 VVL error、帧间确定；forward/offscreen 截图位级一致（blit 语义实锤）、
deferred 黑背景 + 光照与 forward 一致；dump_gbuf=0 直读通道工作；captureFrame 字样全仓清零。

---

## 2026-09-09 — 批次 5（= Phase 7b）GPU culling / DGC：culling compute + 双路提交

**动机**（用户定调）：GPU culling = **默认路径**（`VK_EXT_device_generated_commands` 扩展 required，
不留"开/关 culling"配置开关）；批次 4 的 CPU frustum 剔除降为**兜底 + 对账基准**（每帧仍跑，
结果与 GPU 对账，不一致告警）；RADV（本机 Strix Halo 核显）graphics 侧 DGC 不可用
（`supportedIndirectCommandsShaderStages{Pipeline,Shader}Binding` 仅 COMPUTE，26.2.1 复测
graphicsIndirect=false/computeIndirect=true）→ RADV 回退 = **读回重放**（非 IndirectCount，见下）。

### 设计修正（推翻 research 文档 §4.3 的 IndirectCount 回退方案）

**`vkCmdDrawMeshTasksIndirectCountEXT` 回退不可行**：该命令没有 per-draw push data 机制——
mesh shader 拿不到 per-draw 的 DrawArgs（mvp/objectIndex/totalTriangles），IndirectCount 只给
count 不给数据，`DrawMeshTasksIndirect` 序列里每个 draw 共享当前 push region。项目每 draw 必须
推自己的 DrawArgs（push constant 契约）→ 回退路改为**读回重放**：culling compute 输出序列后
CPU 读回（64B 结果 buffer + 序列 buffer 的 count 字段），CPU 侧逐可见条目重放
`vkCmdPushDataEXT` + `vkCmdDrawMeshTasksEXT`。与 forward 现有提交路径**逐位一致**
（同一 registry 条目、同一 pushData、同一 drawMeshTasks），BIT-IDENTICAL 天然成立。

**mvp 由 CPU 算进序列，GPU 只写可见性**：GPU 端 mvp = viewProj × modelMatrix 的 float4x4 乘法，
FMA 收缩序与 CPU（glm）无位级契约 → 若 GPU 算 mvp，DGC/重放两路拿到的 mvp 可能与 forward
（CPU 算）差 1 ULP → 违反 forward diff0 硬约束。定稿：**CPU 把每 candidate 的完整 DrawArgs
（含 mvp）预写进序列**，GPU 只覆盖 `meshCmd.x`（groupCount|0 或 0）+ 累加 count。
DGC 路与重放路消费**同一份序列**（CPU args + GPU 可见性），两路输出位级一致。

### 实现（5 个子批，均已完成并验证）

- **5a 扩展+能力位**（行为中性）：`features.cppm` 两处加回 `VK_EXT_DEVICE_GENERATED_COMMANDS`
  + `features_config.json` `deviceGeneratedCommands: true` + `just reflect` 重生成；
  `vkdevice.cppm` `queryDgcSupport()` → `DgcSupport{graphicsIndirect, computeIndirect,
  maxIndirectShaderObjectCount}`（`supportedIndirectCommandsShaderStagesShaderBinding` 与
  MESH|FRAGMENT 的交集判 graphics），启动日志打印。
- **5b culling compute + 对账**：`gpu_cull.cppm`（CullEntry 60B 表 + 序列 buffer + count buffer
  + seqStaging ring×2）+ `shaders/cull.comp.slang`（6 面 Gribb-Harman，push 100B = planes[6] +
  sequenceCount，`numthreads(64,1,1)`，ByteAddressBuffer 读 CullEntry 表/对象矩阵表，
  `InterlockedAdd` count）。`vkrenderer` 构造 + `allocateCullCommandBuffers` +
  `reconcileGpuCull`（多重集对账：GPU 可见集合 vs CPU 可见集合，size + 元素集合，不一致告警
  不中断）+ `drawFrame` ④c 段（cull CB submit → waitForFences → resetFences → readback →
  reconcile）。main C3 段构造 candidate 表（`view<MeshRef, MaterialRef, CullSphere>`，
  守卫 = pipelineIndex 有效 && groupCount>0 && !alphaModeBlend）。
- **5c-1 读回重放**（RADV 实际路径）：`FrameDrawList` + `cullArgs`（逐 candidate 的 DrawArgs，
  CPU 预算含 mvp）；`render_list` 重构：candidate 判定**前置**到 cull 测试之前、args 在剔除前
  算好（被剔条目 args 也进 cullArgs，GPU 只置 meshCmd.x=0 不画）；`GpuCull::writeSequenceStaging`
  每帧把 CPU args memcpy 进 seqStaging（MAPPED）→ cull pass 内 staging→sequence 拷贝；
  Renderer ④c 后读回序列可见性，pass 循环 opaque 段 = **重放**（逐可见 candidate：
  registry 条目 + cullArgs[i] push + drawMeshTasks）。
- **5c-2 DGC 模块**（NVIDIA 路径，本机能力门控不激活）：`vk_indirect_commands.cppm`
  （`DgcCommands` RAII：layout 3 token = EXECUTION_SET@0（u32×2：frag@2j, mesh@2j+1，
  per stage-bit LSB→MSB）+ PUSH_DATA@8（100B DrawArgs，`VkPushConstantRange` MESH|FRAG）+
  DRAW_MESH_TASKS@108（action，最后）；indirectStride=120B；execution set SHADER_OBJECTS
  capacity=2×条目数（2j=frag/2j+1=mesh，unique-index VU）；preprocess buffer 经
  `vkGetGeneratedCommandsMemoryRequirementsEXT` + 原生 `vkAllocateMemory`（绕 VMA，
  BDA 兼容）；**8 个 DGC 入口点全走 `vkGetDeviceProcAddr`**——系统 loader 1.4.357 不导出任何
  DGC 全局符号，`vk::` 无 wrapper（vulkan_raii.hpp 有 raii 类但本项目不用，PFN 模式同
  texture_upload 先例））。Renderer 双路：`dgc->active()` → DGC execute（render pass 内
  `vkCmdExecuteGeneratedCommandsEXT`，sequenceCountAddress = count buffer BDA+4）；否则重放。
  main 条件构造 `DgcCommands`（graphicsIndirect && cull active），catch 回落重放。
- **5d 文档**（本条目 + research 勘误 + roadmap + AGENTS）。

### 布局契约（当前值，static_assert 锁定）

| buffer | 布局 | 说明 |
|---|---|---|
| CullEntry 表 | 60B×N | center@0 radius@12 objectSlot@16 pipelineIndex@20 groupCount@24 totalTriangles@28 vertexOffset@32 baseColor@36 rm@52 |
| 序列 | **120B**×N | execSlotFrag@0（2×pipelineIndex）/ execSlotMesh@4（2×pipelineIndex+1）/ DrawArgs@8（100B）/ meshCmd@108（x=groupCount\|0 或 0, y=z=1） |
| CullPush | 100B | planes[6]@0 + sequenceCount@96（viewProj 已移除——mvp 不经 push，CPU 预算进 args） |
| count buffer | 64B | visible@0（InterlockedAdd 累计）+ total@4（thread0 写 push.sequenceCount）；DGC sequenceCountAddress = BDA+4 |

### 踩坑

1. **矩阵表陈值 bug**（5b 背向机位 MISMATCH 根因）：`state.matrices`（对象矩阵表）原来只对
   **CPU 可见**对象每帧写 → GPU cull 读到被剔对象的**陈值/恒等矩阵** → 误判可见。修复 =
   `render_list` 在 cull 测试**前**对**全部** candidate 写矩阵表（剔除只影响 draw list 不影响
   矩阵上传）。教训：GPU cull 的输入矩阵表必须**全量刷新**，不能与 CPU 可见性耦合。
2. **Slang `[[vk::push_constant]]` 挂变量不挂 struct**（与 `vk::buffer` 同理）；
   `ByteAddressBuffer` 只有 `Load/Load2/Load3/Load4` + `Store/Store2/3/4`（无 LoadFloat/
   LoadUint，Store2/4 要 uint2/uint4 先 asuint）。
3. **Slang float4x4 构造/operator[] 语义歧义** → 矩阵乘法用 float4 数组 + 列组合手工展开
   （`C列j = a0*Bj.x + a1*Bj.y + a2*Bj.z + a3`），不依赖 Slang 矩阵语义。
4. **v357 API 面比 research 文档（1.4.341）窄**：无 `vkCmdBindIndirectExecutionSetEXT` /
   `vkCmdSetGeneratedCommandsSequenceAddressEXT` / `vkCmdSetGeneratedCommandsSequenceCountBufferEXT`
   ——全经 `VkGeneratedCommandsInfoEXT` 传（indirectAddress/Size、preprocessAddress/Size、
   sequenceCountAddress、maxSequenceCount/maxDrawCount）；`VkWriteIndirectExecutionSetShaderEXT`
   无 stage 字段（扁平 2j/2j+1 排布）；`sequenceCountAddress=0` → 用 maxSequenceCount。
5. **loader 无 DGC 符号** → 8 入口点 PFN via `vkGetDeviceProcAddr`；BDA 用 core
   `vkGetBufferDeviceAddress`（非 EXT 别名）。
6. **raii::PhysicalDevice 无 `.get()`** → `getMemoryProperties()` 直取；`VK_UINT32_MAX` 在
   v357 不存在 → `std::numeric_limits<uint32_t>::max()`；`eCommandPreprocess` 枚举名带
   `EXT` 后缀（`eCommandPreprocessEXT` / `eCommandPreprocessReadEXT`）。

### 验证

- **5b**：默认机位 GPU=CPU=1571 可见（1591 candidate，20 被剔）；背向机位 0/0（全剔）；
  矩阵表修复前背向 MISMATCH（GPU 38 vs CPU 0）→ 修复后 match。
- **5c BIT-IDENTICAL**（同会话双构建，`git stash` 基线 vs 改动，stride 116→120 变更后**复验**）：
  forward `shot2_base_fwd.png` vs `shot2_gpu_fwd.png` **cmp 相同**；deferred
  `shot2_base_def.png` vs `shot2_gpu_def.png` **cmp 相同**。零 VVL error。
- **DGC 路径**（NVIDIA）：本机 RADV graphicsIndirect=false → 能力门控不激活，代码经 VVL 审查
  + 编译绿，**未运行时实测**（需 NVIDIA 机）。
