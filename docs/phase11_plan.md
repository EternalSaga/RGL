# Phase 11 计划：阴影 + 前置修复批（光照质感与资产管线）

> **状态**：现行（实施计划，2026-09-29 立）。本文件立两件事：
> ① **批 11.0 前置修复**——开工前先解决"光源是绿的 / 湿地不像水 / 背光面死黑"三问题
> （用户 2026-09-29 提出，调查结论见 §2）；② **阴影主体**——原编号 Phase 10，
> 2026-09-27 因调试交互插队顺延为 Phase 11，其大纲沿用 `next_roadmap.md` §Phase 11 原条目，
> 本文件不重复展开。
> 结论回写 [next_roadmap.md](next_roadmap.md) 与 [调试历史.md](调试历史.md)。

---

## 1. 目标

| # | 需求（用户原话） | 验收 |
|---|---|---|
| P1 | "光源是绿色的，这不合理" | 串灯/路灯/店招自发光的**本色**（橙/蓝/白/粉/红/绿各归各位），无绿色偏色 |
| P2 | "潮湿地面反射效果太差" | 湿润区可辨为水膜，且**不是过曝白板** |
| P3 | "楼房背光面暗到不现实" | 背光面可见砖纹/窗框等细节，不死黑 |

三问题在 Phase 8 之后长期挂账。调查发现 **P2/P3 同根**，P1 是独立的资产管线缺陷。

---

## 2. 调查结论（2026-09-29 实测）

> 方法：headless + `diagnostics.rpc` 通道（`POST /camera` / `POST /param` / `POST /shot`）
> 摆位取图 + 逐材质改 baseColor 定位像素归属 + 单变量参数扫描 + 资产侧 glb/FBX/贴图直解。
> 采样口径：`assets/bistro_exterior.glb`（PIL 直读 PNG chunk）、`Bistro_v5_2/`（assimp 重导出 + ffmpeg 解 DDS）。

### 2.1 P1 绿光源 = `tools/bistro_pack.py` 漏 remap（纯资产 bug）

`bistro_pack.py` 的 `referenced` 集合（L71-73）与 remap 循环（L250-258）都只覆盖
`baseColorTexture / metallicRoughnessTexture / normalTexture / occlusionTexture`，
**`emissiveTexture` 两个名单都没进**。后果两层：

1. emissive 贴图**从未被打包进 glb**（`referenced` 不收它 → 不转码 → 不在 `new_images`）；
2. glb 里 `emissiveTexture.index` 保留**旧 image 下标**，指向重排后的**另一张图**。

实测证据（材质 12 `Emissive_StreetLight` 与材质 1 `Paris_StringLights_01_Orange_Color_Emissive`）：

| 材质 | `emissiveTexture.index` | 实际指向 | 该图内容 | 同材质 `metallicRoughnessTexture` |
|---|---|---|---|---|
| `Emissive_StreetLight` (12) | 44 | image 44 | **纯绿 (0,188,0)** | index 38 |
| `Paris_StringLights_01_Orange_Color_Emissive` (1) | 5 | image 5 | **纯绿 (0,188,0)** | index 5（**同图**） |

那张"绿图"就是 Bistro 的 `_Specular.dds`（README 权威定义 R=Aocclusion / G=Roughness / B=Metalness），
且只有 **496 字节**（= 1×1 常量贴图）。于是：

```
emissive = emissiveFactor(0.5) × KHR_materials_emissive_strength(4.0) × (0, 0.737, 0) = 纯绿
```

真正的贴图**一直都在本地**：`Bistro_v5_2/Textures/Paris_StringLights_01_Orange_Color_Emissive_Emissive.dds`
（2872 字节，橙色）。10 个 emissive 材质（1-6 / 12 / 13 / 38 / 39）全部受害。

**连带发现**：`model_light.frag.slang` 的「① metallic 恒 0 / ② emission × 1/4 / 暗绿玻璃定案」
是**为掩盖绿光而加的 look-dev 补丁**，绿光根因消除后其前提失效，须一并重估
（否则会出现"修了变暗"的反向回归）。

**官方对照**：NVIDIA ORCA 页 `Bistro_Exterior_2.png` 里串灯是**橙/黄/白/绿/粉多色小球**，
吊灯为暖黄——证实 P1 确为缺陷而非美术本意。

### 2.2 P2/P3 = 光照配比失衡（75:1），**不是 bistro 就这样**

`BistroExterior.pyscene` 全文只有两行有效信息：

```python
sceneBuilder.envMap = EnvMap("san_giuseppe_bridge_4k.hdr")
sceneBuilder.envMap.intensity = 10          # ← 官方标定值
```

我们 `configs/scene.lua` 用的是 `environment.intensity = 0.2` + 手搓 `light.intensity = 15`
（`light.color` CPU 预乘进 scene data @24），**sun:env ≈ 75:1**，IBL 被压到 1/50。

单变量扫描（同一机位，材质 10 = `Pavement_Cobblestone_Wet_BLENDSHADER`，
已用改红精确定位像素归属）：

| sun / env | 湿地 p50 | 湿地过曝% | 背光黑砖墙 p50 | 全图 p10/p50/p90 |
|---|---|---|---|---|
| 15 / 0.2（现状） | 173 | 0.0 | **19**（死黑） | 14 / 150 / 218 |
| 8 / 0.6 | 135 | 0.16 | 46 | 31 / 112 / 187 |
| **6 / 0.7** | 125 | 0.87 | **52** | 33 / 103 / 175 |
| 官方参考图 | 206 | 0.01 | — | 18 / 91 / 218 |

`env=0` 对照组：湿地 p50 掉到 54 且**完全失去水感**、背光墙 p50 回到 19 →
**湿地的"水感"与背光面的可见性 100% 来自 IBL 项**。

### 2.3 决定性发现：资产里根本没有"湿"的数据

| 检查项 | 结果 |
|---|---|
| `Pavement_Cobblestone_Wet_BLENDSHADER_Specular.dds` | **496 字节** 1×1 常量 (0,188,0) |
| `Pavement_Cobblestone_01_BLENDSHADER`（**干**）的同项 | **496 字节** 1×1 常量 (0,188,0) |
| `Pavement_Ground_Wet` / `_Cobblestone_02` / `Pavement_Curbstones` | 同为 (0,188,0) |
| 全场景统计 | **128/131** 材质 ORM 贴图是常量；R(AO) 恒 0 |
| wet 材质 BaseColor alpha | 恒 255（无 opacity 变化） |
| wet 材质 BaseColor 贴图本体 | **干燥的蓝灰鹅卵石**（原图直读确认，无任何湿的暗化） |
| glb 内 baseColor vs 原版 DDS | 逐通道 mean 完全一致（86.4/84.7/83.1），仅 2048→1024 降采样 |

