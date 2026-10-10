# CPU 性能路线（还债总账 §2.4 的读数与方案载体）

> **状态**：现行（2026-10-09 立）。量具 = **`just cpu`**（`tools/cpu_baseline.py`，stdlib-only）。
> 本文件是 **债-02 / 债-20 / 债-21 / 债-22 / 债-30** 的读数、口径与还账方案；
> `docs/还债总账.md` 只留一行结论 + 指向这里（台账只回答"还欠哪些、走到哪一步"）。
>
> **机器绑定**：本机 = AMD Radeon 8060S Graphics (RADV STRIX_HALO) / Mesa 26.2.3。三机轮换，
> 换机先 `just cpu` 复测再对比（AGENTS §运行要点「本机不著书」）。

---

## 1. 量具与口径（读数怎么来的）

```bash
just cpu                                  # 全部臂，串行，每臂约 25s（启动本身约 10s）
just cpu --arms default,no-vvl            # 改渲染后的最小验收两臂
just cpu --json f.json                    # 落读数
just cpu --compare f.json                 # 与上次读数逐项对差（改前存、改后比）
just cpu --list-arms                      # 臂表现读口——别在文档里抄臂表，抄一次漂一次
```

五条口径（写死在工具里，这里只记为什么）：

1. **单实例串行**：帧率就是被测对象，争用直接扭曲它（→ skill `rgl-verify` §测量纪律）。
2. **退出走 RPC `/quit`，不走 `llm_debug` 截图**：`FrameCapturer::collect`（fence 等待 + 全幅
   回读）在 `drawFrame` 区间**之内** ⟹ 用截图退出 = 窗口里必然混进一帧被回读污染的样本
   （实测把 `frame_ms` 的 p95 抬到 290ms）。
3. **headless 是测点不是臂内容器**：带窗跑会把合成器 present 节奏与 acquire 背压混进来
   （实测 `present` 0.03→0.67ms、`acquire` 0.17→0.50ms，且窗口尺寸变化会让可见集逐帧漂，
   `draw.count` 不再恒定）。工具起跑后核对日志里的 `render scale → render 1280x720`。
4. **主口径 = p50**：帧边界钩子（本工具轮询 `/state`）只污染墙钟的少数样本。
5. **`cpu 真干 = cpu.record − cull.fenceWait − 帧首 fenceWait`**（两处都是帧内主机阻塞等待）：
   只减 cull 那一处，会在"债-30 还掉、背压搬到帧首"之后把"还在等"读成"在干活"，并把 bound
   误判成 CPU-bound（§4.3 的实测就是这条的来由）。缺哪个测点就不减哪个，两个都没有则报 `—`
   ——**0 与"没有这个测点"是两件事**。

**臂表**（`--list-arms` 是权威，别在此抄全表）：除配置轴臂外，`cull-gpu-sync` /
`cull-gpu-lagged` 两臂是债-30 的三档对照，同时**是 cull fence 生命周期的唯一阳性探针**
（默认 `cpu` 档不提交 cull CB ⟹ 提交类错误只在两 GPU 臂才现形，§4.4 的教训）。

**未归因**（本工具唯一不显然的一步）：不是把 stats JSON 里所有 zone 加起来——`cull.fenceWait`
嵌在 `drawFrame.cull` 里，扁平求和会重复计数。正确口径 = **区间并集**：取每帧 `drawFrame`
区间，把其内所有 cpu 区间求并集，未归因 = `drawFrame − 并集`。实测 **p50 0.008~0.010ms**
（≈ 0.03%），债-01 的清偿仍然成立。

---

## 2. 2026-10-09 基线 · **债-30 还账前**（HEAD `e34afe0`，1280×720 headless / deferred / aa=off / 静止机位）

> 本表 = "cull 每帧同步等待"还在关键路径上时的读数，是 §3 三条结论的原始数据，也是
> 后续 `just cpu --compare` 的对照基线（改渲染/改代码前先 `--json` 存读数，改完比）。
> 债-30 还账后的读数在 §4.2。

