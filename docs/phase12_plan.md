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
| 12.3 | `ca18627` | **imgqa**（C++ + vcpkg）区域归因判据工具箱：selftest 26 条 / 判据五条全 advisory / 反模式登记 / 调用台账 / skill 钩子。首战：「anchor_cube_near +Z 黑面」**三配置两机位不复现**（±0.002 亮度）；纯颜色反解被实测推翻 → 顺序命名 + 闭式解色度弱校验（→ 调试历史 2026-09-30）。⚠️ 本行「不复现」已被 12.3b 勘误：当时测量面走 styles 缺表回落 model_pbr（→ 调试历史 2026-10-01） |
| 12.3b | `4e19d34`+`e8be52a` | 闭式解收单源（删 analytic_ref.py，probe 接替 + `--light/--intensity` 接线修复）+ **黑面 bug 定案修复**：Frisvad 切线奇点号写反 → 法线 +Z 面 NaN → 受光面纯黑（loader + bistro_pack 修复 + 单测 `test_frisvad_tangent`，→ 调试历史 2026-10-01）。switch 首战三项全绿（域外变化归因 = 物体间合法互投影，全名单域外恰 0）；texel canary 台账化（白 + 中性法线）。probe 变异 4/4（正对光 / 背光黑 / L⊥N 黑 / 默认 40°）逐字节 PASS |
| 12.3c/d-A…E | `fe846c5` `ea4f49c` `3bacf4a` `e9022f3` + d-E（沉淀回填提交） | **12.3d 调试历史全量裁决**（D10 清偿）：researcher×2 盘点 44 条目 → worker×4 串行落地（阴影/灯数据族接缝测试、五小缺口、snap 恒真断言重写、GLB 手构全链回归、invVP 数学契约、depthClip 双门落地、stale SPV 双护栏）→ 主 agent 逐批独立变异抽验（4 形态全红全还原）+ 36+8 条目沉淀地址回填。just test 116→143。→ §12.3d 执行记录 |

## 缺口 / 债务清单（按"何时必须还"排序）

| # | 债 | 性质 | 还的批次 |
|---|---|---|---|
| ~~D1~~ ✅ | ~~**闭式解双源**：`analytic_ref.py`（Python）与 `imgqa/analytic.cpp`（C++）是同一物理的两份转录，各自带锚点各带自检。将来一处改动另一处静默脱钩 = 两个"真相"互相矛盾时无人可仲裁~~ **12.3b 清偿**：analytic_ref.py 已删，`imgqa probe` 接替 `just analytic`，参考实现只剩 analytic.cpp 一份 | — | ~~12.3b~~ ✔ |
| D2（半）| **`switch` / `dead-branch-scan` 判据零实战**：~~从未真跑过~~ **12.3b 已实战 shadow_pass on/off 两轮**（determinism / impact-domain / dead-branch 三项全绿 + 域外归因，台账已回填）；② `ao.strength` 0↔0.5 轮 + AO 域 K 定标并入 12.3c | 库存（剩 AO 轮） | 12.3c |
| D3 | **字节级锚点 = 押本机 RADV**：imgqa selftest 锚点（(113,56,249) 等）来自本机 RADV 实测；NVIDIA/Intel 的 float32 着色链（FMA 收缩、取整）与 RADV 无位级契约 → 换机 selftest 必红，且会被误读成"转录坏了"。**用户核心场景就是三机测试，这是唯一结构性问题** | 结构性 | 12.4 |
| D4 | **机位撑不起逐面判据**：near_anchor 可见 9/9，但 far_cube 420 px / distant_marker 36 px——面级聚类需要单面 ~10⁴ px；`switch --allow-piece` 的影响域同样要求 piece 够大 | 判据精度 | 12.3c 前置 |
| D5 | **变异测试未做**：晋升四条件（→ imgqa.h 头注释）要求"真实 bug / 变异上变红过"，五条判据一条变异证据都没有 → 全卡 advisory。单测纪律"测试通过≠测试有效"对判据同样成立 | 判据有效性 | 12.3c |
| D6 | **qa_scenes.py 1251 行（含 GlbBuilder）待重写**：用户已拍板"确实需要重写"，方向 = 引擎/工具侧 C++ 直接产生顶点·index·颜色，glb 仅作容器保留（引擎 loader 吃 glb 不变） | 工具形态 | 12.5 |
| D7 | **qa/ 层定位悬空**：AGENTS.md 表里"进程级集成层……量化 harness 未落地"已过时——imgqa 就是那个 harness（RPC 驱动 + OpenCV 量化 + 台账）。该裁掉这行悬空定位而不是"再建一个" | 账目 | 12.6 |
| D8 | **Windows 侧 imgqa 未验证**：opencv4 在 Windows vcpkg 从未编译过（此前全项目无人链接）；首编时长可能不可接受 → 需要 cmake 开关或 vcpkg feature 兜底 | 跨机 | 12.4 |
| D9 | 调试历史 Phase 12 相关条目分散（12.2/12.3 结论散在 AGENTS.md / 提交信息 / imgqa.h 注释） | 卫生 | 12.6 |
| D10 | ~~调试历史未裁决~~ **12.3d 清偿**：36+8 条目全量裁决回填沉淀地址（四 worker 批落地 12 项 TEST/GUARD 缺口 + 1 条弱测试重写 + 2 处文档矛盾收口）；DEFER 项移交见 §12.3d 尾 | — | ✔ |

