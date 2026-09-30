# Phase 12 · 测试体系计划

> **状态**：现行（补立账本，2026-09-30）。本批特殊：12.1→12.3 是滚动长出来的，
> 无事前计划——动机是 ~90 条历史 bug 里大量"验证骗人"形态（阳性锚点缺失、断言写在
> 退化数据上、开关对拍双假绿）。本文件 = **还欠什么 + 下一步做什么** 的总账；
> 现状机制描述权威仍是 `AGENTS.md §测试体系`，本文只引用不重复。
> **用法**：每批收口划掉一条 + 新债追加缺口清单。"下一步干什么"不知道时就来看这份。

## 批次史（已收口）

| 批 | commit | 内容 |
|---|---|---|
| 12.1 | `6a05146` | tests/ 单元测试层（doctest，无 GPU 毫秒级）+ rlcore 库 + 阴影拟合球精度修复 |
| 12.2a | `79fc778` | objectID 全分辨率 dump（图像断言的锚点源） |
| 12.2b | `584bef8` | 程序化 QA 场景生成器 `tools/qa_scenes.py`（shadow_rig / cornell_box） |
| 12.2c | `058f2f9` | 引擎四库分层 rlbase/rlgfx/rlscene/rlframe（判据 = 需不需要 VkDevice） |
| 12.2d-a | `d4be15d` | 分层闭合规则机器可验（rules_layers.py，逆向 import 报 error） |
| 12.2d-b | `2d8abe0` | tests 链接面扩到全四层 + 66→118 用例 |
| 12.2d-c | `f95d5f9` | 闭式解探针 `analytic_ref.py`（GGX+ACES+sRGB 逐式转录 + qa/configs/analytic/） |
| 12.2d-d | `0c35457` | 入口（test/test-all/test-gpu/analytic）+ 文档收口 |
| 12.3 | `ca18627` | **imgqa**（C++ + vcpkg）区域归因判据工具箱：selftest 26 条 / 判据五条全 advisory / 反模式登记 / 调用台账 / skill 钩子。首战：「anchor_cube_near +Z 黑面」**三配置两机位不复现**（±0.002 亮度）；纯颜色反解被实测推翻 → 顺序命名 + 闭式解色度弱校验（→ 调试历史 2026-09-30） |

## 缺口 / 债务清单（按"何时必须还"排序）

| # | 债 | 性质 | 还的批次 |
|---|---|---|---|
| D1 | **闭式解双源**：`analytic_ref.py`（Python）与 `imgqa/analytic.cpp`（C++）是同一物理的两份转录，各自带锚点各带自检。将来一处改动另一处静默脱钩 = 两个"真相"互相矛盾时无人可仲裁。12.3 已定死删除条件 = `imgqa probe` 接替 `just analytic` | 自己刚欠的，越新越便宜 | 12.3b |
| D2 | **`switch` / `dead-branch-scan` 判据零实战**：命令实现了但从没真跑过；台账仅 2 条。没实战的判据是库存不是判据 | 库存 | 12.3b |
| D3 | **字节级锚点 = 押本机 RADV**：imgqa selftest 锚点（(113,56,249) 等）来自本机 RADV 实测；NVIDIA/Intel 的 float32 着色链（FMA 收缩、取整）与 RADV 无位级契约 → 换机 selftest 必红，且会被误读成"转录坏了"。**用户核心场景就是三机测试，这是唯一结构性问题** | 结构性 | 12.4 |
| D4 | **机位撑不起逐面判据**：near_anchor 可见 9/9，但 far_cube 420 px / distant_marker 36 px——面级聚类需要单面 ~10⁴ px；`switch --allow-piece` 的影响域同样要求 piece 够大 | 判据精度 | 12.3c 前置 |
| D5 | **变异测试未做**：晋升四条件（→ imgqa.h 头注释）要求"真实 bug / 变异上变红过"，五条判据一条变异证据都没有 → 全卡 advisory。单测纪律"测试通过≠测试有效"对判据同样成立 | 判据有效性 | 12.3c |
| D6 | **qa_scenes.py 1251 行（含 GlbBuilder）待重写**：用户已拍板"确实需要重写"，方向 = 引擎/工具侧 C++ 直接产生顶点·index·颜色，glb 仅作容器保留（引擎 loader 吃 glb 不变） | 工具形态 | 12.5 |
| D7 | **qa/ 层定位悬空**：AGENTS.md 表里"进程级集成层……量化 harness 未落地"已过时——imgqa 就是那个 harness（RPC 驱动 + OpenCV 量化 + 台账）。该裁掉这行悬空定位而不是"再建一个" | 账目 | 12.6 |
| D8 | **Windows 侧 imgqa 未验证**：opencv4 在 Windows vcpkg 从未编译过（此前全项目无人链接）；首编时长可能不可接受 → 需要 cmake 开关或 vcpkg feature 兜底 | 跨机 | 12.4 |
| D9 | 调试历史 Phase 12 相关条目分散（12.2/12.3 结论散在 AGENTS.md / 提交信息 / imgqa.h 注释） | 卫生 | 12.6 |