| 臂 | frame p50 | cpu.record | gpu.frame | cull.fenceWait | **cpu 真干** ① | draws | vkcmd |
|---|---|---|---|---|---|---|---|
| `default`（VVL 开） | 28.17 | 28.44 | 10.78 | 10.52 | **17.91** | 6942 | 21036 |
| `no-vvl` | 13.29 | 13.14 | 10.51 | 10.78 | **2.36** | 6942 | 21036 |
| `shadow-off`（izb 同关） | 7.79 | 7.67 | 2.99 | 2.99 | 4.68 | 1574 | 4913 |
| `izb-off` | 25.10 | 24.93 | 8.99 | 8.47 | 16.47 | 6601 | 19994 |
| `scale-half`（render_scale=0.5） | 26.85 | 27.24 | 9.98 | 9.62 | 17.62 | 6942 | 21036 |

① **cpu 真干 = cpu.record − cull.fenceWait**。**这是本轮新增的口径，也是本轮最容易读错的地方**：
`cull.fenceWait` 是"CPU 等 GPU"，它随 GPU 时间一起动 ⟹ 任何配置轴的 CPU 增量若不减掉它，
就会把"GPU 变快导致 stall 变短"记成"CPU 变省的收益"（§3.3 的 IZB 就是这一条）。
本表用**当时**的口径（只减 cull 那一处）；债-30 还掉后背压搬到帧首，口径已扩成"两处等待都减"
（§1 第 5 条）。**本表不受影响**：旧数据帧首 `fenceWait` 只有 0.003~0.014ms，两种口径同值
（`no-vvl` 2.358 vs 2.355），差值全部来自 §4.3 那个真实增长。

**与 2026-10-04 台账读数对照**（`default` 臂，同机同档）：frame 28.59→28.17 · cpu.record
26.79→28.44 · gpu.frame 10.89→10.78 · cull.fenceWait 11.05→10.52 · draws 6942→6942 ·
vkcmd 21035→**21036**（+1 条，来自 2026-10-09 的 sync 修复批 `053e38a`）。
**结论：台账那组数没有被 Phase 14 的落地漂掉**（cpu.record 的 +1.6ms 与同臂 run-to-run 浮动
27.4~28.4 同量级，不是回归）。

---

## 3. 本轮实测产出的三条结论修正

### 3.1 「CPU-bound」是**验证层的形状**，不是交付的形状

`no-vvl` 臂是本轮最重要的一个对照：**VVL 占 `cpu.record` 的 54%**（28.44 → 13.14，
= 15.30ms = **0.73µs/条 vkCmd** = 2.20µs/条 draw）。把验证层摘掉之后：

- CPU 每帧真干 **2.36ms**，GPU 帧跨度 **10.51ms** ⟹ 交付路径是 **GPU-bound**，CPU 有 4 倍余量。
- 台账 §1 债-02 与 AGENTS「已知问题」里那句"CPU 侧无余量"**只在开着 VVL 的开发环境成立**。
  Phase 9 当年量它的时候没有这个对照，于是"CPU-bound"被当成了机器的性质，其实是**工具链的性质**。

**这不改变债-30 的优先级**（stall 与 VVL 无关，见 §3.2），但它**改变了"优化 CPU"这件事的意义**：
交付路径上省 1ms CPU 换来 0ms 帧时间，省 GPU 才换得到帧时间。

### 3.2 债-30 定价：交付 −16% / 开发 −41%，且**方案 C 逐位不变（实测）**

机制（本轮从"怀疑"升级为"实测确认"）：读回重放路要在录制 opaque 段**之前**拿到可见位，
而 cull CB 排在上一帧主 CB 之后（同队列按序）⟹ CPU 每帧被钉在"等上一帧 GPU 排空"上，
帧周期 = `cpu 真干 + gpu.frame` 而不是 `max(...)`。