## 计划批次

### 12.3b — 闭式解收单源 + switch 首战（**已完成**，含计划外黑面定案）
- `imgqa probe`：接 analytic_ref.py 的对拍职责（复用 qa/configs/analytic/ 模板与
  RPC 截图通路，闭式解已在 analytic.cpp）；`just analytic` 改指 imgqa；
  **删 analytic_ref.py**（其 PNG 5-filter 往返自检已移植 imgqa selftest）。✔
- `switch` 真跑两轮：① `shadow_pass` on/off ✔（三项全绿；域外 3381px 归因 =
  near_post/near_cylinder/mid_cube 合法互投影，默认 allow 名单只含 ground；
  全名单下域外**恰 0**）；② `ao.strength` 0↔0.5 + AO 域 K 定标 → 并入 12.3c。
- **计划外（主线）**：styles 钉 model_pbr_ibl 后 probe 不绿 → 两天排查 → **黑面 bug
  定案 = Frisvad 切线奇点号写反**（n.z=+1 → NaN 切线 → TBN 污染 → 受光面纯黑；
  loader + bistro_pack 修复 + 新单测 test_frisvad_tangent；shader 端刻意不吞 NaN，
  → 调试历史 2026-10-01）。
  排查中另修：probe `--light/--intensity` 只改预测不改渲染的自欺接线；texel canary
  台账化（白 + 中性法线，「白活 ≠ 全活」实证）。
- **验收**：`just analytic` 绿（C++ 路）✔；`grep -rn analytic_ref` 代码零残留（仅存
  出处注释与历史记录）✔；`imgqa check` 台账增 4 条（probe×2 / switch 全名单+note）✔；
  `just test` 113/113 ✔；probe 变异 4/4（含背光黑两负例）✔。

### 12.3c — 机位补齐 + 变异测试（小-中）
- 前置（D4）：shadow_rig manifest cameras 增补——每个 piece 至少一个"占屏 ≥10⁴ px"
  的近机位（改数据不改几何，qa_scenes.py 只动相机表）。
- 变异清单（每次改坏→确认**点名判据**变红→立刻还原，逐条记录）：
  ① 改 per-object 光方向 → face-clusters 红；② 挪 piece 位置 → visibility /
  switch 域外非零红；③ 改一个顶点法线 → face-clusters 红；④ 把 AO 旁路从 lerp
  改回 multiply → switch-impact-domain 域外非零红。
- **验收**：四条变异全部被对应判据抓到（抓不到 = 判据无效，写进台账并降级注释）。

### 12.3d — 调试历史全量裁决（D10；拆多批，worker 执行 + 主 agent 质检）

