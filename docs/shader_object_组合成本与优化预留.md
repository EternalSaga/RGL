# Shader object 组合成本与优化预留（312 条目从哪来、付了什么、以后怎么优化）

> **状态**：现行（2026-09-28 立）。记录管线表条目数的生成机制、实测成本账本与优化预留方向。
> 优化项**均未实施**——触发条件（§5.1）满足前不动手。数据基准 = bistro_exterior 场景 +
> 2026-09-28 19:57 启动日志（forward，build 侧 diagnostics.picking 临时开）。

## §1 结论速览

当前场景（`models/bistro_exterior.glb`，1591 primitive / 132 材质 / 395 图片）的管线表：

| 项 | 数量 | 来源 |
|---|---|---|
| 模型条目 | **312** = 264 pbr + 25 vegetation + 23 light | 六元组去重（§2.4） |
| picking 克隆 | 312（`diagnostics.picking` 开时） | 每 model 条目 × picking.frag 克隆一条 |
| skybox / tonemap | 1 + 1 | 单条目 |
| **条目总数（本次运行）** | **626** | 下标 0–311 模型、312–623 克隆、624 skybox、625 tonemap |
| VkShaderEXT 句柄 | 1252 = 626 × 2 | 每条目 mesh + frag 一次 `createShadersEXT`（shader.cppm:316） |
| 启动创建耗时 | **≈2.3s** = 312 × ~7.4ms | 日志 19:57:31.167 → 19:57:33.486 |

**一句话**：312 = distinct(16MB 顶点块, 材质) 对数 = 132 材质按块跨度展开（§2.5）。纹理维度已按
唯一图去重到最小；放大器是 30 个顶点块（§2.1）。膨胀的根因是架构选择：binding mapping 创建期
烘焙进 shader object 且不可改（§4）。

## §2 312 的生成机制

### 2.1 顶点侧：30 = 30 个 16MB 顶点块槽（不是"30 个模型"）

`model_importer_impl.cppm:116-140`（`kVertexChunkBytes = 16MB`，`computeChunks`）：

- 1591 primitive 去索引展开为 48B/顶点（`ModelVertex` = vec3+vec3+vec2+vec4）；
- 61 个 doubleSided primitive 追加绕序翻转背面副本（顶点 ×2，`gltf_loader_impl.cppm:469-485`）；
- 合计 **9,333,990 顶点 ≈ 427 MiB**（日志 `gltf_loader_impl.cppm:583`），顺序拼进 ≤16MB 块 →
  **30 块 = 30 个 BDA buffer = 30 个几何 heap 槽**（日志 `model: 1591 primitives → 30 块`，
  `model_importer_impl.cppm:243`）。同块 primitive 共享顶点槽，跨块则槽不同。

### 2.2 纹理侧：405 图片资产（四通道独立去重）

`texture_pool.cppm:254-289`：base/normal/orm/emissive **四类各自独立的 image 下标去重缓存**
（同图跨通道会重复上传，sRGB/线性路不同）→ 395 张 glb 图落 405 个纹理资产 + 白纹理 + 中性法线
= 407（日志 `textures: 407 资产`，`texture_pool.cppm:234`）：

| 通道 | 独立图片 | 回落 | 条目内不同取值 |
|---|---|---|---|
| base | 132（132 材质各一张） | 白 | 132 |
| normal | 132（132 材质各一张） | 中性法线 | 132 |
| orm | 131（11 primitive 无 ORM） | 白（4096） | 132 = 131 + 白 |
| emissive | 10（102 primitive 引用） | 白（4096） | 11 = 10 + 白 |

### 2.3 spec 侧：材质名连连看（spec 是材质的纯函数）

`main_scene.cppm:150-168` + `configs/scene.lua material_styles`：材质名含 `Emissive` → light
（**12** 材质，scene.lua 注释写 10 已漂移）、含 `Foliage` → vegetation（10）、其余 → pbr（110）。
forward 经 `styles` 表落规格名：`model_light` / `model_vegetation` / `model_pbr_ibl`。