**因果证据**（`scale-half` 臂是为此专门设的探针：像素 ÷4、draw 数一条不少）：

| 臂 | gpu.frame | cull.fenceWait | gpu − stall |
|---|---|---|---|
| default | 10.78 | 10.52 | 0.26 |
| no-vvl | 10.51 | 10.78 | −0.27 |
| scale-half | 9.98 | 9.62 | 0.36 |
| izb-off | 8.99 | 8.47 | 0.53 |
| shadow-off | 2.99 | 2.99 | 0.01 |

**stall 在 3.6× 的 GPU 时间跨度上始终 = gpu.frame ± 0.5ms** ⟹ 它不是"驱动提交开销"，
就是"等上一帧排空"。

**定价 spike**（一次性本地改动：`CullDriver::active()` 直接返回 false，= 方案 C 的形态；
量完即还原，还原后截图 sha 回到 `fdece9d0…`）：

| 臂 | 基线 frame | spike frame | Δ | spike 的 bound |
|---|---|---|---|---|
| `default`（VVL 开） | 28.17 | **16.72** | **−41%** | CPU-bound（cpu 真干 17.12 > gpu 10.81） |
| `no-vvl` | 13.29 | **11.18** | **−16%** | **GPU-bound**（cpu 真干 2.64 < gpu 10.46） |

**两条实测红线**（方案 C 的准入条件，都已过）：

1. **逐位不变**：spike 构建的 `just shot` 截图 sha256 = `fdece9d05e0c7509…` = 红线 A 同值。
2. **移动机位下两条剔除也一致**：`camera.frame_yaw = 0.2°/帧` 跑 **1290 帧**，日志里
   `reconcile MISMATCH` **0 条** ⟹ "CPU 集重放"不是"静止机位才巧"，而是两条剔除本来就同值
   （`checkCullAlignment` 保证 candidate 序 = opaque 段序 ⟹ 命令流逐条相同）。

> 本节是**方案 C 落地前的定价**。落地实测在 §4.2：交付 **−19.8%** / 开发 **−45.9%**
> （同一构建内 `cpu` 档 vs `gpu_sync` 档对照 = −44.4%），
> 两条都优于本处 spike 预测（spike 用 `active()=false` 连 cull 的 CPU 录制一起省掉，
> 落地版 `cpu` 档仍留着 CPU 剔除 + 候选表，所以差异不在方向上，而在**旧路径的串行还顺带
> 压低了 CPU 自身成本**——见 §4.3 的"背压搬家 + CPU 真干上涨"）。

### 3.3 债-20 换轴：4 级阴影是 **GPU 账**，不是 CPU 账

| 口径 | 4 级阴影 | 全部 | 占比 |
|---|---|---|---|
| CPU 录制（VVL 开，四条 shadow pass 的 zone 之和） | 11.08ms | 28.44ms | 39% |
| **CPU 录制（VVL 关，交付口径）** | **1.14ms** | 2.36ms | 49%（**绝对量只有 1.1ms**） |
| **GPU 执行** | **6.59ms** | 10.78ms | **61%** |

- **台账债-20 那句"19.06ms CPU（71%）"是配置轴差值口径，它同时含三样东西**：4 条 shadow pass
  的录制 11.08ms + 随阴影一起关掉的 IZB 1.44ms + **stall 因 GPU 变快而缩短的 7.5ms**
  （最后那 7.5ms 根本不是 CPU 收益，是"等得少了"）。而 11.08ms 里又有 **9.9ms 是验证层**
  ⟹ 交付口径下 4 级阴影的 CPU 只剩 **1.14ms**（仍是 CPU 录制里的最大单项，但省它换不到帧时间）。
- 它的真实身份是 **GPU 侧 61%**：shadow0 1.06 + shadow1 2.04 + shadow2 1.90 + shadow3 1.59。
  逐 pass 的 GPU 时间戳现在每臂都有（`just cpu` 直接打），**N3b 想要的"级联轴真数"里
  GPU 那一半已经不用改代码重编就能拿到**，只有"减级联后 split 边界变化"那一半仍要改常量。
