# Phase 3 实施计划：纹理管理

> **状态**：现行（2026-08-27 规划；**M1/M2 已完成并验证**；**M3 受阻**：Slang 分离绑定在 RADV descriptor heap 采样恒 garbage——RADV 只映射组合 OpTypeSampledImage（R1 已证实，见 heap_texture_research §八），代码已就绪待 **NVIDIA 补测**定案：NVIDIA 支持分离 → M3 直接过；同样只认组合 → model.frag 改 GLSL 组合 sampler2D（spike 已验证路径）或等 RADV 修复）
>
> 配套 [next_roadmap.md](next_roadmap.md) 的 Phase 3 小节。可执行粒度实施计划，
> 含已确认决策、事实核查、分步执行、文件变更清单与风险。

---

## 0. 目标

glTF 模型 `baseColorTexture` 进现有 descriptor heap 管线并采样（`baseColorFactor × texture`，sRGB 线性化），
同时清偿路线图已知债：**统一 slot 分配器 + heap 统一步长**（"归 Phase 3 TexturePool 并案（资产表为宿主）"）。

**验收**：带纹理模型在 Strix Halo（RADV）上纹理正确渲染——`llm_debug` 截图目检 + 无 warn/error。

**不做（延后）**：mipmap、texture atlas、外部图片配置字段、透明排序（Phase 5）、PBR 光照（Phase 4/5）、
sampler heap、transfer queue 启用、GPU 侧纹理上传。

## 1. 事实核查（2026-08-27）

### 1.1 测试模型：`assets/realistic_wooden_cottage_garden_house_3d_model.glb`（257MB，用户 2026-08-27 下载）

| 项 | 值 | 影响 |
|---|---|---|
| primitive 数 | **392**（392 mesh） | 392 个 BDA buffer + 392 描述符 + 392 管线表条目（per-primitive 管线为 Phase 2 既定设计）；启动期一次性成本，需实测 |
| 顶点（去索引后） | 2,578,056 索引项 → **~793K 顶点 / 859K 三角形 / 82.5MB**（32B packed） | 启动上传 ~80MB mapped 写，秒级 |
| 内嵌纹理 | **33 张全部 4096×4096**（22 jpeg + 11 png），解码后 RGBA8 合计 **2.2GB** | Phase 3 只采样 11 张 `baseColorTexture`（≈740MB GPU）；其余 22 张（mask）只解析不上传 |
| 材质 | 12 个，11 带 `baseColorTexture`，1 无（`Wooden_CottagestandardSurface2`） | 无纹理材质 → 白 1×1 纹理特例（D4） |
| 顶点属性 | 全部含 `POSITION`+`NORMAL`+`TANGENT`；**1 个 primitive 缺 `TEXCOORD_0`**（仅 NORMAL+POSITION） | **新决策 D9：TEXCOORD_0 改可选，缺失 → 白纹理**（现状 loader 严格校验会整体失败） |
| 多余属性 | `TANGENT`/`TEXCOORD_1`/`TEXCOORD_2` | 现 loader `accessorOf` 只取三属性，天然忽略，零改动 |
| 节点 | 787 个，**无 translation**（几何自带世界坐标） | translation 扁平化不受影响；相机位用首跑日志的包围球定 |
| 三角形分组 | 859K / `kTrianglesPerGroup(10)` ≈ 85,936 group | per-primitive `drawMeshTasksEXT`，无压力 |

### 1.2 代码现状（调研确认，2026-08-27）

- `VmaDescriptorHeap`（VmaBuffer.cppm:114-186）：`descriptorSize` 只取 `bufferDescriptorSize`（vkresources.cppm:173），
  slot 寻址 `slot * descriptorSize` 统一；`getSlotCpuAddress` 返回 `std::expected<void*, Error>`。**步长是注入参数，VmaBuffer.cppm 零改动**。
- slot 硬编码：cube 顶点/索引 = `reserved + 0/4`（types_schema.lua），模型 = `reserved + 8 + i`（model_importer.cppm:62）。
- 反射工具：`schema_loader` shape 自由字符串；`validator` **强制** `heapSlotOffset >= 0` + `cppName` 非空（validator.cppm:119-126）；
  `generator` 对匹配名**无条件**发 slot 常量（generator.cppm:190-201）→ 纹理槽（运行时分配、无常量）需放宽两者。