**wet 与 dry 的 PBR 参数逐字节相同**（roughness 均 0.737、metallic 均 0、AO 均 0、不透明），
唯一区别是材质名里的 `Wet` 字样 + 独立 mesh。

**NVIDIA ORCA 页 History 揭示了真相**：

> *Revealed at 2017 GDC to showcase new **anti-aliasing and transparency** features*

材质名后缀 `_BLENDSHADER` = Source 2 的**自定义 blend shader 变体**命名——水膜由该 shader 在
引擎侧算出。而 ORCA 只发布 FBX + 贴图 + pyscene，**未发布 Source 2 的 shader 代码**。
官方参考图为 **Falcor 路径追踪**产物。

### 2.4 对既有延后项的修正（`next_roadmap.md` L182-189）

该处已列 6 项补齐清单（2026-09-14 用户拍板延后），本次调研**验证 3 项、修正 2 项、新增 1 项**：

| 原条目 | 本次结论 |
|---|---|
| ① 反射源（planar reflection / SSR） | **降级为非必要**。官方 `Bistro_Exterior_1` 湿地 RGB 饱和度仅 **14**（近灰白）→ 主要反射的是**明亮天空**，IBL cubemap 原理上已覆盖。只有 `Bistro_Exterior_2`（室内机位，天空被建筑挡死）才需要真 SSR |
| ② 水洼状粗糙度贴图（洼 0.05 / 湿石 0.3-0.5） | **升级为 P0 必要**。资产未给湿/干区分（§2.3），必须自定。实测材质 10 roughness 0.737→0.12 立即呈现明显水膜 |
| ③ 湿区 albedo × 0.5-0.7 压暗 | **改为渲染时计算**，不做手工贴图。官方贴图无湿的暗化（§2.3），压暗应来自法线朝上程度 × AO |
| ④ 水膜法线扰动 | 保留（"水"与"玻璃"的区分要素） |
| ⑤ 太阳 glint | 保留（GGX 机制已在，被粗糙度分布解锁） |
| ⑥ AO/接触阴影（石缝积水暗部） | **升级为 P0 必要**。这是湿/干对比度的**唯一**来源——官方图里石缝暗、石块顶面亮，靠的是 path tracing 的接触遮蔽，我们无 AO 时缝隙与顶面同亮 |

### 2.5 能力缺口分级（回应"水面必须加反射和 AO 吗"）

**不是两者都要，按必要性分级：**

| 缺口 | 必要性 | 理由 |
|---|---|---|
| **AO / 接触遮蔽** | **必须** | 湿/干对比度唯一来源（§2.4⑥）。资产无 AO 数据，只能自算。投入产出比最高 |
| **湿面 roughness 定值** | **必须** | 资产 wet=dry（§2.3）。无此项则与干地视觉无异 |
| **局部反射 / SSR** | **可选** | Exterior_1 视角不需要（反射天空即可）；Interior_2 类机位才需要。若做，planar reflection（单平面镜像相机）对 forward/deferred 都成立，不破 D1 对等；SSR 属后处理 → deferred-only |
| **env 强度** | **必须（参数）** | §2.2，0.2→0.7 直接把背光墙从 19 拉到 52 |

### 2.6 附带发现（本次调查的副产品）

1. **`pack.py` 丢弃 FBX 材质标量**：assimp 重导出显示 132 材质全为
   `roughnessFactor = 0.5528 / metallicFactor = 0.0`，而 `pack.py` L69-70 强制写 `= 1.0`。
   正确解析应为 `0.5528 × 0.737 ≈ 0.407` 而非现在的 0.737。
   **待定**：README 只定义了贴图通道语义，未说 factor 权威性；需先定夺再改。
2. **FBX 里的日光全丢**：`assimp export` 产物的 `extensions: []`（无 `KHR_lights_punctual`），
   CHANGELOG v5 声称 "Embedded ... lights into FBX file"。当前 `light.direction = (0.5,-0.75,-0.45)`
   是手搓的，未必是官方太阳方向。
3. **cubemap 隐式 LOD 无问题**：探针实验（skybox `Sample` → `SampleLevel(dir, 0.0)`）输出
   **bit-identical**，隐式 LOD 本就为 0；canary 6 面 × 2 texel 回读 PASS。排除 RADV 走 2DArray
   硬件路径导致 LOD 偏 `log2(6)≈2.6` 的怀疑。**该探针改动已 revert。**

### 2.7 RPC 通道的两个缺陷（Phase 10b 批 11b.2 遗留）

1. **`POST /camera` 的 `target` 在 fps 模式下是死写入**。`camera.cppm:150-151`：
   trackball 用 `lookAt(position, target, up)`，fps 用
   `lookAt(position, position + cameraForward(yaw,pitch), up)`，`target` 不参与计算。
   但 `/state` 照样回显 target → **"接口 ok + 状态已改 + 画面不动"的假成功**。
   `position` / `up` 有效，仅 `target` 无效。
2. **多实例静默串扰**：`cpp-httplib` 在 Linux 启用 `SO_REUSEPORT`，同时跑 3 个实例时
   **三个日志都打印"监听 127.0.0.1:8788"成功**，端口由内核分发，请求随机打到不同进程。
   （本次调查中一度因此出现"视角对不上"的乱象。`AGENTS.md`「窗口测量纪律」要求单实例串行，
   需补一条"先 `pgrep -cx rgl` 确认"。）

**正面结论**：RPC 的**错误处理是干净的**——404（label/page/route）、400（bad mode / 畸形 JSON /
缺字段）、409（`/shot` 互斥、`llm_debug` 在跑）、并发 `/shot` 互斥全部正确。

---

## 3. 决策表