- **IZB 复核**（`izb-off` 臂，用 §2 的 cpu 真干 口径）：CPU **1.44ms**（台账记 0.993ms，
  那是没减 stall 的原始 `cpu.record` 差）· GPU **1.78ms**（台账 1.576 ✓）· draws **−341** ✓。

---

## 4. 债-30 · **已还**（方案 C 门形，2026-10-09）

> **已落地**：方案 C 的门形，2026-10-09，形态见 §4.1、验收见 §4.2。方案 A（读回延后一帧）
> 与 C 同构，已作为 `gpu_lagged` 一档一起拿到 ⟹ 本节只保留**没选的方案 B** 的调研结论，
> 它的三条前提仍未验证（别重复调研一遍）。

| 方案 | 语义 | 为什么没选 |
|---|---|---|
| **B · cull 挪到闲置 compute 队列**（债-24 的队列） | **同帧语义完全不变** | 三条前提**全部未验证**：① RADV 上两队列真能并行；② cull CB 不能再写跨帧共享的 scene data heap（改读 per-slot staging，顺带消掉债-21 的双拷）；③ sequence/count/readback 的队列族所有权。前提①若不成立则整条作废，而**收益上限与已落地的两档相同** ⟹ 只有"同帧语义是硬要求"时才值得 ⟹ **等 DGC 线** |


### 4.1 落地形态（2026-10-09，方案 C 门形已实现）

三态键 **`diagnostics.cull_replay = "cpu" | "gpu_lagged" | "gpu_sync"`，出厂默认 `cpu`**
（未知值启动即 fail-fast，不静默回落——回落的失效形态是"你以为在测 GPU 剔除路，其实跑的是
CPU 路"，而两种读数看起来都合理，与 `present_mode` / `aa` 同一条纪律）。

| 面 | 落点 |
|---|---|
| 门本体 | `diagnostics.cppm` 的 `CullReplayMode` + `cullReplayName()`；解析/回显在 `diagnostics_impl.cppm`；键在 `configs/diagnostics.lua`（与同族 `dgc_static` 同文件 ⟹ 白拿 gate-lint + 回显覆盖率） |
| 帧编舞 | `CullDriver`：`gpuPassRuns()`（要不要录/提交 cull CB）与 `drivesDraws()`（opaque 段由谁驱动）两个判据分开；`runFrame` = 收上一帧 → 提交本帧 → 同帧档才本帧收割 |
| fence | **自持 `cullFences[slot]`**，不再借 `session.currentInFlightFence()`（消除"submit 后必须立刻 reset 才能给主 CB 复用"那条隐式契约；滞后档必须自持才表达得出） |
| 消费点 | `vkrenderer_passrec.cppm`：opaque 分支判 `drivesDraws()`，`drawFrame.cull` 段判 `gpuPassRuns()`（`cpu` 档连 zone 都不出现，不在 trace 里留 0 时间假段） |
| 滞后对账 | 比的是「GPU 的 F−1 结果 vs **提交那一帧**记下的 CPU 表」。拿本帧表比 = 相机一动必然失配 = 告警永久化 = 护栏失效 |
| lint | `kGateRules` 45 行 / 43 门（批 N6b 起含 shadow.lua 侧 13 门）：`cull_replay ≠ gpu_sync × dgc_static` → warn（DGC execute 路根本走不到） |

三档共用同一条异步管道（cull CB 录制+提交后不等 fence；下一帧收割 + 对账），差别只在
"opaque 段重放消费哪个集合"：