- `shader.cppm` RenderPipeline：2 条目 binding mapping（binding0 顶点 / binding1 索引），`heapOffset = slot * descriptorSize`，`resourceMask = ALL_EXT`。
- `vkrenderer.cppm` `addModelPipeline(vertexSlot, assetId)`：per-primitive 条目，mapping 烘焙顶点 slot，pipelineIndex 写回资产表。
- `drawFrame` heap barrier（host→resourceHeapRead）**每帧无条件**（vkrenderer.cppm:403-410）→ 启动期一次性描述符写入安全。
- `stb_image.h` 已在 vcpkg 安装目录（build/vcpkg_installed/.../include/）；`stb_image_resize2.h` 亦在（可选降采样用）；
  `tinygltf_impl.cpp:4` 的 `TINYGLTF_NO_STB_IMAGE` 注释"port 未安装"已过时。
- `model.slang` 已声明 `uv : TEXCOORD0`（顶点流 offset 24 已上传），`model.frag.slang` flat unlit 未采样。
- spike 已验证（RADV）：单 heap 混存 buffer+image、`pEmbeddedSampler`（OpTypeSampledImage 路径）、v341 4 参写描述符、
  `VkImageDescriptorInfoEXT{pView=create info, layout}`。见 `docs/heap_texture_research.md`。

## 2. 已定决策

| # | 决策 | 依据 |
|---|---|---|
| D1 | 纹理源 = **glTF 内嵌纹理**（tinygltf + stb 解码）；不做 scene.lua 外部图片字段 | 真实工作流、单文件、sRGB/factor 语义归 glTF spec |
| D2 | 测试模型 = cottage（§1.1），保留 transparent.glb 作回归基线（M1 验证用） | 用户指定；规模压力充分 |
| D3 | **统一 slot 分配器 + heap 步长 `max(buffer,image)` 纳入本期** | 路线图已知债并案；RADV image=32B > buffer=16B 不改必撞墙 |
| D4 | 单一 shader 路径：frag 恒采样，无纹理/无 uv 的 primitive 用**白 1×1 纹理**特例（pool 固定首槽） | 单 SPV + 单管线 spec；cottage 12 材质中 1 个无纹理 + 1 primitive 无 uv |
| D5 | ~~sampler 一律 `pEmbeddedSampler`~~ → **改独立 sampler heap**（`vkWriteSamplerDescriptorsEXT` + `vkCmdBindSamplerHeapEXT` + 独立 `eSamplerHeapReadEXT` barrier）；Slang 走 `Texture2D<float>` + `SamplerState`（OpTypeImage+OpTypeSampler 对） | **pEmbeddedSampler 对 Slang 分离对在 RADV 实测采样恒 0**（R1 证实），回退链（独立 sampler heap）已执行；heap_texture_research §八 |
| D6 | 格式 `R8G8B8A8_SRGB`（glTF baseColor 纹理 spec），启动期 formatProperties 校验、不支持回落 UNORM + warn | sRGB 线性化硬件完成 |
| D7 | 上传 = 启动期一次性：graphics 队列独立命令缓冲（linear staging → optimal 拷贝 + UNDEFINED→SHADER_READ_ONLY）+ waitIdle；**不启用**闲置 transfer queue | 与 `recreateDepthImage` waitIdle 模式一致，最小改动 |
| D8 | 无新增 feature/扩展 | heap_texture_research §六.5 |
| D9 | **`TEXCOORD_0` 改可选**：缺失 → 该 primitive 用白纹理（uv 字段填 0，不报错） | cottage 有 1 个 primitive 缺 uv；glTF spec 中 TEXCOORD_0 本就可选。POSITION/NORMAL 维持严格 |
| D10 | 不降采样（4K 全尺寸，11 张 ≈ 740MB GPU；Strix Halo 统一内存容量无压力）；**记录启动耗时**，若 >60s 再议 `maxTextureSize` 旋钮（`stb_image_resize2.h` 已可用） | 学习项目保留原始质量；旋钮是 follow-up 不是主线 |

## 3. 关键设计

### 3.1 heap 统一步长（M1）

`Resources::createDescriptorHeap`（vkresources.cppm:169）：

```cpp
uint32_t descriptorSize = max(bufferDescriptorSize, imageDescriptorSize);  // 查询两个属性 + 日志
```