### 2.4 去重入口：六元组（不是五元组）

`shader_registry_impl.cppm:25-29`：

```cpp
const auto key = std::make_tuple(specName, vertexSlot, textureSlot, normalSlot, ormSlot, emissiveSlot);
```

每 primitive 调一次 `addModelEntry`（`main_scene.cppm:169-180`），同组合返回既有条目。sampler 槽
不进 key（全场景单值 `ensureShared`）；scene data / env 槽同理恒等。条目名 =
`{spec}.v{v}.t{t}.n{n}.o{o}.e{e}`（`shader_registry_impl.cppm:65`），逐条 debug 日志
（`:74`）可直接统计。

> 已知文档债：`shader_registry.cppm:58-59` 类注释仍写"五元组"（Phase 8 ③ 加 emissive 槽前的
> 残留）；`addModelEntry` 接口注释（`:178`）与 impl（`:27`）均为"六元组"，以后者为准。

### 2.5 312 = 块跨度展开

条目数 = distinct (块, 材质) 对数（spec 是材质纯函数、每材质纹理四元组固定，均不再分裂）。
132 材质的块跨度分布（raw glb 模拟，附录 A）：

| 跨块数 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 材质数 | 68 | 27 | 11 | 9 | 5 | 2 | 3 | 3 | 2 | 1 | 1 |

68×1 + 27×2 + 11×3 + 9×4 + 5×5 + 2×6 + 3×7 + 3×8 + 2×9 + 1×10 + 1×11 = **312**。
按 spec 切：110 pbr 材质 → 264、10 vegetation → 25、12 light → 23。

## §3 成本账本（付了什么 / 换来了什么）

**付（启动期，一次性；shader 不变不重复）**：

- 创建耗时 ≈2.3s。312 条均匀 ~7.4ms/条——**不随 SPV 重复出现递减**（全部条目仅共享
  1 mesh SPV + 3 frag SPV），说明驱动按 (SPV, mapping) 逐条做创建期工作（§6 尽调项 ③）。
  SPV 读盘可忽略（6.3KB + 6~20KB × 624 次 ≈ 8MB，page cache）；模型路径 `loadShaderEffect`
  无 SPV 缓存（每条重读 2 文件），picking 路径有 `meshCache` 先例（`shader_registry_impl.cppm:88-103`）。
- DGC execution set 容量 = 2 × 条目数 = 624 slots（`vk_indirect_commands.cppm:38-39`）。
- GPU 显存：spec 保证同 SPIR-V 不同 mapping 的编译二进制相同（`VkShaderCreateInfoEXT`
  VU 注，本地 spec v1.4.360）→ 312 条目共享 3 份编译产物，增量只是句柄 + mapping 元数据。

**换（每帧）**：1591 draw × 3 条命令（`pushDataEXT` 4B + `bindShadersEXT` +
`drawMeshTasksEXT`，`vkrenderer_impl.cppm:424-430`），零描述符 update/bind 操作；heap 帧首绑一次
（`:385-397`）。无 VkPipeline、无 descriptor set/pool。

## §4 为什么"多一个纹理槽 = 多一个 shader object"

三层结构（纹理槽存在哪）：

```
堆槽（启动期写完，终生不变）        slot N = {SAMPLED_IMAGE, 某 imageView}
shader object mapping（创建期烘焙） binding 1 → heapOffset = N × descriptorSize
draw（3 条命令，无任何绑纹理步骤）   换纹理 = 换 shader object 句柄
```

- Slang 的 `uniform texture2d<float> : register(t1)` 只有 binding 号；"哪张图"只存在于 mapping。
- mapping 随 `vkCreateShadersEXT` 冻结（`shader.cppm:244-258`，
  `VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT`），**无创建后修改 API**。
  新纹理槽 = 新 mapping 组合 = 新对象（若组合未见）。