| 档 | 语义 | 代价 |
|---|---|---|
| **`cpu`（默认）** | 与还账前**逐位相同**，GPU 集整段不上 CB | RADV 路的 draw 集不再由 GPU 位驱动（GPU 剔除降级为验证器）。这是**学习/产品价值判断**，不是技术判断；NVIDIA 的 DGC execute 路本来就不读回，不受影响 |
| `gpu_lagged` | GPU 集照跑、滞后一帧对账，draw 仍由 CPU 驱动 | GPU 集自身的成本（dispatch + 读回拷贝 + 一次提交），不含 stall |
| `gpu_sync` | 今天的形态（同帧等 + 读回驱动 draw） | 债-30 那笔账本身；也是将来 DGC / 方案 B 的对照组 |

### 4.2 五条验收判据全过（本机 8060S / RADV STRIX_HALO / 1280×720 headless）

| 判据 | 结果 |
|---|---|
| ① `cull_replay=cpu` 截图 sha256 | `fdece9d0…` = 红线 A ✅ |
| ② `just regress ab`（A=工作树 / B=`e34afe0`） | 6 张全 `fdece9d0`，跨构建**最大 0 像素差异** ✅ |
| ③ syncval hazard | 三档各 **0** 条 ✅（修复前 `cpu`/`gpu_lagged` 各 8 条，见 §4.4） |
| ④ `just cpu --compare §2 基线` | 交付 **−19.8%**（13.29→10.66）/ 开发 **−45.9%**（28.17→15.25）✅ |
| ⑤ 阳性锚点 | 破坏 CPU 剔除的 left 面 → 移动机位 1493 帧 **28 条 MISMATCH**；还原 → 1456 帧 **0 条** ✅ |

**三档 + 缺键回落 逐位相同**（`cpu` / `gpu_sync` / `gpu_lagged` / 不写该键，四张截图 sha 全
= `fdece9d0…`，各 errs=0 warns=0）。这条比判据①强：它同时证明 `gpu_sync` 档在 fence 改自持
之后仍画同一张图——而那一档恰恰是本次唯一**行为路径被改**的档。

**性能四臂**（`just cpu --arms default,no-vvl,cull-gpu-sync,cull-gpu-lagged`，ms，p50）：

| 臂 | frame | cpu.rec | gpu.frame | 帧内主机等待 | cpu 真干 | draws | vkcmd | bound |
|---|---|---|---|---|---|---|---|---|
| `default`（VVL 开，cull=cpu） | **15.25** | 15.26 | 10.85 | 0.05 | 15.21 | 6942 | 21036 | CPU-bound |
| `no-vvl`（交付口径） | **10.66** | 10.43 | 10.30 | 6.73 | **3.70** | 6942 | 21036 | **GPU-bound** |
| `cull-gpu-sync`（VVL 开） | 27.45 | 27.60 | 10.82 | 11.32 | 16.28 | 6942 | 21036 | CPU-bound |
| `cull-gpu-lagged`（VVL 开） | 15.38 | 15.32 | 10.82 | 0.07 | 15.25 | 6942 | 21036 | CPU-bound |

- **`default` vs `cull-gpu-sync` = 15.25 vs 27.45（−44.4%）**：同一构建、只差一个键，
  这就是债-30 在开发环境的价格。交付口径 13.29 → **10.66（−19.8%）**。
- **draws / vkcmd 四臂完全相同**（6942 / 21036）⟹ 通用 `drawOne` 路与重放路发出的命令流
  条数一致，是"逐位相同"的机制级旁证。
- **滞后档的收割确实几乎不等**：`gpu_lagged` 的帧内等待 0.070ms（`gpu_sync` 是 11.32ms）。
  机制：帧 F−1 的 cull CB 排在帧 F−1 主 CB **之前**入队，而帧首 `beginFrame` 已等过帧 F−2
  的主 CB ⟹ 轮到此处时它通常早已信号。`gpu_lagged` 与 `default` 只差 0.13ms = GPU 集自身成本。
- **交付路径已贴 GPU 地板**：frame 10.66 vs gpu.frame 10.30，差 **0.36ms**。