- 连带：`reservedSlots = ceil(minResourceHeapReservedRange / descriptorSize)`（NVIDIA 6048→3024 槽，保留字节数不变；RADV 0 不变）
- capacity 公式 `reserved + HeapSlotModelVertexBase + kUserSlotMargin(1024)` 不变；纹理槽消费裕量（cottage：2 cube + 392 model + 12 tex ≈ 406 << 1026 用户槽）
- `getSlotCpuAddress`/`getHeapSize`/`writeDescriptors`/`shader.cppm` `heapOffset` 全走 `descriptorSize`，自动兼容

### 3.2 统一 slot 分配器（M1）

`Resources::allocSlot(const char* name)`：从 `reservedSlots` 起单调递增，name→slot 打 debug 日志（slot 表可排查）。

布局从"稀疏约定"改**连续**（schema 偏移常量同步改，反射重新生成）：

```
reserved+0    cube 顶点        （原 +0）
reserved+1    cube 索引        （原 +4）
reserved+2..  模型顶点 × N     （原 +8+i；N = primitive 数）
reserved+2+N  白 1×1 纹理      （TexturePool 固定首槽）
reserved+3+N.. 其余纹理（pool 上传序）
```

- schema 改动：`types_schema.lua` indexBuffer `heapSlotOffset` 4→1；`model_schema.lua` 8→2（含注释）→ `just build` 自动重新生成两个 cppm
- **启动期 assert**：分配器实际布局 == schema 约定（cube 两槽 + 模型首槽），违例 = 契约漂移，直接报错
- `MeshAsset::heapSlot` 语义不变（绝对 slot，资产表记录）

### 3.3 TexturePool（M2，新模块 `texture_pool.cppm`）

```cpp
struct TextureAsset {
    VmaImage image;
    VkImageViewCreateInfo viewCreate;   // 必须保留：v341 VkImageDescriptorInfoEXT::pView 要 create info 非 handle
    vk::raii::ImageView view;
    uint32_t heapSlot;                  // allocSlot 分配（白纹理 = 固定首槽）
    uint32_t width, height;
};

class TexturePool {
    // 解码（stb，glb 内嵌 byte span / 外部文件；不 flip——glTF 纹理坐标约定）
    // → 创建 optimal 图（SAMPLED|TRANSFER_DST，sRGB）+ linear staging
    // → 一次性拷贝 + UNDEFINED→SHADER_READ_ONLY（graphics 队列独立命令缓冲，waitIdle）
    // → 写描述符（SAMPLED_IMAGE，4 参 v341，spike 已验证形态）→ allocSlot → 注册
    uint32_t loadTexture(const std::string& path);            // 外部文件
    uint32_t loadEmbeddedTexture(const uint8_t* data, size_t size);  // glb 内嵌（buffer view byte span）
    uint32_t getWhiteTexture();                               // 1×1 白，幂等
    const TextureAsset& get(uint32_t id) const;
    VkSamplerCreateInfo embeddedSampler;                      // 共享：linear/min + mipmap NEAREST + CLAMP_TO_EDGE，maxLod 0
    // path/span → id 缓存（同图不重复上传）
};
```

- 生命周期：`Resources` 成员（与 AssetTable/heap 同层），buffer/image 随 `SharedAllocator` 控制块存活
- sRGB 校验：`getFormatProperties` 查 `VK_FORMAT_FEATURE_SAMPLED_IMAGE`，不支持 → UNORM + warn（R4）
- 只加载材质引用的 `baseColorTexture` 图（11/33），mask 图解析不出纹理数据（D1 范围）

### 3.4 glTF 侧（M2）

- `tinygltf_impl.cpp`：删 `TINYGLTF_NO_STB_IMAGE`（启用 stb 解码路径）；`stb_impl.cpp` 加 `#define STB_IMAGE_IMPLEMENTATION`（实现 TU 唯一）
- `GltfPrimitive` 增 `int baseColorTextureIndex = -1`（image 下标；-1 = 无纹理 → 白纹理）
- `GltfModel` 增 `std::vector<GltfImage> images`（`std::span` 指向 buffer 数据的 byte span + mimeType + width/height（解码后回填））；glb 单 buffer 场景 span 生命周期 = GltfModel 存活期
- `TEXCOORD_0` 可选（D9）：缺失时 uv 填 0 + 该 primitive 强制白纹理 + 日志

### 3.5 shader + binding mapping（M3）