- 对比传统路线：纹理在 descriptor set 里 per-draw 供给，加纹理不加管线；本路线把 per-set 的
  "指哪张图"摊到了 per-shader-object。

**这不是 spec 强制**：`VkDescriptorSetAndBindingMappingEXT::sourceData` 是 union（本地 spec
v1.4.360 与系统头 v357 均含全部取值）：

| source | 槽怎么定 | 效果 |
|---|---|---|
| `HEAP_WITH_CONSTANT_OFFSET`（本项目，shader.cppm:253） | 创建期常量 | 纹理 = 对象身份 → 组合枚举 |
| `HEAP_WITH_PUSH_INDEX` | `base + *(pushData + pushOffset)`，per-draw | 一个对象服务所有纹理 |
| `HEAP_WITH_INDIRECT_INDEX(_ARRAY)` | GPU buffer 读索引 | 同上，GPU 侧索引 |

类比：constant offset = 静态链接（每纹理组合一个"二进制"，运行期零解析）；push index =
动态链接/PLT（draw 时查一次）。选 constant offset 的理由：本机 CPU-bound（Phase 9 基线
frame 7.10ms ≈ cpu.record 7.03ms，CPU 侧零余量，`docs/phase9_plan.md` §10），per-draw 索引解析
方向相反；descriptor heap 是 RADV 实验特性，变体 source 更缺测试（踩洞史见
`docs/DESCRIPTOR_HEAP_SPEC.md`、`heap_texture_spike`）；DGC 路径下执行集合绑定完全静态，
push-index 会扩大 GPU 生成序列的正确性面。

## §5 优化方向（预留，未实施）

### 5.1 触发条件（满足任一再动手）

- 启动时间成为痛点（shader 高频迭代 / CI 跑测 / 换场景后条目数量级增长）；
- 或 §6 尽调项 ①③ 已在别的工作里顺手确认，实施成本骤降。

### 5.2 L0：运行内 SPV 字节缓存（低成本先做，收益小）

模型路径按 (path, mtime/内容 hash) 缓存 SPV 字节，312 条只读 1 + 3 次文件。仿 picking 的
`meshCache` 先例。收益 ~几十 ms（读盘非瓶颈），价值主要是消灭重复 I/O。

### 5.3 L1：磁盘二进制缓存（真正的启动优化，"binary hint"）

API 真实存在（本地 spec v1.4.360 + 系统头 v357）：

- `vkGetShaderBinaryDataEXT(device, shader, &size, data)`：取回驱动编译二进制
  （`VK_INCOMPLETE` = 缓冲区不足，全有或全无）；
- `codeType = VK_SHADER_CODE_TYPE_BINARY_EXT` + `pCode`（16B 对齐）：免 SPIR-V 编译重建。

**Key 设计**（spec 依据：`chapters/shaders.adoc` "Mappings are ignored when codeType is
VK_SHADER_CODE_TYPE_BINARY_EXT" → mapping 在首次创建时**编进二进制**，二进制是 mapping 相关的）：

```
key = (driverID, driverUUID, apiVersion,        // 二进制 specific to physical device，驱动更新即失效
       hash(mesh SPV 字节), hash(frag SPV 字节), // just reflect / 热重载改 SPV → 自然失效
       六元组 ≈ 条目名去掉 spec 前缀)            // 与现有去重 key 同源，零新机制
```

流程：查 key → 命中 `codeType=BINARY` 创建 → **失败回落 SPIR-V 并删坏缓存**（同
`shader_hot_reload` 的 buildReplacement 失败降级模式）；未命中 SPIR-V 创建 →
`vkGetShaderBinaryDataEXT` 尽力落盘（失败不影响启动）。缓存目录放 build/ 下即可
（clean-all 清掉无妨）。理论命中 312/312（§3 的均匀 7.4ms 推断驱动无跨调用去重），
二启后启动最多省 ~2s。