| # | 决策 | 取值 | 理由 |
|---|---|---|---|
| D1 | 前置批编号 | **批 11.0**，独立于阴影主体 | 三问题与阴影无技术耦合，但会**污染阴影的视觉基线**（阴影一开，暗部更暗），必须先清 |
| D2 | P1 修复方式 | **修 `bistro_pack.py` 重跑 pack** | 根因在资产管线，渲染端任何补偿都是补丁。assimp 已装，重跑约 5-10 min |
| D3 | P1 修完是否保留 ×1/4 | **不保留，重新标定** | 该 hack 的前提（绿光掩盖）消失，留着会"修了变暗"。改为按 glTF 原值 + 场景曝光重新定标 |
| D4 | P2/P3 修复方式 | **先改配置**（`sun 15→6` / `env 0.2→0.7` / `exposure 1.2→1.0`），**AO 与湿面 roughness 留到阴影批** | 配置改动零风险、立刻见效；AO 需要新管线（不进阴影批就是凭空插一条 pass） |
| D5 | 湿面 roughness 载体 | **材质名规则表**（`configs/scene.lua` 的 `material_styles` 已是此形态） | 资产不给湿/干区分，只能由配置按材质名路由；复用现成机制，不新增机制 |
| D6 | AO 实现形态 | **批 11.2（随 CSM）** | CSM 的 depth 已含遮挡信息，AO 可复用；且 AO 是阴影的自然延伸，Phase 9 已把 ⑥ 列在阴影条目下 |
| D7 | SSR 是否进 Phase 11 | **不进**，列 Phase 12 候选 | §2.5：Exterior_1 不需要。若进，优先 planar reflection（forward/deferred 都成立，不破 D1 对等） |
| D8 | FBX `roughnessFactor` | **暂不改**，先在 §2.6-1 定夺 | 改错方向会让全场景更粗糙；且与 D5 的湿面 roughness 定值耦合，宜合并决策 |
| D9 | RPC 缺陷修复排期 | **随批 11.0 顺手修** | fps 拒收 `target` 或加明确回显提示 + bind 日志加 pid；都是小改动 |

---

## 4. 批次

### 批 11.0 — 前置修复（不涉及渲染管线，纯配置 + 资产管线）

| 步骤 | 内容 | 验收 |
|---|---|---|
| 11.0a | `pack.py`：`referenced` 收 `emissiveTexture`；remap 循环补 `emissiveTexture` | 重跑 pack 后 10 个 emissive 材质的 `emissiveTexture.index` 指向**橙色/蓝/白/粉/红/绿各异**的真图 |
| 11.0b | 重跑 `assimp export` + `pack.py`，重载模型 | 目检：串灯多色、路灯暖黄、店招本色 |
| 11.0c | `model_light.frag.slang` 重估 ×1/4（决策 D3） | 白天灯罩显本色不炸、夜景不过曝 |
| 11.0d | `configs/scene.lua`：`light.intensity 15→6`、`environment.intensity 0.2→0.7`、`exposure 1.2→1.0` | 背光黑砖墙 p50 从 19 → 50+；全图饱和 <1% |
| 11.0e | RPC 缺陷修复（D9） | fps 模式 POST `target` 有明确语义；bind 日志带 pid；`pgrep -cx rgl` 入测量纪律 |

**红线**：11.0a/b 会改 479 MB glb → 需重跑全量冒烟 + BIT-IDENTICAL 基线重建。
11.0d 改配置即改视觉基线 → BIT-IDENTICAL 回归须同会话双构建（skill `rgl-verify` 方法学）。

### 批 11.1 — 保守光栅化基建

见 `next_roadmap.md` §Phase 11 ①：`VK_EXT_conservative_rasterization`（RADV 26.2.1 已实测 rev 1），
CSM/HFTS 共同硬前提。

### 批 11.2 — CSM【承诺交付】+ AO（本次新增）

见 `next_roadmap.md` §Phase 11 ②（4 级 cascade + PCSS + 光锥剔除复用 ④c 编舞）。
**新增**：AO / 接触阴影（§2.4⑥ + D6）—— 目标是把"石缝暗、石块亮"做出来，
这是湿地"像水"的最后一块拼图。GPU 预算仍受 Phase 9 的 **≤3 ms/帧** 硬约束。

### 批 11.3 — HFTS/IzB spike（带退出条件，非承诺项）

见 `next_roadmap.md` §Phase 11 ③（FTIZB 优先，达不到即收手）。

---

## 5. 红线与风险

1. **阴影会让 P3 更糟**：阴影只减少直射光，背光面问题靠 env 提亮解决。**11.0d 必须先于阴影落地**，
   否则阴影一开，画面比现在更糟、且会被误判为阴影 bug。
2. **本机 CPU-bound，无 CPU 余量**（Phase 9 批 9.4 判定：CPU 7.03 ms / GPU 2.98 ms）。
   新增 pass（阴影 depth、AO）只加 GPU 成本，≤3 ms 内则整机帧时不变；超了直接掉帧。
3. **AO 若走后处理会破 D1 对等**（deferred-only）—— 与 SSR 同病。优先考虑能同时服务两路的前向形态。
4. **`pack.py` 重跑会改变 132 材质的 `emissive` 强度**（绿 → 本色 × 2.0），
   夜间场景可能过曝；`Bistro_Night` 类对比要单独看。
5. **测量纪律**：`AGENTS.md` 已要求单实例串行（并行实例互相遮挡合成会扭曲帧率与 present timing），
   本次追加 **RPC 场景下的 `pgrep -cx rgl` 前置检查**（`SO_REUSEPORT` 会让多实例静默共存）。

---

## 6. 验收清单

> 状态以 §8 落地记录为准（2026-09-29）。判据被 §8.1 修正过，以修正后的为准。

- [x] 批 11.0a：`pack.py` 两处补齐，代码走 `just checks --strict` 0 警告
- [x] 批 11.0b：重跑 pack 成功；10 个 emissive 材质自发光色各异（橙/蓝/白/粉/红/绿 + 路灯暖黄 + 店招本色）
- [ ] 批 11.0c：`model_light` 无 ×1/4 hack，白天不过曝 —— **未做**，绿光源根因已除后需重新标定
- [x] 批 11.0d：判据改为**同材质阴影/受光线性比 0.0022 → 0.037**（落进 0.03~0.10 带内）；
      全图饱和 0.78% → 0.26%（< 1%）；湿地无成片过曝
