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

## 1. EVSSM 原理四步（对照 Filament 实测读，11.6a 已核完）

1. **矩写入**（`surface_depth_main.fs: computeDepthMomentsVSM`）：
   `z = exp(c·depth)` → 矩 = `(z, z², −1/z, 1/z²)` 写 RGBA16F；
   **解析方差** `+c²·z²·¼(dFdx²+dFdy²)`（用屏幕导数把纹素内深度斜率折进二阶矩，
   VSM 抗 acne 的正解，替代 bias）；`min(·, MAX_MOMENT)` 防溢出；centroid 防 MSAA。
2. **指数 c**（`computeDynamicVsmExponent`）：PCSS/EVSSM 路 = **固定 5.5**——
   fp16 上限 `exp(2c) ≤ 65504` 与安全下限双约束直给，无动态项。
3. **mip 生成**（`vsmMipmap.mat`）：**自定义 2×2 box filter texelFetch**——GPU
   硬件 bilinear 精度不够（矩对插值精度极敏感），矩的平均 = 合法 EVSM 粗 mip；
   clear 值 = **far 面深度的最大矩**（不是 0！Chebyshev 快路 `depth ≤ m1 → 全亮` 依赖）。
4. **O(1) blocker 搜索 + 半影 + 滤波**（`surface_shadowing.fs`）：搜索半径（世界）
   = 光源半径 × 深度 → `searchLod = log2(texels)` → **一次** textureLod 读矩 →
   指数解码 `zBlocker`；`pureGeometricRatio = (zRecv−zBlocker)×投影参数×ratioScale`
   过 **soft clamp**（`max+1−exp(−(r−max))`）；滤波 = `idealLod=log2(半影宽)`
   硬件 mip 当高斯核 + quincunx 5 抽头（1 抽头 −0.86 补偿 / 9 抽头 +旋转噪声）。
   Chebyshev 上界 + LBR + 动态方差地板（`ε·(w²)` 链式导出）在 `evaluateEVSM`。

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

## 3. 三只拦路虎（我们侧特有，Filament 作业抄不到；11.6a/b 已核实两处）

1. **图集布局硬伤 → 改动面已列**：现 = 2048² 单 2D 纹理 2×2 分块
   （`render_graph.cppm` kShadowMapName/Format/Size 契约组，格式已是字符串常量
   `"d32_sfloat"`）；mip 降采样会**跨级联块混合 = 级联互染**，必须转
   sampler2DArray（4×1024² 层 + 8 mip）。有利因素：**阴影段本来就是 4 个独立
   pass**（每级一个，tile viewport）→ array 化 = 每 pass 改 attach 第 c 层，
   拓扑不动。改动点：① RT 缓存 depthOffscreen 建法 → colorOffscreen
   RGBA16F+depth 附件对 + array + mipLevels=8；② 图装饰 shadow pass RT 装配
   （color+depth 双附件）；③ 执行器（vkrenderer）RT 装配；④ descriptor heap
   对 sampled 2D array image 的支持（**未核实，剩余最大项**）；⑤ shadow.slang
   `Texture2D→Texture2DArray` + `readShadowAtlas` 契约退役（EVSSM 变体内）。
   **IZB 已核不吃亏**：izb_create 深度源 = 场景深度图（D32，与 SSAO 共用
   depthSampleSlot），非 shadow map——格式切换零影响。**heap 支持已核**：
   descriptor heap 走 imageView 透传（env cubemap 走同条路进 heap 有先例），
   硬编码点 = RT 缓存 view 创建（`render_target_cache.cppm:161/178` e2D）+
   mip_gen e2D——改动为常规加 kind，非 heap 工程项目。
2. **深度 pass 改矩写入**：现 shadow.frag 空壳 depth-only → EVSSM frag 写
   `computeDepthMomentsVSM` 到 RGBA16F color 附件（depth 附件保留做 depth test/
   early-Z）；**clear 语义 = far 面最大矩**（非 0，Chebyshev 快路依赖）——RT 缓存
   clear 路径要支持 float 色值；mip 生成 = 每级一个 box-filter pass 进图
   （线性图可容纳，32 pass 上界，light pass 前）。
3. **RADV iGPU 带宽账（未测 = 11.6c）**：2048² RGBA16F = 32MB（现 D32 16MB）+
   mip 链 +33% + 每帧 8 级矩生成。本机 = Raphael 核显共享带宽，Phase 9
   CPU-bound 结论帮不上。探针 = 临时 pass 分配同规格 array + 每帧全链降采样，
   `frame_stats` 量帧增量；判据数值探针后拍板。

## 4. 批 11.6 spike 批次拆分（先小后大，全部门后）

- [x] 11.6a 作业细读补全（本文档 §1 公式表已入：矩写入/解析方差/c=5.5/
      box-filter mip/clear=最大矩/Chebyshev+LBR）。
- [x] 11.6b 图集 array 化改动面清单（本文档 §3.1 五点；IZB 深度源已核 =
      场景深度不受影响；**剩余未核 = descriptor heap 对 sampled 2D array 的支持**）。