- `model.slang`（**Slang 2026.7 实测语法**：小写 `texture<float,2>` 报 "undefined identifier"+segfault，须大写 `Texture2D<float>`；`Load4` 非 `LoadFloat4`）：
  ```slang
  [[vk::binding(1, 0)]] Texture2D<float> baseColorTexture;
  [[vk::binding(2, 0)]] SamplerState baseColorSampler;
  ```
- `model.frag.slang`：`return float4(modelDrawArgs.baseColor.rgb * baseColorTexture.Sample(baseColorSampler, uv).rgb, 1.0);`（`.Sample` 大写 S；alpha 忽略，Phase 5）
- `shader.cppm` `RenderPipeline`：构造增可选 texture/sampler 参数 → mapping 2 条目（cube 不变）/ 3 条目（模型）：
  binding0 顶点 slot（现状）、binding1 纹理 slot（SAMPLED_IMAGE 描述符，resource heap）、binding2 sampler slot（**`samplerHeapOffset`，独立 sampler heap**——`pEmbeddedSampler` 对 Slang 分离对 RADV 实测无效）；`resourceMask` 维持 `ALL_EXT`
- `vkrenderer.cppm` `addModelPipeline(vertexSlot, textureSlot, samplerSlot, assetId)` 透传；`drawFrame` 增 `vkCmdBindSamplerHeapEXT` + barrier 增 `eSamplerHeapReadEXT`；pipelineIndex 写回不变
- **push constants / DrawCall / renderListSystem 零改动**（纹理槽烘焙进 per-primitive 管线 mapping，每帧零成本）

### 3.6 反射工具扩展（M3）

- **M3.0 先验证**：slangc `-reflection-json` 对 `texture<float,2>` / `SamplerState` binding 的 `binding.kind`（预期 `descriptorTableSlot`）与 `type.baseShape` 字符串 → 定 schema 写法
- `model_schema.lua` descriptorSlots +2 条目（name/index/shape；**无 cppName/heapSlotOffset**——运行时分配器分配，不生成常量）
- `validator.cppm`：`heapSlotOffset`/`cppName` 改可选（有 cppName 才要求 offset ≥ 0；纹理槽跳过）
- `generator.cppm`：`cppName` 为空 → 不发 slot 常量
- 模板 `types_cppm.inja` 预期零改动（slots 数组少两条）

## 4. 执行步骤

| 里程碑 | 内容 | 验证 |
|---|---|---|
| **M1** 步长+分配器（纯重构，行为零变化） | ① `max` 步长 + 属性日志 ② `allocSlot` + cube（0/1）/模型（2+i）迁移 ③ schema 偏移改（4→1，8→2）+ 重新生成 ④ 布局 assert ⑤ 注释/文档同步 | `just build` + llm_debug 截图：transparent.glb 渲染与改前一致；日志核对 slot 表 |
| **M2** TexturePool + 解码上传 | ① tinygltf/stb 启用 ② `GltfModel.images` + `baseColorTextureIndex` + TEXCOORD_0 可选（D9）③ `texture_pool.cppm`（资产+pool+白纹理+一次性上传+sRGB 校验）④ `Resources` 挂 pool ⑤ CMake FILE_SET | 换 cottage：启动日志（392 槽位表/11 纹理尺寸格式/包围球/耗时），无 warn/error；llm_debug 截图（flat 无纹理基线——shader 未改，应与 M1 一致） |
| **M3** 采样 + mapping | ① M3.0 反射 shape 验证 + 组合验证（RADV）② shader 两文件 + schema + tools 放宽 ③ `shader.cppm` 3 条目 + `vkrenderer` 透传 + `main.cpp` 初始化序（模型解析后、registerScene 前加载纹理；MaterialRef 记 textureId）④ scene.lua 换 cottage ⑤ CMake 模型 copy + 相机位（首跑包围球定） | llm_debug 截图纹理目检（木屋贴图可见）+ 像素无紫边/全黑 + VVL 无新错 |
| **M4** 收尾 | ① AGENTS.md（模块表 + texture_pool、已知债划掉 slot 分配器、运行要求不变）② next_roadmap Phase 3 完成情况 ③ heap_texture_research 回填（Slang 组合验证结果；NVIDIA 待补测如实标注）④ 技术概念.md 补纹理节 | — |

## 5. 文件变更清单