- [x] 批 11.0e：fps + `target` 有明确行为（`cameraAimAt` 反推 yaw/pitch）；
      "bind 日志带 pid" 改为 bind 前探测（GMF 红线不允许 `<unistd.h>`）
- [x] 批 11.4（新增）：SSAO 落地，默认 strength 0.5；`strength=0` 在三个 shader 版本间
      截图 sha256 相同（逐位旁路成立）；GPU 代价 1.5%（128 → 125 fps）
- [x] 湿面：`pack.py` 的 `WET_ROUGHNESS_SCALE`；资产侧结果与 RPC 逐材质验证**逐位一致**
- [~] 批 11.1：保守光栅化 RADV 能力**已实测**（§9）—— OVERESTIMATE 基线可用、
      `maxExtraPrimitiveOverestimationSize = 0` 无外扩余量、可走 DynamicState3。
      **代码未接**：阴影 pass 尚不存在，无消费者的扩展不预先请求（接入口 = 批 11.2 第 3 步）
- [ ] 批 11.2：CSM 4 级 + PCSS；GPU ≤ 3 ms
- [ ] 批 11.3：HFTS spike 出结论（做 / 收手二选一）
- [x] BIT-IDENTICAL **新基线已建立（2026-09-29）**：默认配置 + 默认机位 + llm_debug 路径，
      forward `md5 7a87251793dba125…`（sha256 `2ebb1f33036bc003…`）。同会话连跑两次逐位相同；
      deferred `md5 1139acb4e29fb2cb…`，与 forward 差异 >8 的像素 **0.0%**、均值差 0.17
      （= 批 11.0c 关掉 emission 4× 破口后的 D1 对等实测）。旧 forward 基线 `10b00c78…` 作废。
- [ ] 跨驱动（NVIDIA / Intel）复测

---

## 7. 文档回写

- [ ] `next_roadmap.md` §Phase 11：插入批 11.0 段 + 修正 L182-189 延后项（§2.4）
- [ ] `AGENTS.md` 已知问题：新增"地面水质感"三缺口（AO/湿面 roughness/局部反射）
- [ ] `AGENTS.md` 运行要点：RPC 测量纪律补 `pgrep -cx rgl`
- [ ] `调试历史.md` 2026-09-29 条目：绿光源根因（`pack.py` remap 缺漏）+ 光照配比 75:1
- [ ] `docs/archive/phase11_plan.md`（收口时归档）

---

## 8. 落地记录（2026-09-29，批 11.0a/b/e + 新增批 11.4）

### 8.1 与原计划的偏差（**结论有变，不是照抄**）

| 计划项 | 计划值 | 实际落地 | 为什么改 |
|---|---|---|---|
| 11.0d 光照配比 | sun 15→**6**、env 0.2→**0.7**、exp 1.2→1.0，判据 = "背光黑砖墙 p50 ≥ 50" | sun 15→**12**、env 0.2→**1.0**、exp 1.2→**1.0**，判据换成**同材质阴影/受光线性比 ∈ 0.03~0.10** | 原判据的区域坐标与取景对不上（见 8.2），且 sun 6 会把画面压得没有直射感 |
| 11.0e | "bind 日志带 pid" | 改为 **bind 前 httplib Client 探测**：连得上 = 已有监听者 → error + return false | `getpid()` 要 `<unistd.h>`，GMF 红线禁止；探测同时解决"多实例串扰"这个真问题 |
| 11.0c | `model_light` 去 ×1/4 | **未做**（留后续） | 绿光源根因已除后需重新标定，且需要夜景对比图才能判断 |

### 8.2 方法论教训（本批最贵的一条）

三次走错方向，全部源于**在错误的区域上量数据**：

1. 第一轮扫描 32 组光照配比，整轮跑在**默认机位**上（重启实例后没重新 `aim`），
   区域数字全废；
2. 坐标约定 `[560:700, 300:850]` 是 **numpy `[rows, cols]`**，我按 `[x0:x1, y0:y1]` 读，
   于是"背光墙"实际量到的是**屋顶上的微缩模型**（coverage 4.8 kpx）；
3. 材质定位用 **baseColor（乘法）闪色**：纹理本身就暗的表面 ×红色后变化 < 阈值 40，
   于是画面里最大的那批暗面被整体漏判为"无可见变化"。

**正确做法**（已固化进 `/tmp/opencode/flash_mat3.py`）：`emissive`（**加法**）闪色 + 像素差分。
加法项与 albedo 亮度无关，再暗的表面也亮得起来；再叠一张"连拍两张基线求噪声掩膜"，
剔除透明段每帧距离排序抖动造成的恒定 ~2.4 kpx 假信号。

### 8.3 "楼房背光面过暗" = 环境光配比，不是资产 bug

定量（emissive 掩膜精确定位 #79 `MASTER_Concrete_Smooth` 与 #75 `MASTER_Concrete`）：

- 墙面 albedo 是**浅灰 150**，材质完全正常（asset 侧无问题）；
- 渲染 p50 仅 **13**（受光 p95 = 218）→ 阴影/受光线性比 **0.0022**；
- 户外散射光的合理带是 **0.03~0.10** —— 差了一个数量级；
- env 强度扫描证明 **IBL 通路无故障**（0.2→1.0 时阴影 13→66，单调线性），
  病根就是 `environment.intensity = 0.2` 这个历史值。

排除过的假设（都写在这里以免重走）：

- **不是遮挡贴图**：`model.slang` 明确"R（AO）采样不应用（留后）"，ORM 的 R=0 是惰性的；
- **不是玻璃**：5 个玻璃材质在本视图合计覆盖 0.1%；
- **不是 Source2 blend shader 丢色**：`MASTER_Wood_Painted_Green`（#71）的 baseColor
  确实是源资产里的 2048² 纯灰 140（绿色只在 `_BLENDSHADER` 变体里），但该材质在本视图
  只占 1.0%，**与"背光面"无关**（那句绿色是酒馆的木质门脸）。

### 8.4 批 11.4 SSAO 落地要点

见 `rgl/shaders/ao.frag.slang` 文件头（决策与已知近似的权威在 shader 里）。三个实测踩的坑：

1. **产物语义必须是可见度**（1=通透）不是遮蔽量。写成遮蔽量会被 tonemap 当乘数把
   开阔处（occlusion=0）乘成 0 —— 实测 strength=1 整幅全黑；
