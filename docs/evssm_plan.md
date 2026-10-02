# EVSSM 软阴影（批 11.6）——调研与实施计划

状态：**现行**（2026-10-02 立；批 11.6 spike 载体）。路线裁决见
`docs/hfts_izb_plan.md` §4.3「路线改道」：远场软影目标 = EVSSM，HFTS 降级
backlog，经典 PCSS（批 11.5，`af5811a` 冻结）= 零内存 fallback。

## 0. 动机（用户实感 → 调研 → 定案，全链路）

1. 街景路灯机位实看：IZB 硬影「全程非常清晰但不符合画质预期」——缺物理半影
   （太阳角直径 0.53° → 半影全宽 = Δz × 0.0093，3m 杆顶 ≈ 2.8cm）。
2. 首反应批 11.5 经典 PCSS（poisson blocker 搜索 + 变半径加权 PCF）全套建成，
   门关红线实证后**冻结**——Filament（本地 `/home/robin/program/filament`）
   现场核实给出更优答案。
3. Filament 事实链（本机核到，非二手）：
   - `Options.h:688` `ShadowType::PCSS` 注释 = "EVSM with soft shadows and
     contact hardening"——**经典 PCSS 已被官方重写替换**；
   - 重写提交 = 2026-07 `4dcdb4034c` "implements EVSSM (a.k.a. PCSS with EVSM)"；
   - `ShadowMapManager.cpp:73` `PCSS_MIP_LEVELS 8`、`:1402` 格式 =
     `elvsm ? RGBA16F : RG16F`、`:220` 正交光 `projectionParam = f − n`；
   - 作业面四件：`filament/src/materials/evsm/vsmMipmap.mat`（指数矩专用
     降采样——硬件 blit 会算错 EVSM 矩）、`shaders/src/surface_depth_main.fs`
     （深度 pass 写指数矩）、`shaders/src/surface_shadowing.fs`
     `ShadowSample_EVSSM`（O(1) mip blocker 搜索 + 半影估计 + LOD quincunx）、
     `ShadowMapManager.cpp`（动态 maxSearchRadius / 参数打包）。

## 1. EVSSM 原理三步（对照 surface_shadowing.fs 实测读）

1. **O(1) blocker 搜索**：搜索半径（世界）= 光源半径 × 接收面深度 → 换算 texel →
   `searchLod = log2(radius)` → **一次** `textureLod` 读 mip 矩 → 指数解码
   `zBlocker = (log(negMoment)/−c + 1)·0.5`。泊松搜索漏检细杆的问题不存在
   （mip 是硬件过滤的 min/矩）。
2. **半影估计**：`pureGeometricRatio = (zRecv − zBlocker) × 投影参数` ×
   penumbraRatioScale（美术乘子）；**soft clamp**：超过 maxPenumbraRatio 部分
   `max + 1 − exp(−(r−max))` 指数收束（不硬切，免等高模糊带）。
3. **滤波**：`idealLod = log2(penumbraWidthTexels)`（硬件 mip 当高斯核）+
   5 抽头 quincunx（或 1 抽头 −0.86 补偿 / 9 抽头 + 旋转噪声）——宽核免费。

## 2. 分轴质量对比（裁决依据，勿再重推）

| 轴 | EVSSM | HFTS(Division) | IZB(已建成) | CSM-PCF(现行) | 经典PCSS(冻结) |
|---|---|---|---|---|---|
| 半影平滑+接触硬化 | ✅ mip 免费宽核 | ✅ | ❌ 零半影 | ❌ 均匀糊 | ✅ |
| 软影 GPU 成本 | ✅ O(1)+5 抽头 | ❌ 贵 1.4–1.7×PCSS | 中 | 低 | 中（24 采样） |
| 细杆锐影 | ❌ **mip 稀释亚纹素遮挡体** | ⚠️ 中 | ✅ 实测独有 4705px | ❌ | ⚠️ 搜索漏检 |
| 光漏/精度 | ⚠️ EVSM 祖传（ELVSM+LBR+动态方差缓解） | ⚠️ | ✅ | ✅ | ✅ |
| 地形 height field 自影 | ❌ | ✅✅ | ❌ | ❌ | ❌ |