### 5.4 路线级（减对象数，非缓存能解）

- 16MB 顶点块粒度调大（如 256MB → ~2 块）→ 条目趋近材质数 132；
- push-index mapping（§4 表）→ 条目 ≈ spec 数 3，但 per-draw 解析成本回来，属路线级决策。

缓存（§5.3）只压缩创建时间，312 句柄 / 624 DGC 槽 / 每帧 bind 一个都不少。

## §6 实施前尽调项（L1 开工前必须收口）

1. **linked object 的二进制语义（第一风险）**：本项目 mesh+frag 一次 linked 创建
   （`mesh.nextStage = FRAGMENT`）。`vkGetShaderBinaryDataEXT` 返回 per-stage 二进制还是 linked
   程序一份？从二进制重建时链上两个 createInfo 各自怎么给码？spec 此处含混 → VVL 下小实验确认。
2. **RADV 自带缓存行为**：是否落盘 ISPIR-V 缓存（删缓存目录 / 换驱动版本观察创建耗时）。
   若驱动已有 on-disk cache，L1 增量收益缩水。
3. **成本构成 spike**："同 (SPV, mapping) 连建 N 次" vs "每次换 mapping"，分离
   SPIR-V 编译 / 校验 / handle 分配。若大头是校验 + handle，二进制缓存收益有限。

## §7 代码与日志锚点

| 内容 | 位置 |
|---|---|
| 顶点 16MB 分块 | `rgl/model_importer_impl.cppm:116-140` |
| doubleSided 背面副本（顶点 ×2） | `rgl/gltf_loader_impl.cppm:469-485` |
| 纹理四通道独立去重 | `rgl/texture_pool.cppm:220-289` |
| 六元组去重 + 条目名 + 逐条日志 | `rgl/shader_registry_impl.cppm:22-78` |
| 去重表成员（六元组） | `rgl/shader_registry.cppm:352`（过期"五元组"注释 :58-59） |
| spec 派生 + per-primitive 建条目 | `rgl/main_scene.cppm:149-180` |
| mapping 烘焙（constant offset） | `rgl/shader.cppm:236-319` |
| 帧首绑 heap + draw 3 命令 | `rgl/vkrenderer_impl.cppm:376-398, 424-430` |
| DGC 容量 2×条目 | `rgl/vk_indirect_commands.cppm:38-39` |
| 材质风格规则 | `configs/scene.lua`（`material_styles` / `styles`） |
| 基准日志 | `build/rgl/logs/gllog.txt`（2026-09-28 19:57 运行；build 侧 picking 临时开） |

## 附录 A：312 的独立复现（raw glb + 项目算法，2026-09-28 逐项吻合）

纯 Python 重放（无项目依赖）：解析 glb JSON → 按 mesh/primitive 序逐 primitive：
顶点数 = indices 数（无 indices = POSITION 数）× (doubleSided ? 2 : 1)，字节 = ×48B，
顺序拼 16MB 块（`off>0 && off+b > 16MB` 进新块）；纹理键 = (base/normal/orm/emissive 的
texture index 直接当 image index——loader 假设 texture 下标 == image 下标，
`gltf_loader_impl.cppm:265-273`；-1 = 回落)；spec = 材质名含 Emissive/Foliage 连连看。
条目 = distinct (spec, 块, 四键)。

```
primitives: 1591 (log 1591)   total vertices: 9333990 (log 9333990)
chunks: 30 (log 30)           materials: 132 (log 132)
entries: 312 (log 312)        pbr 264 / vegetation 25 / light 23 (log 同)
distinct per dim: 3/30/132/132/132/11 (log 同)
```

复现时注意：loader 对 texture 的 `index` **不查 textures 数组**、直接当 image index 用
（越界 = -1）；`just reflect` 或资产变更后数字会变，本文档数据以 §7 基准日志为准。