- [x] 11.6c **带宽探针**（2026-10-02 实测收口，数据如下）。

### 11.6c 实测数据（RADV STRIX_HALO / 8060S，150 帧 compute-only，探针 = /tmp/opencode/evssm_probe.{slang,cpp} 一次性件不入库）

| 负载（4×1024²，链=8 mip box） | 吞吐 | 逐帧 fence |
|---|---|---|
| EVSSM 全链（矩写 33.5MB + 降采样链，计 ~89MB 名义流量） | **0.387 ms/帧** | 0.456 ms |
| 矩写 only | 0.056 ms | 0.097 ms |
| D32 等量写基线（R32F 16.8MB） | 0.049 ms | 0.089 ms |

**判据结论：带宽放行**——EVSSM 全链净增 ≈ +0.34 ms/帧，Phase 9 GPU 余量
3.1 ms 吃 ~11%。**诚实注脚**：① 名义 601 GB/s 超 LPDDR5X 物理规格 → 纯写被
缓存/压缩吸收，绝对 GB/s 不可作 DRAM 账，**只有相对比较可信**；② 本机是
8060S（Strix Halo）——与 AGENTS「本机 = 9950X Raphael」表述冲突，**机器身份
待用户确认**；Raphael（带宽低档）线性外推 ×2–3 ≈ 0.7–1.1 ms 仍在余量内，
跨机复测列入实施批（同 Phase 9 §11 遗留合并做）。
**新顾虑 = CPU 侧**：8 mip × 4 层 ≈ 28–32 额外 dispatch/pass，本项目
CPU-bound（record 7.03 ms）——实施批必量录制增量，必要时合批（单次 dispatch
循环多 mip 或用 subpass 式层级打包）。
- [ ] 11.6d 观感三版对比（同机位 llm_debug 截图）：经典 PCSS（解冻 11.5 门）/
      EVSSM 最小可用版 / 现行 PCF——多模态读图 + 半影宽度闭式测量
      （Δz×0.0093 锚，测量基建 = 11.5 计划中的 imgqa 判据，届时补建）。
- [ ] 11.6 转正裁决：EVSSM 进默认路径 or 保持门后 or 回退经典 PCSS。

### 4.1 实施形态定案（2026-10-02，11.6d 权威路线）

**C 方案 = 矩由 compute 从 D32 阴影图派生**，Filament 阵型（光栅直写矩 +
sampler2DArray + RT 缓存改造）推迟为后续优化：

- shadow pass **零改动**（depth-only D32 图集原样，正交深度本就线性）；
  矩 = 图外 compute（IZB 同类编舞，dispatch 基建/槽位/转换全部有先例）：
  ① moments_from_depth（读 D32 各 tile → 写 RGBA16F mip0，Filament 同款
  `z=exp(c·(2d−1))` 矩组 + far 像素=最大矩天然成立，无需特判 clear）
  ② 7 级 per-tile box 降采样（探针已证 tile 边界对齐、dispatch z=tile 即可，
  不跨块互染）；
- 矩图 = 2048² RGBA16F mipLevels=8 单纹理（非 array——per-tile 手动 mip 链，
  采样端 textureLod + 块内 0.5 texel inset）；
- 消费 = shadow.slang 新变体 `-DEVSSM_SOFT`（O(1) mip 搜索 + soft clamp +
  quincunx 5 抽头，Filament 公式直抄）；变体组合 = forward×3 + light×{base,izb}×EVSSM
  （rgl_shader_defined2 已备）；
- scene data 新 vec4 区 @722048（半坡 tan(θ/2) / 搜索半径 / 半影上限）；配置
  门 `shadow_evssm`（与冻结的 shadow_pcss 分立），数值键复用 pcss_*；
- 代价 = 比 Filament 融合式多一遍 D32 读 + 矩写（探针估 ~+0.1 ms），换掉的
  是 RT 缓存/graph/executor 三大件零改动 + 门关红线构造性保证不新增暴露面。

**机器注脚更新**：探针机 8060S STRIX_HALO 已由用户确认（三机轮换常态，
AGENTS 已定「机器不著书」纪律）；Raphael/NVIDIA 跨机复测仍列遗留。

## 5. 红线与纪律

- 门关逐位 = 老红线（批 11.5 已把「门关 = 选原版 SPV 变体」机制建成，
  EVSSM 变体走同一路；教训入册：**变体分支必须各自完整持有源文本，
  语义等价的汇合重构也破逐位**——`af5811a` commit message）。
- 每步 commit 绿树（just test/test-gpu/checks）；测试纪律 → skill
  rgl-test-discipline；截图/读图/回归 → skill rgl-verify。
- 阶段 2（IZB 性能）与本批解耦，不互为前置。

## 6. 已知遗留（未排期，登记勿丢）

- 11.5 冻结件未写测试（fallback 定位，转正时补：半影宽度闭式 + swim 扩展）。
- ~~源树 configs 测试态改动待处置~~ → 已转正（`c05b2bd`：variant=deferred、
  izb_shadow=true、pcss_* 四键默认关）。