2. **ddx/ddy 必须整体位于任何分支之前**。天空早退、strength 早退都要排在导数之后，
   否则导数落入非一致控制流 = UB（天空/几何交界处的半个 quad 最易触发）；
3. **深度图不能用共享 sampler**。共享那份是 LINEAR + mipmap LINEAR，而 D32Sfloat
   深度图在 Vulkan 里**不可过滤**（invalid usage），且深度插值无物理意义、REPEAT 对
   屏幕坐标有害 → 新增 `SamplerLibrary::ensurePointSampler`（NEAREST + 无 mip + CLAMP）。

调参收敛过程（都是实测，不是猜）：

| 版本 | 效果（强度=1.0 的隐含乘数） | 结论 |
|---|---|---|
| 二值累加 + `pow(x,2)` | 立面 p50 0.94 / p1 0.64 | 噪声看似很大，实测 AO 场高频能量**低于底图自身**（0.80/0.64/0.00），是可视化放大的错觉 |
| 改加权累加 `(1−d/R)·hemi` | 立面 p1 0.933 | 权重把信号压没了（单样本只剩 0.2~0.35） |
| 加权 + `kGain=3.0` | 立面 p50 0.71 / 灌木 0.91 / 开阔地面 1.00 | **采用**；默认 strength 0.5 |

depth-only AO 的固有边界：**法线贴图细节看不见**（鹅卵石缝是法线贴图，深度图上是一整片
平面 → 开阔地面乘数恒 1.000）。这是选 depth-only 的代价，不是 bug。

### 8.5 湿面（批 11.0d 之外的第三条缺口）

`_Wet` 材质在资产侧给 `roughnessFactor = 0.163`（`WET_ROUGHNESS_SCALE`），
effective = 0.163 × ORM.G(0.737) ≈ **0.12**（水膜）。**平台期实测**：effective roughness
0.221 / 0.120 / 0.059 三档，路面带 p90 = 197.0 / 197.3 / 197.0 —— 0.12 就到顶，
因为没有 SSR，能反射的只有 IBL 高 mip（近似均匀天空辉光），再锐化也反射不出新信息。

**已知残留偏差**：真实湿面会同时压暗漫反射（水填进微观凹凸），这里只加高光不压暗，
故湿区比官方略"亮而奶"。补它需要 per-material 的 wetness 分量进 scene data
（draw data schema 扩列 = 反射管线改动），列 Phase 12 候选。

### 8.6 本批顺带清的结构债

- 装饰 pass 拆出 `rgl/render_graph_decorate.cppm`（`module render_graph:decorate`）：
  `render_graph_impl.cppm` 507 → 412 代码行，`decorateHighlightPass` 认知复杂度 16 → ~8；
- 渲染图编译器放宽"depth 附件只允许 depthWrite"（ao pass 要以 sampleRead 读 depth），
  同时把 `decorateHighlightPass` 的消费点判定收紧为**只匹配色附件** —— 否则 ao 会被
  误当消费点，colorAtt 取到 `"depth"` 与 depthAtt 撞名，编译期报
  `references attachment 'depth' more than once`。

---

## 9. 批 11.1 调查：保守光栅化在 RADV 上的真实能力（2026-09-29，vulkaninfo 实测）

原计划写"VK_EXT_conservative_rasterization，RADV 26.2.1 已实测可用 rev 1"——**这句不完整，
会误导 批 11.2**，补实测属性表：

```
VkPhysicalDeviceConservativeRasterizationPropertiesEXT:
    primitiveOverestimationSize                  = 0
    maxExtraPrimitiveOverestimationSize          = 0
    conservativePointAndLineRasterization        = false
    degenerateTrianglesRasterized                = true
    degenerateLinesRasterized                    = false
    fullyCoveredFragmentShaderInputVariable      = true
    conservativeRasterizationPostDepthCoverage   = true
（VK_EXT_conservative_rasterization : extension revision 1）
extendedDynamicState3ConservativeRasterizationMode    = true
extendedDynamicState3ExtraPrimitiveOverestimationSize = false
```

### 结论

1. **要的能力有**：`OVERESTIMATE` 模式的基线语义 = "fragment 方形与三角形**有重叠就出
   fragment**"（而非默认像元中心测试）。这正是阴影深度 pass 要的"共面三角形之间不留缝"，
   且 `primitiveOverestimationSize = 0` 不妨碍它——该值是在基线之上**再加**的量。
2. **要不到的能力要说清楚**：`maxExtraPrimitiveOverestimationSize = 0` → **无法再把三角形
   外扩**。所以"加粗 fattening 挡薄几何/双面片漏光"这条路在 RADV 上没有硬件支持，
   CSM 设计**不得依赖它**。（`extendedDynamicState3ExtraPrimitiveOverestimationSize`
   同样是 false，动态状态也救不了。）
3. **架构契合点是动态状态**：`extendedDynamicState3ConservativeRasterizationMode = true`
   → 保守模式可以走 **DynamicState**，与本项目既有的 ShaderObject + DynamicState 路线一致，
   **不需要**为阴影 pass 新增管线条目（管线表在 bistro 基准下已有 312 条，见
   `shader_object_组合成本与优化预留.md`）。
4. `conservativeRasterizationPostDepthCoverage = true` 是 CSM 之后若要做
   **early-Z 的 HFTS/FTIZB 路线**（批 11.3）的又一个前提，顺带确认它成立。

### 对 批 11.2 的直接影响（写进设计前置）

- 阴影深度 pass 用 `OVERESTIMATE` + extraSize=0，**只当作"无缝"用，不要当作"加宽"用**；
- 漏光问题改用 **normal offset（阴影着色期沿法线偏移采样点）+ slope-scaled depth bias**
  组合解决，这是 RADV 上唯一可用的手段；
- **保守模式只在阴影 pass 开**：主 pass 开 OVERESTIMATE 会让像素边缘多写深度，而 ao pass
  正是**读深度重建世界坐标**——两者相互作用属于不必要的风险面。

---

## 10. 批 11.2 CSM 实施设计（2026-09-29 定稿；两个硬阻塞 + 最小改动清单）

### 10.1 先说结论：现在的渲染图**表达不出**阴影 pass，两处硬阻塞

不是"加个 pass 就行"。逐条实测/读码确认（锚点为 2026-09-29 的 `文件:行号`）：