结论：EVSSM ≈ HFTS 的**只有软影三轴**；细杆轴 EVSSM 反而弱于两者 →
**IZB（近场细影）+ EVSSM（远场软影）**组合成立（= HFTS 混合本意的等价实现）。

## 3. 三只拦路虎（我们侧特有，Filament 作业抄不到，= spike 内容）

1. **图集布局硬伤**：我们 = 2048² 单 2D 纹理 2×2 分块（`shadowAtlasColumns`）；
   mip 降采样会**跨级联块混合 = 级联互染**。Filament 用 sampler2DArray
   （每级联一层）。改 texture array（4×1024² 层）牵动：RT 缓存
   （`render_target_cache`）的 eDepth 建法、图装饰 shadow pass 的 attachment、
   shadow.slang 的图集 uv 数学（`readShadowAtlas` 契约）、IZB priming 是否读
   阴影深度（待核：IZB 深度源 = G-buffer 场景深度，理论不受影响，须验证）。
2. **深度 pass 改矩写入**：现 shadow.frag 空壳 depth-only；EVSM 要 frag 写
   指数矩到 RGBA16F 色 attachment（深度 attachment 保留做 depth test/early-Z）。
   图装饰 + RT 格式 + 清值（矩清 0/清 e^?，对照 Filament clear 语义）。
3. **RADV iGPU 带宽账**：2048² RGBA16F = 32MB（现 D32 16MB）+ mip 链 +33% +
   **每帧 8 级矩生成**。本机 = 9950X Raphael 核显（共享内存带宽）。
   Phase 9 的 CPU-bound 结论帮不上——这是 GPU 带宽/写压力，spike 必须实测。

## 4. 批 11.6 spike 批次拆分（先小后大，全部门后）

- [ ] 11.6a 作业细读补全：vsmMipmap.mat 降采样核 + surface_depth_main 矩写入
      公式 + ShadowMapManager 的 clear/LBR/方差参数 → 本文档 §1 展开成公式表。
- [ ] 11.6b 图集 array 化改动面清单（一页纸，含 IZB priming 深度源核实）。
- [ ] 11.6c **带宽探针**：临时 pass 分配 4×1024² RGBA16F array + 每帧 8 级
      矩降采样，`frame_stats` 量帧增量（真机 RADV）。判据：增量 ≤ 阴影画质
      预算（对齐 §hfts_izb_plan 4.4 口径精神，软门数值 spike 后拍板）。
- [ ] 11.6d 观感三版对比（同机位 llm_debug 截图）：经典 PCSS（解冻 11.5 门）/
      EVSSM 最小可用版 / 现行 PCF——多模态读图 + 半影宽度闭式测量
      （Δz×0.0093 锚，测量基建 = 11.5 计划中的 imgqa 判据，届时补建）。
- [ ] 11.6 转正裁决：EVSSM 进默认路径 or 保持门后 or 回退经典 PCSS。

## 5. 红线与纪律

- 门关逐位 = 老红线（批 11.5 已把「门关 = 选原版 SPV 变体」机制建成，
  EVSSM 变体走同一路；教训入册：**变体分支必须各自完整持有源文本，
  语义等价的汇合重构也破逐位**——`af5811a` commit message）。
- 每步 commit 绿树（just test/test-gpu/checks）；测试纪律 → skill
  rgl-test-discipline；截图/读图/回归 → skill rgl-verify。
- 阶段 2（IZB 性能）与本批解耦，不互为前置。

## 6. 已知遗留（未排期，登记勿丢）

- 11.5 冻结件未写测试（fallback 定位，转正时补：半影宽度闭式 + swim 扩展）。
- 源树 configs 测试态改动（`izb_shadow=true`、`variant=deferred`）待用户处置。
