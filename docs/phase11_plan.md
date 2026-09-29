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
- [ ] 批 11.1：保守光栅化 RADV 实测通过
- [ ] 批 11.2：CSM 4 级 + PCSS；GPU ≤ 3 ms
- [ ] 批 11.3：HFTS spike 出结论（做 / 收手二选一）
- [ ] BIT-IDENTICAL 基线重建（新默认值已改视觉基线）
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