**一次未归因的逐位漂移（如实记录）**：落地过程中 `gpu_sync` 臂出现过 **1 次** sha ≠ 红线
（`a2e8e144…`，errs=0 warns=0），随后 **16 次同档重跑 + 三档各 4 次共 12 次全部回到 `fdece9d0…`**
⟹ 约 1/16 的偶发，**不是本次改动引入的系统性差异**。两条排除证据：① 出现漂移的恰是**被串行化**
的那一档（`cpu` / `gpu_lagged` 两档帧真重叠，若是我补的 barrier 不够，该是它们更常漂）；
② `gpu_sync` 的"GPU 位驱动 draw"这条路径与还账前的默认路径逐字节相同，而红线 A 正是它在
`just regress` 下 6 张 0 像素差异立起来的。**它落在"同构建地板"该管的范围里**（→ skill
`rgl-verify` 的地板归因），再遇到先按地板归因走，别直接记成某次改动的回归。

### 4.3 一条反直觉读数：背压不消失，只搬家

| 口径 | 还账前 | 还账后 |
|---|---|---|
| cpu.record | 13.14 | 10.43 |
| 其中 cull 读回等待 | 10.78 | —（该档不提交 cull CB） |
| 其中帧首 `fenceWait` | **0.003** | **6.73** |
| cpu 真干 | 2.36 | **3.70** |

- 旧代码帧首那把 fence 只等 **0.003ms**——因为 cull 的无限超时等待已经把全部背压吃掉了。
  所以"新口径多减一项"解释不了任何差（旧数据按新口径算真干 = 2.355，与旧口径 2.358 同值）。
- 于是 **CPU 真干 2.36 → 3.70（+1.34ms）是真实增长**，不是口径产物。候选解释（驱动在
  CPU 领先两帧时的提交侧开销 / 与 GPU 争内存带宽）**本轮没拆开，记为待查**。
- 结论的写法因此必须是净额：债-30 的 10.5ms 里，**2.63ms 变成真收益**，6.73ms 变成
  GPU-bound 应用的**合法背压**（CPU 无事可做），1.34ms 被 CPU 自身成本上升吃掉。
- **量具口径同步修正**：`cpu 真干 = cpu.record − cull.fenceWait − 帧首 fenceWait`。只减前者
  会把"还在等"读成"在干活"，并把 bound 误判成 CPU-bound。

### 4.4 本轮被护栏抓到、且**默认档看不见**的两个真 bug

1. **cull fence 带 `eSignaled` 出厂** → `vkQueueSubmit(): fence submitted in SIGNALED state`
   （两 slot 各一条）。照抄 `FrameSession::inFlightFences` 的做法在这里不成立：本类首次提交前
   没有任何等待动作，而 FrameSession 每帧先 wait+reset 再提交，首帧的等待恰好把它清干净。
2. **IZB 的 head buffer 缺一条跨帧 barrier** → `WRITE_AFTER_WRITE hazard`（`cpu` /
   `gpu_lagged` 各 8 条，`gpu_sync` 0 条）。head 是**单份**跨 frames-in-flight 共享的 buffer，
   本帧 `vkCmdFillBuffer` 与上一帧对它的 compute 写 / mask pass 读 / dump 拷读之间没有声明过
   依赖。**此前"没事"是因为 cull 每帧同步等待把帧序列整个串行了**（= 债-30 那笔账本身）；
   串行一去掉 VVL 立刻报。修法 = head 清零前补一条 barrier（mask 清零处本来就有，head 是漏的）。

**教训（两条同因）**：`cpu` 档根本不提交 cull CB、也不让两帧真重叠 ⟹ **默认档看不见 fence
生命周期与跨帧依赖这两类错误**。所以 ① 非默认档必须单独跑一次冒烟 + syncval；② `just cpu`
已把 `cull-gpu-sync` / `cull-gpu-lagged` 做成正式臂（两 GPU 臂同时是 fence 生命周期的阳性探针，
臂内 `errors` 计数会抓到）。更一般的形式：**只要还掉一笔"每帧同步等待"的债，就要重新审计
所有单份跨帧共享资源的依赖声明**——本轮已知的其它候选（`nodesBuffer`、Evssm/FSR 的单份缓冲）
在 syncval 三档下均未报，暂不动。

