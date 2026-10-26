---
name: rgl-verify
description: 运行 RGL 程序做 headless 冒烟、llm_debug 截图多模态读图、BIT-IDENTICAL 截图回归比对、imgqa 区域归因判据。触发词：运行、冒烟、just smoke、just shot、截图、验证渲染、回归、headless、llm_debug、RGL_CONFIG_DIR、配置副本、认设备/哪台机器、dump_gbuf、图像判据、归属/可见性/影响域、闭式解预测、黑面/没画还是没着色。也适用于排查 "Log config file not found" 等启动问题。
---

# RGL 运行验证（冒烟 / 截图 / 回归）

## 运行环境

- **CWD 必须是 `build/rgl/`**：按相对路径读 `configs/` 与 `generated_shaders/*.spv`，
  其他目录启动报 "Log config file not found"
- **运行模式全配置化，字段清单去读 `configs/*.lua` 本体**（逐字段带注释，是唯一真相源）：
  `runtime.lua`（headless/尺寸/llm_shot）、`diagnostics.lua`（各类诊断门，全可选默认关）、
  `render_graph.lua`（**空表 = 内建 deferred 图；出现 `attachments`/`passes` 任一键 = 自定义图**；
  顶层 `variant` 键与 `RgGraphVariant` 枚举已删，写它 = 启动报错）、
  `camera.lua`（机位，含帧驱动 `frame_yaw`）、
  `scene.lua`（场景构成）。仅驱动级 env（`VK_DRIVER_FILES` / prime offload）由 `just run-*` 注入。
- **build 端 configs 副本 = `copy_if_different`（只源更新才覆盖）**——临时改副本调试**不会自动
  复原**，要复原得 `cp configs/*.lua build/rgl/configs/`。这就是"双源"，**一次性跑别碰副本**，
  用 `just smoke` / `just shot`（走 `RGL_CONFIG_DIR`，见下）

## agent 冒烟 / 截图：`just smoke` / `just shot`

```bash
just smoke                                # headless 出一帧，报 errors/warnings + 设备现场
just smoke --aa taa                       # 换抗锯齿档（off|taa|fsr）；--ssr / --sync 同族
just shot --name foo --size 1600x900      # 截图 → build/rgl/screenshots/foo.png（读它）
just smoke --from-build                   # 配置基准 = build 副本现场（默认 = 仓库 configs/）
just smoke --set 'diagnostics:picking=true'      # 任意配置键注入（可重复）
just smoke --set 'shadow:mode="pcss"'            # 字符串值**必须带引号**
```

`--set` 的两个坑（工具会**出声**，但先知道更省一轮）：① 文件前缀用**裸名**（`shadow` 不是
`shadow.lua`），白名单 = camera/diagnostics/render_graph/runtime/scene/shadow，不带前缀 = 打
runtime.lua；② 原值是字符串的键给裸值会被拒（写进 lua 成了未定义全局 = nil = 静默回落默认，
正是它拦下的形态）。**渲染路线没有开关**：`variant` 键与 `RgGraphVariant` 枚举已删
（`docs/archive/图变体收敛_plan.md` §4），要自定义图就在 `render_graph.lua` 里写 `attachments`/`passes`。

`tools/smoke.py`（stdlib-only）把一次性配置铺到临时目录并用 **`RGL_CONFIG_DIR`** 注入（引擎契约：
只覆盖配置目录，资产仍按 CWD 相对）——**不改任何配置副本，因此没有"跑完要复原"的状态**。
旧做法（手改 build 副本再 `cp` 回来）已废弃：忘复原是静默的。

需要工具表达不了的配置（相机机位、场景构成）：改**仓库** `configs/` 后 `just build` 再跑。

- **凡"截 UI"验收必须外部截图**：`llm_debug` 截图发生在 `frameEndHook`（imgui 绘制）**之前**
  → 天生不含 UI（既有事实，非回归）；imgui 面板视觉验收走窗口模式 + 外部截图工具
- 诊断直读 G-buffer 通道 = `diagnostics.lua` 的 `dump_gbuf`

## 截图回归方法学

BIT-IDENTICAL 比对须**同会话双构建**，已工具化 = **`just regress ab`**（`tools/regress.py`：
stash 基线 → 双构建 → 逐位比对 → **归因**；`finally` 恢复现场并核对 `git diff HEAD --binary`
与进场逐字节相同）。跨日 / 跨全量重建存在 ±1 LSB 叶缘噪声（增量 .o vs 全量 codegen 微差，
见 `git:调试历史` 2026-09-08）。其余子命令：`floor`（同构建地板，见下）· `burst`（连拍量
同相位周期）· `selftest`（量具自检）。三条口径写死在工具里，别在别处重新发明：零容差 ·
不存 golden · 不存基线哈希（`--expect` 只接受你从文档抄来的前缀）。

**位比红了先问"地板"再归因**：`just regress floor` 量同一构建连跑 N 次的 run-to-run 漂移，
`ab` 自动拿它给 FAIL 归因（"差异 ⊆ 既有地板" = 不是本批引入，但零容差仍判 FAIL）。