## 计划批次

### 12.3b — 闭式解收单源 + switch 首战（小，一个会话）
- `imgqa probe`：接 analytic_ref.py 的对拍职责（复用 qa/configs/analytic/ 模板与
  RPC 截图通路，闭式解已在 analytic.cpp）；`just analytic` 改指 imgqa；
  **删 analytic_ref.py**（其 PNG 5-filter 往返自检已移植 imgqa selftest）。
- `switch` 真跑两轮：① `shadow_pass` on/off；② `ao.strength` 0↔0.5——正向锚点现成
  （lerp(1,x,0)≡1.0 ⟹ strength=0 逐位旁路，域外必须恰为 0）。台账落 2 条。
- **验收**：`just analytic` 绿（C++ 路）；`grep -r analytic_ref` 全仓 0 残留；
  `imgqa check` 台账 ≥4 条；`just test` 113/113。

### 12.3c — 机位补齐 + 变异测试（小-中）
- 前置（D4）：shadow_rig manifest cameras 增补——每个 piece 至少一个"占屏 ≥10⁴ px"
  的近机位（改数据不改几何，qa_scenes.py 只动相机表）。
- 变异清单（每次改坏→确认**点名判据**变红→立刻还原，逐条记录）：
  ① 改 per-object 光方向 → face-clusters 红；② 挪 piece 位置 → visibility /
  switch 域外非零红；③ 改一个顶点法线 → face-clusters 红；④ 把 AO 旁路从 lerp
  改回 multiply → switch-impact-domain 域外非零红。
- **验收**：四条变异全部被对应判据抓到（抓不到 = 判据无效，写进台账并降级注释）。

### 12.4 — 三机跨机（中，需要另外两台的人手）
- D3 方案：字节锚点 → **每机登记表**（`imgqa selftest --record-anchor` 生成该机
  anchors 文件 + 闭式解 vs 实测容差 ±2 LSB）；无登记表的新机器降级为容差模式
  （闭式解 vs 实测 ≤2 LSB 即绿），**不是**把本机字节硬编码搬来判红。
  容差理由要写：跨驱动 float32 FMA 收缩序无位级契约（AGENTS.md 已知问题节同款论据）。
- D8：imgqa 加 CMake 开关（如 `RGL_BUILD_IMGQA`，Windows 默认关到有首编实测）。
- **验收**：NVIDIA 机与 Intel 机上 attribution + faces 绿（容差模式）；
  Windows 至少 configure+imgqa 编译通过。

### 12.5 — 程序化几何重写（中；先拍板再动手，D6）
- 候选：(a) `imgqa scene` C++ 子命令产 顶点/index/color 并序列化 glb（引擎 loader
  不动，glb 降级为纯容器格式）；(b) qa_scenes.py 保 stdlib-only 重写瘦身。
- 回归锚点 = 现有两场景的 attribution 输出表逐字节对拍（重写前后一致才允许删旧）。

### 12.6 — 收尾盘点（小）
- qa/ 悬空行按 D7 裁定改写进 AGENTS.md 表；D9 调试历史归拢。
- 单测缺口只补"有真实痛点"的候选（RPC 传输解析、param_registry），不刷覆盖率指标
  ——113 条用例的价值在阳性锚点，不在数量。

## 显式非目标

- GPU CI / 每 commit 自动跨机（三机 = 人工节奏，12.4 只做"人工跑一次是绿的"）。
- SSIM/PSNR/FLIP 类全局指标——`imgqa refuse` 登记过拒绝理由，别重新发明。
- 判据晋升的"≥20 次无关改动不误报"不造数据凑数，台账自然积累。