### 4.5 明确不做

把 `framesInFlight` 加大。它不解除串行——串行点是 cull 自己那把无限超时 fence，
不是 swapchain 槽位不够（实测 `fenceWait`（帧首那把）在基线里只有 0.014ms）。

---

## 5. 还账方案 · 债-20（换轴之后还剩什么）

1. **先补 N3b 的 CPU 那一半已经不需要了**（§3.3：逐 pass GPU 时间戳每臂都有）。
   仍要改 `kActiveShadowCascades` 重编的只有一件事：**减级联后 split 边界变化**导致
   留下的级联覆盖更远、投影体更多 ⟹ 实际节省小于线性外推。**要用户放行改代码**。
2. **减级联是画质决策**（顶 2026-09-29 拍板），本轮不动。GPU 侧外推：4→3 省 ≈1.6~2.0ms
   （= shadow3 的实测 GPU 时间，口径限制同上）。
3. **登记但本轮不做**：静止机位下阴影段命令流逐帧相同 ⟹ 二级命令缓冲缓存（录制一次、
   每帧 1 条 `vkCmdExecuteCommands`）。**作用域**：只对**相机静止**有效，移动相机零收益；
   **仍然要做的部分**：不据此冻结任何阴影优化；**什么实验推翻它**：移动机位下重录成本
   高于直接重放（本机 `frame_yaw` 档一测即知）。<br>
   ⚠ **2026-10-10 补一条前提变更（别把上面那句读成长期性质）**："只对相机静止有效"是
   **今天这套"可见性编码在命令有无里"的架构**的性质，不是这个优化主意的性质。若走 MDI +
   **满槽孔洞流**（每桶 `drawCount`/`offset`/`stride` 恒定、被剔的只是那行 `groupCountX=0`），
   则**命令流与可见集解耦** ⟹ CB 缓存变成**相机无关**的优化，且它成立的前提
   （"0 计数空转且不跳 DrawIndex"）已在本机实测坐实（`tests/test_mdi_drawindex.cppm`）。
   账 → `docs/还债总账.md` §2.7（债-81 未覆盖的两半 / 债-82 阴影时序摊销）；
   `mdi_plan.md` §4 阶段 C。
4. **静态 DGC 供阴影段：本轮判为不值得**。**作用域** = 交付路径（VVL 关）+ 本机 + 阴影段
   CPU 只有 1.14ms 可省，而 opaque 路实测的 GPU 性能亏 1.85× 是同一笔要付的钱。
   **仍然要做的部分**：NVIDIA 路的 DGC execute 照旧。**什么实验推翻它**：在阴影段单独复测
   静态 DGC，若 GPU 亏损 < 20% 而 CPU 省下 1ms 以上（换到 GPU-bound 之后就没意义了，
   所以还要加一条"届时 CPU 已不是瓶颈"）。

---

## 6. 明确不做（CPU 侧，附实测理由）

| 项 | 理由 |
|---|---|
| 债-21 scene data 双拷 / 债-22 `reconcile` 无门控 / 债-23 shader object 条目 | 交付路径 CPU 真干合计 **2.36ms**，这三条加起来 **< 0.1ms**（`drawFrame.dataPath` 0.025 · `cull.readback` 0.040 · `drawFrame.staging` 0.014）。它们的**非 CPU 理由**仍在（债-23 的 752 句柄 / 启动 1.13s / 隐式契约群债-13），但"性能头寸"这个理由可以划掉 |
| 为"CPU-bound"再补 CPU 侧插桩 | 未归因 0.008ms（0.03%），债-01 已清偿，没有黑洞了 |
| 把 VVL 关掉当基线报 | 反过来：**任何 CPU 读数必须带 VVL 档**（§3.1 的教训就是没带它读出来的） |