🔴 **反过来也成立：「锚复现」必须连跑两次，一次不算数**（2026-10-25 实测）。一次实验变异还原后
单次 `just shot` 读出的哈希 ≠ 台账锚，而 `grep` + 逐行读源码证明代码已彻底还原、
门开门关分支都是原版；连跑两次均复现锚。⟹ 那是一次孤立异常读数（疑与同条命令里紧接的
`just test` 重建 / 文件 IO 竞态有关，未定因）。
**代价不是"多跑一次 12 秒"，而是误归因**：若当时只跑一次就下结论，会判定"实验污染了代码"，
然后去排查一个不存在的 bug —— 而更坏的情形是反向的：某次真污染恰好读出了正确的锚，一次就放行。
出厂档 `aa = "off"` 是纯空间档（本应逐位稳定），所以这条**不是**给时域噪声开的豁免，
而是"任何单次读数都可能是竞态"的兜底。

**位比只对无时域历史的档成立**（实测划界，2026-10-09，`floor --frames 1200 --runs 3`）：
`aa=off` 与 `aa=fsr` **纯空间**档逐位稳定；带历史的两档（`aa=taa` / `--aa fsr --fsr-temporal`）
不逐位确定 ⟹ **别给带历史的档立位比闸门**（对时域抗锯齿做 bit-identical 无意义，走 imgqa
统计判据）。`burst` 的异相位对照仍要读：它量的是**周期**（统计判据的输入），不是位比。

**红线比对算子固定为「逐位相同」，不得换成 PSNR/SSIM**（用户拍板，此前一次提议被实测推翻）。
三条理由别重新发明：① 逐位是**零容差 A/B 算子**（门关 ⇒ 逐位不变），PSNR/SSIM 是**带阈值的
相似度**，而红线要防的正是"泄漏一点点"（改坏 3% 像素的改动 SSIM 0.999 稳过）；② 现机制**从不存
golden**，换过去 = 主动引入"要存参考图 + 一个会被讨价还价的阈值"；③ 阈值会被谈到通过为止。
两者均已登记反模式，原地可读 = `imgqa refuse`。**要一个能看的数**可以，但只准**报告不准判据**。
⟹ 因此**不做豁免清单**——"这几个像素可以不算"正是零容差红线最容易被谈掉的地方。工具只报
归因（"差异 ⊆ 既有地板"），判定仍红。

**单帧截图看不见时域不稳定**：静止场景比"改前 vs 改后"两张图**永远测不出逐帧抖动**——TAA 开就是
这样过了单帧比对，被用户肉眼叫停。凡被测项**本身是时域的**（时域滤波 / 任何跨帧累积 / 噪声类），
必须走 `just regress burst`（底层 `POST /shotburst`）连拍后测**相邻帧差**；且先立天然基线：
静止场景 + 无随机 pass 时逐帧差**必须恰为 0**（`aa=off` 实测最小周期 = 1 帧）。

## 测量纪律：什么能并行、什么不能（两版实验都做错后于 2026-10-07 定口径）

- **帧率 / present timing / stage 可得性 → 必须单实例串行**。争用直接扭曲被测对象本身。起跑前
  `pgrep -x rgl` 确认无残留（双检：日志终值行）。用户关窗 = 该次长跑作废，不得只取关窗前片段。
- **像素 / 统计类判据（headless + 帧驱动激励）→ 可以并行**。渲染路径**唯一的墙钟输入**是 legacy
  `spin.speed * dt`；帧驱动门（`spin_frame_angle` / `camera.frame_yaw`）把 dt 停用，角度是**帧号
  纯函数** ⟹ 争用只改墙钟不改帧序列。实测争用下 `aa-coverage` 逐字相同、`temporal` 抑制比不变。
  **红线**：并行只准用于同一次运行内自洽的统计量/比值；跨进程**逐位**比对不许并行（本来就不成立）；
  legacy dt-spin profile 必须被并行路径拒绝。全量论证与两轮实验设计错误 → `git:调试历史` 2026-10-07。
- **热身按帧数，不按墙钟**：`advanceFrames(n)`。按墙钟睡会在争用下量到 TAA 未收敛的**瞬态**，
  并把它误读成"渲染被扰动"（踩过两次）。
- **"并行"还有一层不是 CPU 争用、而是 `git` 状态**：`just regress ab` 为构建 B 侧跑
  `git stash -u`，**连未跟踪文件一起卷走**，直到 `finally` 才弹回。⟹ 同一检出里**任何**并发工作
  （包括"我只新建文件、绝不碰你的东西"这种自认安全的做法）都会在 regress 窗口里凭空消失；
  中途 `stash pop` / `checkout` 还会和它的现场恢复自检对撞。实测 2026-10-10 两次。
  **判据**：`git stash list` 出现 `regress-baseline-<时间戳>` = 有 regress 在飞。
  ⚠ **别用 `pgrep -f regress.py` 当这条判据**——它会匹配到发起检查的那条命令自身（实测假阳性），
  要看得看 stash 里有没有 `regress-baseline-*`。
  **正确做法**：并发期间要么等它退出（`pgrep -f regress.py`），要么把产物先抄到**仓库外**
  （`/tmp/opencode/…`）落地保存。切分支**不解决**——`git checkout -b` 把工作树改动一起带走，零隔离。