| # | 阻塞 | 位置 | 症状 |
|---|---|---|---|
| **B1** | **depth-only pass 被拒**：drawMesh pass 必须有 colorWrite | `render_graph_impl.cppm:169-172`（`precheckDrawPass`） | 阴影 pass 只写深度、不写色 → 编译期直接报 `draw pass 'shadow' has no colorWrite output` |
| **B2** | **depthWrite 只允许 `kind=depth`，而 `kind=depth` 恒等于 Resources 持有的**交换链尺寸主深度图 | 校验 `render_graph_impl.cppm:265-269`；解析 `vkrenderer_impl.cppm:136`（`case depth: return depthImage`）与 `:145`（`depthView`） | 阴影图需要**独立尺寸**（如 2048²，与窗口无关）、**独立生命周期**（跨帧存活给 light pass 采样）、可选**数组/图集**承载 4 级 cascade —— 现在的 `kind=depth` 一条通道表达不了 |

`RgAttachmentKind` 现仅三值：`swapchain / depth / offscreen`（`render_graph.cppm:49-53`）。

### 10.2 最小改动清单（按依赖顺序，每步可独立验收）

1. **新 kind `depthOffscreen`**（图属 transient 深度 RT，归 RenderTargetCache 而非 Resources）：
   - `render_graph.cppm:49` 枚举 + 三行职责注释；
   - `render_graph_impl.cppm` 的 `aspectForKind`（kind→aspect）**必须返回 `eDepth`** ——
     漏了就是"屏障用 eColor 转深度图"，RADV 上表现为随机黑块，最难查；
   - `registerAttachment` 的按 kind 格式词表校验：`depthOffscreen` 只收深度格式；
   - `rgFormatTable`（`render_graph_impl.cppm:444`，现 14 项）补 `d32_unorm`（主用）与
     `d16_unorm`（低带宽档）两个词；
   - `validatePassRef` 的 B2 判据放宽为"`depthWrite` 允许 `depth` 或 `depthOffscreen`"。
2. **放开 B1**：`precheckDrawPass` 改成"至少一个写附件"（colorWrite **或** depthWrite），
   并把"无 colorWrite 的 drawMesh pass 必须有 depthWrite"作为对偶校验，避免空 pass 蒙混过关。
3. **执行器侧**：`SlotResolver::imageForSlot / viewForSlot` 加 `depthOffscreen` 分支
   （`vkrenderer_impl.cppm:136/145` 旁）；RenderTargetCache 需支持深度格式 +
   **`eDepth` aspect 的 view**（采样侧只用 depth aspect，别把 stencil 带进来）。
   注意 `beginPassRendering` 的 depth 条目目前只认 `pass.depth`——要让它从 colors/depth
   计划里按 kind 取（阴影 pass 的 depth 就是它的 `depthOffscreen` 附件）。
4. **shadow pass 本体**：`kind=drawMesh`、`drawSource="shadow"`、**必须是图内第一个 pass**
   （不依赖相机深度，且放最前可让 barrier 推导最简单）；
   `outputs = { shadow_map depthWrite load=clear(1.0) store=store }`，无 color 附件。
5. **cascade 数组**：首选**单张 2D 图集**（4 级 = 2×2 tile，viewport/scissor 切换）而非
   image array —— 后者要 per-layer 描述符，descriptor heap 路线上更别扭；图集还能一次
   clear。texel snapping 在 tile 内做（否则相机平移时阴影" crawling"）。

### 10.3 与既有子系统的接点（都能复用，别新造）

| 需求 | 复用点 |
|---|---|
| 光锥剔除（阴影 pass 要按光锥而非视锥重跑一遍） | `CullDriver` 的 ④c 编舞（`cull_driver.cppm`：dispatch→preprocess→同帧等待→readCull→reconcile），新增第二组候选序列即可 |
| depth-only 的管线 | 阴影 pass **无材质分叉**：一条 depth-only 条目 + mesh 管线（顶点变换复用 `model.mesh.slang`） |
| pass 状态 | `StateKit` 新增 `shadowState`（depthCompare=LESS、depthWrite=true、cull 前背面**朝光**需另议——见下） |
| 保守光栅 | DynamicState3（§9 结论 3），不进管线表 |
| 阴影采样 | 走 scene data 或独立 heap 槽；**注意 D1 对等**：forward 的 `model_light.frag.slang` 与 deferred 的 `deferred_geometry_light.frag.slang` 都要接，否则重演批 11.0c 那种"只有一路有"的破口 |

### 10.4 两个必须提前定的取舍

1. **背面剔除方向**：阴影 pass 剔前壁还是后壁？剔后壁（常规）在薄壳/单面片上漏光；
   剔前壁（front-face culling）压掉 peter-panning 但在闭合体上把自身内部投进阴影。
   **配合 §9 结论 2（RADV 无 fattening 余量）**，建议：阴影 pass **不剔**（双面）+
   **normal offset** 采样 + slope-scaled depth bias，三件套一起调；
2. **GPU 预算**：Phase 9 基线是 GPU 侧余量 ≈3.1 ms/帧、**CPU 侧无余量**。
   阴影 depth pass 是纯 GPU 光栅，283 万三角形重跑一遍是大头——**先用 1 级 cascade 实测
   单遍成本**，再决定 4 级是否可行（4 级不等于 4×：远处 cascade 可用粗 texel + 剔掉大部分几何）。


---

## 11. 批 11.2 基建落地记录（2026-09-29，§10.2 步骤 1-4）

§10.2 的五步里**前四步已落地并逐位验证**，第五步（深度-only 管线）留给后续。

### 11.1 改了什么（按 §10.2 清单对号）