**目标终态**：`调试历史.md` 只保留真正难测的教训，其余每条带「沉淀地址」；
需要跨机（NVIDIA/Intel 实测）的条目 DEFER 并 12.4，本机可测的先全部转完。

**规则（先行入册，本批起对新条目生效；→ AGENTS 写测试硬纪律 + 调试历史头部）**：
1. **沉淀地址**：每条历史收账时裁决三选一——① `TEST: <路径>`（断言已存在/新增）
   ② `GUARD: <机制>`（工具护栏，做不掉就先记债）③ `DOC: <理由>`（硬件观察/决策
   记录/负知识——断言写不出来的才留）。
2. **变异红线**：每条测试声明"哪个变异让我红"，写不出 = 无效测试；worker 交付的
   测试由主 agent **当场变异验红**后才入账（12.3b test_frisvad_tangent 双变异验红
   为范例）。
3. **断言独立于实现**：期望值来源 = 数学契约 / 闭式解 / 实机实测，不来自记忆也不
   抄被测码（12.3b 教训：主 agent 第一版修复公式被自己新写的正交性断言打红）。

**流程**：researcher 全量盘点（条目 → 现象/根因/不变量候选/现有覆盖/裁决建议）→
主 agent 按归属拆批（rlbase 可测 / 设备级 / 工具护栏 / 纯文档 / DEFER 跨机）→
每批派 worker 执行 → 主 agent 质检（变异验红 + `just test` 全绿 + 裁决诚实性复核，
"转不了"的理由要经得起反驳）。

**分批原则**：按被测对象归属切（同一库的测试共享构建），不按年代切；
护栏类（如 stale SPV mtime 检查、RPC 起跑探测）单独一批进 tools/。

**拆批（researcher 两轮盘点后定稿，worker 串行执行、主 agent 逐批质检）**：

盘点结论：44 条目中约半数不变量**已被现有测试钉住**（核对属实，只欠沉淀地址回填）；
DEFER（跨机）= **0 条**——DGC/NVIDIA 系列全是硬件观察或已闭环，非待验不变量。
待执行四批：

| 批 | 内容 | 关键难点 |
|---|---|---|
| **d-A** 阴影/灯数据族 + 能力门 | ① `fillShadowData` 契约（未生效级 splits=1e9 / 量化同源 / params.w）② `fillLightData` 按 index 落槽（乱序 entity→槽位、primary=0、kMaxLights 截断）③ depthClip **双条件门落地**（issue_history L25 声称 vs 代码单 bit 的矛盾以代码就范——`eds3 && base`，RADV 行为不变）+ features 真值表测试（含 presentTimingSupported/getDeviceExtensions） | ①② 需测试接缝（匿名 namespace → 接口导出）；接缝形态主 agent 先定 |
| **d-B** 小缺口 + 弱测试修复 | ① exposure 非默认值解析 ② dgc_static 默认门 ③ shadow pass loadOp=clear 断言 ④ depth-only 消费者反向用例（ao 不被 highlight 误判）⑤ expectedGeometrySlot 纯逻辑 ⑥ **snap"世界锚定"测试重写**——现存用例是恒等输入比对（注释声称换姿态、实参相同），必须改成姿态无关判据 + 变异"NDC 中心吸附"验红 | ⑥ 是"测试值得怀疑"的实锤案例，质检重点 |
| **d-C** glTF 越界读回归 + 数学契约 | ① gltf uv 步长 idx*2（cottage 越界 bug）——测试内手构最小 GLB（JSON+BIN chunk 字节串）走 loadGltfModel 全链，断言 uv 精确值 ② invVP 列叉积 forward 提取式（5a-8c）数学断言（随机 500 组，变异"第 2 列取反"必红） | ① GLB fixture 构造量 ~100 行；② 断言的是数学同一性，注释指回 shader 行 |
| **d-D** 工具护栏（GUARD 债） | ① slang import ⊆ rgl_shader() DEP 表构建期检查（stale SPV 根因护栏，E1④/E16④）② imgqa probe 起跑核 SPV 新鲜度（mtime 轻版）③ `.slang` 红线扫描（StructuredBuffer 顶点/索引禁令）评估——先读 skill rgl-new-module 红线清单，能否加规则由主 agent 裁 | ① 落 cmake/RglSlang.cmake 还是 tools/checks 需权衡（构建期 vs 检查期）|