## 材质定位（加法闪色）

"哪个材质占这块屏幕"用**加法闪色**：材质页改 **emissive**（加法）再截图差分。**不要改 baseColor**
（乘法：纹理本身就暗的表面 ×红后变化 < 阈值 → 暗面被整体漏判，因此走错过三次）。比较亮度口径用
**同材质内的比值**（如阴影/受光），跨图跨时刻的全局百分位不可比。

## imgqa —— 区域归因判据（排查渲染问题的**默认起手**）

`tools/imgqa/`（C++ + vcpkg，`just imgqa <子命令>`）。它不是"两图算一个数"——**判据的区域来自几何**：
objectID 附件反解可见 piece，闭式解（GGX+ACES+sRGB 逐式转录，独立第二实现）给逐面预测，失败信息
直接点名"哪个 piece 的哪簇像素不对"而不是"SSIM 0.93"。

**何时抄它**（比目视/数值日志优先）：怀疑"画了没着色 vs 根本没画"、验证某 feature 的影响范围、
问"这块像素该是多少"、改着色链后的定位。**子命令与默认值单一真相源 = `imgqa -h` / `imgqa -h <子命令>`**
（`imgqa args <cmd>` 回显解析后的实际默认值）；常用：`selftest`（无引擎，毫秒级）/ `attribution` /
`faces` / `switch` / `probe` / `check` / `refuse` / `note "事后真相"`。

**三条硬约束（工具替你守，写新判据时你也要守）**：① 判据只在 objectID 反解出的**可见集**上求值；
② 统计一律线性域，比较域 = 观测域（预测字节先过完整显示链，ACES 有通道串扰没有比例关系）；
③ 每次真实调用 `note` 回填**事后真相**——调用台账 `qa/log/imgqa_calls.jsonl` 是判据可信度的一手数据。
**没有"晋升"这回事**，判据一律 advisory；凭据 = 注册表里的容差理由 + guard 出处（`selftest` 对
**每条**强制）。**读反模式必须连 `stillOpen` 一起读** → skill `rgl-test-discipline`。

**已知边界**：亮度聚类 gap=0.02 会把亮度相近的两面并成一簇，并簇对"每簇可解释"**假通过**——并簇时
人工看面预测表；id↔piece 用 primitiveIndex+1 顺序假设 + 闭式解色度**弱**校验（纯颜色最近邻不可靠：
ACES 饱和移位与 piece 间色度距离同量级，踩过）。

## 时域 / 噪声类判据的设计前置

**可复用到任何开关的硬判据 =「因果泄漏恒等式」**：开关关掉后，**附件/掩码说"这里没影响"的像素上，
两张交付图必须逐位相同**。它**不需要任何区域定义**，且顺带证明"该附件是交付差的唯一来源"
（交付 Δ 随附件偏离单调上升即旁证）。`shadow.lua` 的 `mode` / 任何 feature 门同型可用。

其余四条前置（"这个量在当前配置下到底会不会变"、"屏幕量 ≠ 世界量"、"区域不干净时主判据走中值残差
高频项"、"程序化靶是判据成立的前提"）→ `git:调试历史` 2026-10-04 条目。**先问会不会变**这条最
便宜也最常被跳过：AO 的 hash 只吃像素坐标、shader 无帧号输入 ⟹ TAA 关时逐帧**完全静止**，静态机位
连拍量不到任何时域信息，正确量具是**亚像素机位位移**。

## 阴影验收判据（前 8 轮修复里指标全绿而 bug 全程在场过两次）

**开关对拍 + 基线稳定 + D1 小，三者合起来仍会骗人**。除 skill `rgl-test-discipline` 的通用条
（阳性锚点 / 每条新逻辑要有阳性测试 / "0 差异"两种解释）外，阴影特有两条：

1. **机位对必须含"同 position 纯转向"**（只改 target）：平移与旋转是独立自由度，只验平移会让
   旋转分量整整漏一轮。
2. **机位对必须含"远离相机、跨越多级"的取景**：只贴着相机看会落进最小级联盒内，把"大面积走全亮
   旁路"的坏路径整个罩住。
3. **定位手段优先级：三色哨兵 > 数值日志 > 推导**——把 shader 的"出块/越界"返回改成哨兵值分色
   直出，**一步**分清"选错级"与"盒装不下"；把级号编进哨兵亮度就能直接读出选了哪一级。
4. **恒等式日志是指纹**：两个"本该不同"的量恒等 = 其中一个分支从未生效。数值日志不只看"变不变"，
   要看"哪些量死也不变"。