| 步骤 | 落地 | 备注 |
|---|---|---|
| ① 新 kind `depthOffscreen` | `render_graph.cppm` 枚举 + `aspectOf`/`isDepthKind`/`isRtBackedKind` + 深度格式词表 `rgDepthFormatTable` | **两表刻意分立**（颜色 `rgFormatTable` / 深度 `rgDepthFormatTable`）：合并后 `offscreen` 就能声明深度格式 → RT 缓存建出 `COLOR_ATTACHMENT` 用途 + `eColor` view = invalid usage，只有 VVL 抓得到 |
| ② 放开 B1 | `precheckDrawPass` 改成"至少一个写附件"，并补对偶校验（一个写都没有 = 空 pass 仍拒） | 对偶校验是防"放宽"变成"全放开" |
| ③ 执行器侧 | `SlotResolver` 四 kind 穷尽 + **`renderAreaFor`**（pass 的渲染区域）；`ensureOffscreenAttachments` 两类 RT 分流 + 独立 extent；RT 缓存 `ensure(..., aspect)`（深度档 usage/view aspect 走深度面） | 独立 extent 是新增能力：阴影图不能绑窗口尺寸 |
| ④ shadow pass 本体 | `decorateShadowPass`（图首插）+ `StateKit::shadowState` + `segmentRange("shadow")` 空段 + `diagnostics.shadow_pass` 门 | **段内零 draw**（FrameDrawList 尚无阴影段）→ 画面逐位不变，这是刻意的中间态 |
| ⑤ 深度-only 管线 | **未做** | 归后续 |

### 11.2 实施中撞到的三件事（都不在原设计里）

1. **`vk::Format::eD32Unorm` 根本不存在**。Vulkan 的深度格式是 `D16_UNORM` / `D32_SFLOAT` /
   `D24_UNORM_S8_UINT` / `D32_SFLOAT_S8_UINT`。写错时 clang 报的是
   `missing '#include <vulkan/vulkan_enums.hpp>'; 'eD16Unorm' must be declared before it is used`
   —— 报的是**另一个**存在的枚举，把人往"模块里枚举常量不可见"的方向带偏（我确实先被带偏了，
   去查了 rlstd 导出面）。**教训**：看到 "X must be declared before it is used" 且 note 指向
   一个你没写过的名字，先怀疑**自己写错了**，再怀疑模块可见性。
2. **零色附件的 pass 会让色态动态命令报 VVL**：`vkCmdSetColorBlendEnableEXT(): attachmentCount
   must be greater than 0`（`setColorWriteMaskEXT` 同）。修法 = `applyDynamicState` 在
   `colorAttachmentCount == 0` 时**整段跳过色态**（本来就无对象可作用）。既有 pass count ≥1
   逐位不变。
3. **`vk::Format` 枚举常量在模块接口分区不可用**（rlvk 的 GMF 头"可达但名字不可见"），
   所以深度格式词表**不能**做成接口的 `inline constexpr`（同 rlstd 必须导出"完整可用面"那类坑的
   Vulkan 版）——只能住在 impl，接口只暴露 `isKnownDepthRgFormat` + 复用 `resolveRgFormat`。

### 11.3 验证（基线见 §6 的新基线条目）

| 项 | 结果 |
|---|---|
| `just checks --strict` | 0 警告 / 100 个 .cppm |
| `shadow_pass=false` 截图 md5 | `7a87251793dba1254a9a863683b4dd9f` = **与新基线全等**（基建对默认路径零影响） |
| `shadow_pass=true` 45 s 冒烟 | 0 校验层报错、0 error 级日志；截图 md5 **与基线全等**（零 draw pass 对画面中性，符合设计） |
| selfCheck | 新增 10 个样本（5 拓扑 + 5 结构断言）全绿，钉住：depth-only pass 可编译、两表分立、独立 extent 只对图属 RT 开放、空 pass 仍拒、装饰产物 = 图首 pass + depthOffscreen 附件、编译产物 aspect/extent/discard/plan.depth/colors 全部符合预期 |


---

## 12.5 批 11.2 · 5a-2：阴影 pass 真正画出内容（depth-only 管线 + 投影体清单）

基建（§11）后的第一步实实上线：阴影 pass 从“零 draw”变成“真的在光空间里光梲化几何”。

### 12.5.1 落地内容

| 项 | 内容 |
|---|---|
| `shadow.mesh.slang` | depth-only mesh；**自带 8B push** `ShadowDrawArgs{objectIndex, cascadeIndex}`（不 import material.slang）；只输出 `position`（不带 5 个无人消费的 varying）；变换链 = `readObjectMatrix` → `readShadowViewProj`（两段列组合，不预先在 CPU 乘进 VP） |
| `shadow.frag.slang` | 空壳（无输出）——动态渲染里 depth-only pass 仍需一个 frag stage |
| `addShadowEntry(vertexSlot)` | 去重键 = **vertexSlot 单一变量**（阴影 mesh 只读顶点 buffer + sceneData，不消费纹理）→ bistro 1591 primitive 共用约 8 个拼接块槽 = **8 条目而非 1591** |
| `MeshAsset::shadowPipelineIndex` | 独立字段（不复用 `pipelineIndex`：两者绑的不是同一个纹理 buffer，去重键也不同） |
| `FrameDrawList::shadowCalls` | **独立向量**，不拼进 `calls`（后者的段边界被 GPU culling 的候选序对账强依赖） |
| `segmentRange` → `segmentSpan` | 改返回 `span`（两个段不共用索引空间）；阴影段名用**前缀匹配**（`"shadow"`），执行器拓扑校验同步改前缀匹配（两处分开写 = 拓扑允许未知 drawSource 静默落到 transparent 段 = 静默多画透明） |
| 投影体剔除 | 世界球 × **光空间拟合球**（不是主视罞！）；只收不透明材质；**不过过 GPU culling**（候选表按主视罞建立，用它会漏掉靠谱面屋顶的投影体） |
| scene data 子区 | 分界远距 / 各级 texel 世界尺寸 / 参数（depthBias, normalOffset, enabled）3 个 vec4，追在 VP 区之后（基址 721952/721968/721984，两侧字面量 static_assert 钉死） |

### 12.5.2 为何阴影 mesh 不能复用 `material.slang` 的 DrawArgs

DrawArgs 是 1591 条模型管线共享的 4B 推理契约（`material.slang` 单一真相源）。阴影需要多一个 `cascadeIndex`，而改那份共亮契约
= 整表重建。故阴影管线**自带 push 块**（仍然只装纯索引，符合 bindless 原则）。体积几何与牌展示
两类管线各自带推理块已有先例（`cull.comp` / `mip_gen.comp`）。

### 12.5.3 验证