**DEFER 并 12.3c**：AO 0↔0.5 switch 轮（E6b）、选级端到端跨级远景机位（E12②）。
**DEFER 并 12.6**：RPC loopback 测试（E2③④，"有真实痛点才补"边界内）。
**主 agent 终批 d-E**：全条目沉淀地址回填 + 调试历史收缩（被测试/护栏完全吸收的条目
删除，git history 即档案；终态 = 真正难测的教训）+ H1 矛盾裁决记录 + 变异抽验。

### 12.3d 执行记录（**已完成**，researcher×2 + worker×4 串行，主 agent 逐批质检）

**盘点结论**（researcher 两轮，覆盖核对全部实测）：44 条目中约半数不变量**早已被
现有测试/护栏钉住**（只欠沉淀地址回填）；完全无覆盖的不变量 7 项；DEFER（跨机）**0 条**。
两处"文档撒谎/测试摆设"实锤：① issue_history L25 双条件门声称 vs 代码单 bit；
② snap"世界锚定"用例 = 同参调两次的恒真断言。

**落地**（每批 worker 交付后主 agent 独立变异抽验，形态与 worker 所验不同）：
- **d-A** `fe846c5`：fillShadowData/fillLightData 接缝导出 + 10 用例（splits 哨兵/
  落槽/预乘）；depthClip 双条件门真实落地（矛盾①以代码就范收口）+ features 真值表。
- **d-B** `ea4f49c`：exposure/dgc_static/loadOp=clear/depth-only 反向/expectedGeometrySlot
  五缺口 + 矛盾②整条重写（契约 double 独立重算 + 同格两路 + 格相位恢复校验，
  信号/噪声比实测标注；主 agent 变异 round→floor 验红）。
- **d-C** `3bacf4a`：手构最小 GLB 全链回归（cottage 越界读）+ invVP 提取式数学同一性
  500 姿态（主 agent 跨流误读变异验红）；副产物：normalize(vec4) 后截断 ≠ 截断后
  normalize（差 1%，复刻注释）。
- **d-D** `e9022f3`：stale SPV 双护栏——`tools/check_shader_deps.py`（DEP ⊇ import
  传递闭包 + StructuredBuffer 全禁红线，构建期恒跑）+ imgqa `checkShadersFresh()`
  （逐 spv 比自身源闭包 mtime，清单由前者每次构建写出 = 图真相源单点）。
- **d-E**（`2ee3bcc` + `36a4896`）：36+8 条目沉淀地址全量回填；随后**全量删减
  1806→985 行**（-45%）——只删"已机制化事实"（每段删除前复核承载点实文），
  误诊叙事/负知识/方法论/症状指纹/勘误链全保留（约 260 行整段逐字保护）；
  悬空指针 7 项处置、行号指针清零；调试历史头部改写终态声明——
  **本文件只留教训不留事实**，"现在保证成立什么"的权威口径转移到 tests/ 与
  tools/checks/，这里回答"当初为什么错、错在哪一步被抓住"；
  issue_history 头部加裁决指针。
- **d-F**（`6da61e3`，用户追加）：AGENTS.md 瘦身 376→249——硬纪律十条迁新 skill
  `rgl-test-discipline`、运行要点迁 rgl-verify/技术概念（先补目的地再删源）、
  过时 4 处修正（alphaTest 已落地/prefiltered 已落地/九段/0-30 口径）、
  内部重复删除、文档索引补漏、阴影失效排查归档。

**遗留移交**：AO 0↔0.5 轮 + 选级端到端远景机位 → 12.3c；RPC loopback → 12.6。
**验收**：just test 143/143（本批 +27 用例）· checks 0 警告 · probe 逐字节 PASS ·
主 agent 独立变异 4 形态全红全还原。

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