| 文件 | 变更 | 里程碑 |
|---|---|---|
| `vkresources.cppm` | `max` 步长 + `allocSlot` + 布局 assert + pool 成员 + 注释 | M1/M2 |
| `model_importer.cppm` | 顶点槽 → `allocSlot`（删 8+i 硬编码）+ 布局 assert | M1 |
| `gltf_loader.cppm` | `images`/`baseColorTextureIndex` 解析 + TEXCOORD_0 可选（D9） | M2 |
| `tinygltf_impl.cpp` / `stb_impl.cpp` | 启用 stb 解码（删 NO_STB_IMAGE / 加 IMPLEMENTATION）+ 过时注释 | M2 |
| `texture_pool.cppm`（新） | TextureAsset + TexturePool + 白纹理 + 一次性上传 + sRGB 校验 | M2 |
| `scene.cppm` | `MaterialRef` + `textureId`（kInvalidTextureId → 白纹理） | M3 |
| `shader.cppm` | mapping 2→3 条目（可选纹理参数 + 内嵌 sampler） | M3 |
| `vkrenderer.cppm` | `addModelPipeline` 透传 textureSlot | M3 |
| `main.cpp` / `CMakeLists.txt` | 纹理加载初始化序 + FILE_SET + cottage 模型 copy | M2/M3 |
| `shaders/model.slang` / `model.frag.slang` | 纹理+采样器声明 / 采样 | M3 |
| `shaders/model_schema.lua` / `types_schema.lua` | 纹理槽条目（无 cppName）/ 偏移 8→2、4→1 | M1/M3 |
| `tools/validator.cppm` / `generator.cppm` | offset/cppName 可选化 | M3 |
| `configs/scene.lua` | model → cottage 路径 | M3 |
| `AGENTS.md` / `docs/next_roadmap.md` / `docs/heap_texture_research.md` / `docs/技术概念.md` | 回写 | M4 |

## 6. 风险与对策

| 风险 | 等级 | 对策 |
|---|---|---|
| **R1** Slang OpTypeImage+OpTypeSampler 对 + `pEmbeddedSampler` 组合未实测（spike 只验证 OpTypeSampledImage） | 高 | **已证实（2026-08-27，RADV）**：分离对无论走 `pEmbeddedSampler` 还是独立 sampler heap 均采样恒 garbage——RADV descriptor heap 只映射组合 OpTypeSampledImage（spec 层面分离绑定合法，是 RADV 实现缺口）。代码已按 spec 正确实现（sampler heap + 独立 read barrier），**NVIDIA 补测定案**：支持分离 → M3 直接过；只认组合 → model.frag 改 GLSL 组合 `sampler2D`（spike 已验证）或等 RADV 修复。详见 heap_texture_research §八 |
| **R2** NVIDIA `imageDescriptorSize` 未知（buffer=16 已知） | 中 | 运行时查询零硬编码；步长取 max 对 NVIDIA 上界安全（VUID 11207）；NVIDIA 验证待 dGPU 环境，文档如实标注 |
| **R3** VVL descriptor-heap image layout 跟踪伪报（09600，spike 已定性） | 低 | 验收以截图/像素比对为准，VVL 结果单独记录不阻断 |
| **R4** 某驱动不支持 sRGB 采样格式 | 低 | 启动期 formatProperties 校验 + UNORM 回落 + warn |
| **R5** cottage 392 管线条目 + 11 张 4K 纹理 + 80MB 顶点，启动耗时可能 10~60s | 中 | 实测记录（D10 阈值 60s）；超阈值加 `maxTextureSize` 降采样旋钮（stb_image_resize2 已可用）；shader 创建批量耗时超阈值则记 follow-up（不改本期设计） |
| **R6** 模型层级假设：cottage 787 节点无 translation（几何世界坐标），若个别资产依赖嵌套 node 变换则错位 | 低 | 首跑截图目检；发现错位则记录为 gltf_loader 已知限制（Phase 2 既定，不在本期扩） |

## 7. 验证矩阵

| 步骤 | RADV（当前 Strix Halo 机） | Intel | NVIDIA |
|---|---|---|---|
| M1 | llm_debug 截图（transparent.glb 回归）+ 日志 | 机器在手时抽查 | 同左 |
| M2 | 启动日志 + 截图（flat 基线） | 同左 | 同左 |
| M3 | llm_debug 纹理目检 + VVL | 同左 | 同左 + spike 补测回填 heap_texture_research |

## 8. 预估

M1 半天 / M2 1~1.5 天 / M3 1 天 / M4 半天 ≈ **3~4 人天**（R1 若触发回退链 +0.5~1 天）。