| 项 | 结果 |
|---|---|
| `just checks --strict` | 0 警告 / 100 个 .cppm（doc-check 1006 声明） |
| `shadow_pass=false` 截图 md5 | `7a87251793dba1254a9a863683b4dd9f` = **与基线全等** |
| `shadow_pass=true` 截图 md5 | **与基线全等**（尚无采样消费者 → 阴影图内容不能影响成片，**这正是验证点**） |
| `shadow_pass=true` VVL | 0 校验层报错、0 error 级日志 |
| **GPU 实测（关门开 + profiler）** | 阴影 pass **1.627 ms**（p95 1.79）vs opaque 2.662 ms；`gpu.frame` 2.98 → **4.451 ms** |

> “阴影 pass 真的画了”的确证方式：**GPU 时间戳区间**。无采样时它只能证明“没报错”，
> 而 1.63 ms 的光梲化时间直接证明 2.8M 三角形确实渲染过了。

### 12.5.4 预算弹性（实测结论，非计划）

本机 Phase 9 判定的 GPU 侧余量约 **3.1 ms/帧**。单级阴影 pass 占 **1.63 ms** ——
剩余约 1.5 ms。这直接决定了后续设计的上限：

- **4 级全量绘制不可行**（4 × 1.63 = 6.5 ms，超预算 2 倍）。级联必须带**每级光空间剔除**，
  且需实测验证总量落在预算内；
- frag 侧 4 抽头 PCF 的开销还要从这 1.5 ms 里出（未实测，见 5a-3 记录）。

---

---

## 12. 批 11.2 · 5a-1 落地记录：scene data 阴影 VP 区 + 单级光空间矩阵

### 12.1 做了什么

| 项 | 内容 |
|---|---|
| scene data 尾区 | 追加 `kSceneShadowMatrixOffset = 721696`（= 辐照度区尾）、容量 `kSceneShadowCascadeCount = 4` 槽 × 64B。**沿用 Phase 8 ④a 的追加法 → 既有各区基址逐字节不动**（头 128 / 矩阵表 128 / 灯 262272 / MVP 262800 / draw 524944 / 辐照度 721552 全部不动） |
| 单一真相源 | `kShadowMapSize = 1024` 放在 **scene_data.cppm**（不是 render_graph）：两个消费方是"视锥拟合的 texel 吸附"与"图附件 extent"，后者由 `main_impl` 显式传参进 `decorateShadowPass(desc, mapSize)`，图模块因此不必 import scene data 契约 |
| CPU 每帧写 | `renderListSystem` 填 `sceneData.shadowViewProj[0]`，`Renderer::writeSceneStaging` memcpy 整区（4 槽一次写，[1..3] 留单位阵） |
| 矩阵构造 | `scene_context::frustumBoundingSphere`（Eberly 增量法，平面约定沿 `extractFrustum`）+ `directionalShadowMatrix(light, viewProj, mapSize)` |

### 12.2 拟合目标为什么不是场景包围球（实测决定）

初版按 `SceneBounds` 拟合，读运行期日志才发现问题：

```
gltf_loader_impl.cppm:583  bounds center=(14.367052, 8.558889, 2.984644) radius=116.26
```

**场景包围球半径 116.26**（整条运河 + 街景都在里面），而要投影阴影的微缩街景模型只是其中
一小块 → 单级 texel = 2·116.26/1024 = **0.227 世界单位**，整个模型只摊到 ~88 个 texel，
阴影必然糊成一团。改拟合**视锥包围球**后，texel 随相机拉近而变细（近景约 0.02~0.05
量级，差一个数量级）。

代价（写进注释，别当没看见）：单级拟合视锥 = **阴影图只覆盖可视区**，
落在视锥外的投影几何没有阴影；级联切分正是为了兼顾远景。

### 12.3 两个自己踩的坑（都是"看起来对、其实错一位"）

1. **texel 吸附的平移加错了空间**。偏移量是**光视空间**单位（ortho 把 [-r,r] 映到
   [-1,1]，故 NDC 差 × r），但我最初写成 `translate(delta) * (proj·view)` —— 那是
   **裁剪空间**单位，等于把位移放大了一个正交半边长。探针实测把视锥角点顶到
   NDC **1.43**（越界，阴影图边缘会丢几何）。改成 `proj·translate(delta)·view` 后
   max|s.xy| = 0.95，落回 [-1,1]。
   > 顺带一条判据：这种"只差一个矩阵因子顺序"的错误，**肉眼看阴影图只会觉得
   > "边缘有点怪"，不会报错**。所以这类几何必须用"点投影回 NDC 看是否在 [-1,1]³"来验，
   > 不要靠看图。

2. **验证脚本自己写错，比被验的代码错两次**。先用 numpy 复刻 GLM 的 `lookAt`+`ortho`
   自检，出现"覆盖失败"——排查后是我把 view 矩阵平移项的行序写反了；改完仍失败，
   再查发现 numpy 的 `ortho` 与 `lookAt` 深度约定不自洽（一个右、一个左）。
   > **教训**：验证数学时**直接链项目真实的 GLM 头**写个小探针（g++ + vcpkg include，
   > 30 行、秒级），比在 Python 里复刻一份"差不多的实现"可靠得多——复刻版会把
   > **自己的 bug** 误判成 **被测代码的 bug**，进而改坏本来正确的代码。

### 12.4 验证（真实 GLM 探针 `/tmp/opencode/glmprobe/probe2.cpp`）

| 判据 | 结果 |
|---|---|
| 视锥 8 角点全部落在 [-1,1]×[-1,1]×[0,1] | 近/中/远三档全绿（max\|s.xy\| = 0.949 / 0.925 / 0.953） |
| 爬行（吸附有效性） | 相机平移 0.0005（= 0.001 texel）→ 固定世界点在光空间漂移 **0.027 texel** ≪ 1 → 无整 texel 跳变 |
| 球面覆盖（球拟合版另测） | 球心 NDC 精确 (0,0,0.5)、球面 x/y ∈ [-1,1]、z ∈ [0.25,0.75]（5 用例含 up 退化/近水平光） |
| 项目侧 | `just checks --strict` 0 警告 / 100 cppm；默认路径 md5 = 基线；`shadow_pass=true` 0 VVL 0 error |

> 判据说明：爬行**不**用"矩阵逐位相同"衡量——球拟合的半边长 r 会随相机距离微变
> （那是"阴影呼吸"，幅度与 r 的相对变化同阶，肉眼不可见）；真正刺眼的是**整 texel 跳变**，
> 吸附就是为消掉它。量法 = 固定世界点在光空间的位移 / texel 尺寸。
