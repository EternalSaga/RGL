---
name: rgl-verify
description: 运行 RGL 程序做 headless 冒烟、llm_debug 截图多模态读图、BIT-IDENTICAL 截图回归比对、imgqa 区域归因判据。触发词：运行、冒烟、截图、验证渲染、回归、headless、llm_debug、dump_gbuf、图像判据、归属/可见性/影响域、闭式解预测、黑面/没画还是没着色。也适用于排查 "Log config file not found" 等启动问题。
---

# RGL 运行验证（冒烟 / 截图 / 回归）

## 运行环境

- **CWD 必须是 `build/rgl/`**：程序按相对路径读取 `configs/` 和 `generated_shaders/*.spv`，其他目录启动报 "Log config file not found"
- Mesa ≥26.2.3：RADV 无条件 advertise `VK_EXT_descriptor_heap`，**无需** `RADV_EXPERIMENTAL=heap`（`just run*` 也不再注入）
- 运行时资源由构建自动同步：`configs/` 增量拷贝（`copy_if_different`，改 config 后 `just build` 即生效；**新增** config 文件需 `just config`）；SPV 直接编译到 `build/rgl/generated_shaders/`
- **build 端 configs 副本 = `copy_if_different`（只源更新才覆盖）**——调试临时改 `build/rgl/configs/`（副本），调试完**手动** `cp configs/*.lua build/rgl/configs/` 复原（改副本不会触发覆盖，`just build` 不清理陈旧副本内容）；别改仓库源文件
- 启动期 DGC 选路可直接看 log：`DGC path: device=... -> ...`

## 运行模式全配置化（环境变量→配置文件，2026-09-08）

- `configs/runtime.lua`：headless / headless_size / win_size / show_after / llm_debug + llm_shot
- `configs/diagnostics.lua`：no_vvl / no_hdr / no_present / vtx_canary / texel_canary / dump_gbuf / style_dist / tex_dist / texture_force_*（全可选默认关）
- `configs/render_graph.lua` 顶层 `variant = "forward"|"deferred"|"offscreen"`（路线选路，替代旧 RGL_DEFERRED/RGL_OFFSCREEN env）
- 仅驱动级 env（`VK_DRIVER_FILES` / prime offload）保留——由 `just run-*` 注入，非应用配置

## headless 模式

runtime.lua `headless=true` 脱 SDL/compositor（`vkCreateHeadlessSurfaceEXT`），不建 SDL 窗口；尺寸 `headless_size={W,H}`（默认 1280×720）。
llm_debug 截图 = drawFrame 帧内回读 swapchain 后缓冲（present 前拷贝，真实上屏内容）；headless+llm 时 main 契约令 headless_size 跟随 llm_shot.size。
坑：v357 结构体无 width/height/format 字段、须显式 `VK_KHR_surface`、无 `eImmediate`（用 `eFifo`）。

## agent 冒烟

1. 改 `build/rgl/configs/runtime.lua` 为 `headless = true`（build 副本临时改；不自动复原，跑完手动 cp 回）
2. `cd build/rgl && timeout 8 ./rgl`
3. 正常应零 `[error]`；deferred 路线另改 build 副本 `render_graph.lua` 的 `variant="deferred"`

## llm_debug 截图（多模态读图）

1. build 副本 `configs/runtime.lua` 改 `headless=true` + `llm_debug=true`
2. 直接跑二进制 → 延迟 N 帧存 PNG（路径打日志）并退出 → 读图
3. `cp configs/*.lua build/rgl/configs/` 复原配置（改源后 `just build` 才会自动覆盖副本）

- 相机位置 = `camera.lua` 顶层 position/target/fovDegrees
- 截图尺寸 = swapchain extent（headless 跟随 llm_shot.size）
- 诊断直读 G-buffer 通道 = `diagnostics.lua` `dump_gbuf=0..3`
- **凡"截 UI"验收必须外部截图**：`llm_debug` 截图发生在 `frameEndHook`（imgui 绘制）**之前**
  → 天生不含 UI（既有事实，非回归）；imgui 面板视觉验收走窗口模式 + 外部截图工具
  （本机 KDE Wayland：`spectacle -b -n -o <file> -d <ms>`）

## 截图回归方法学（2026-09-08）

BIT-IDENTICAL 比对须**同会话双构建**（`git stash` 基线构建 vs 改动构建）；跨日/跨全量重建存在 ±1 LSB 叶缘噪声（增量 .o vs 全量 codegen 微差，见 `docs/调试历史.md` 2026-09-08 条目）。

**红线比对算子固定为「逐位相同」，不得换成 PSNR/SSIM**（2026-10-03 用户拍板，此前一次提议
被实测推翻）。三条理由，别重新发明：① 两者回答的不是同一个问题——逐位是**零容差 A/B 算子**
（门关 ⇒ 逐位不变），PSNR/SSIM 是**带阈值的相似度**，而红线要防的正是"泄漏一点点"（改坏 3%
像素的改动 SSIM 0.999 稳过）；② 现机制**从不存 golden**（就是上面那条同会话双构建），所以
"golden 存不过一天"这个 PSNR 的拒绝理由在现机制里根本不存在，换过去反而是主动引入"要存参考
图 + 一个会被讨价还价的阈值"；③ 阈值会被谈到通过为止。两者均已登记为反模式，理由原地可读 =
`tools/imgqa/judge.cpp` 的 `kAnti`（含 ssim 不可归因：说不出哪块错、错在亮度还是几何）。
**要一个能看的数**可以，但只准**报告不准判据**（同 `temporal` 里 L1 比值 1.38× 的处理）。

**单帧截图看不见时域不稳定**（2026-10-03 TAA 实测教训）：静止场景比"改前 vs 改后"两张图
**永远测不出逐帧抖动**——TAA 开就是这样过了单帧比对，被用户肉眼叫停（"地震"）。凡被测项
**本身是时域的**（TAA / 任何跨帧累积 / 噪声类），必须走 `POST /shotburst` 连拍后测**相邻帧差**；
且先立天然基线：静止场景 + 无随机 pass 时逐帧差**必须恰为 0**，非零即泄漏。

## 测量纪律（帧率 / present timing，2026-09-27 教训）

帧率/present timing 类测量**必须单实例串行**——起后台跑后，在下一次起跑前须确认前一进程已退出（`pgrep` + 日志终值行双检）。并行两实例会互相遮挡合成，把 stage 可得性与帧率双双扭曲（本项目曾据此得出"85% 白渲"的错误结论并已勘误，见 `docs/调试历史.md` "并行双实例污染测量"）。用户关窗 = 该次长跑作废，不得只取关窗前片段。

## 材质定位（加法闪色，2026-09-29 教训）

"哪个材质占这块屏幕"用**加法闪色**定位：材质页改 **emissive**（加法）再截图差分，**不要改 baseColor**（乘法：纹理本身就暗的表面 ×红后变化 < 阈值 → 暗面被整体漏判，2026-09-29 因此走错三次，见 `docs/调试历史.md`）。比较亮度口径用**同材质内的比值**（如阴影/受光），跨图跨时刻的全局百分位不可比。

## imgqa —— 区域归因的图像判据工具箱（Phase 12 批 12.3，排查渲染问题的**默认起手**）

`tools/imgqa/`（C++ + vcpkg，`just imgqa <子命令>`）。它不是"两图算一个数"——**判据的区域来自几何**：
objectID 附件反解可见 piece，闭式解（GGX+ACES+sRGB 逐式转录，独立第二实现）给逐面预测，
失败信息直接点名"哪个 piece 的哪簇像素不对"而不是"SSIM 0.93"。

**何时抄它**（比目视/数值日志优先）：怀疑"画了没着色 vs 根本没画"、验证某 feature 的影响范围、
问"这块像素该是多少"、改着色链后的定位。**子命令**：
- `selftest` —— 判据/闭式解转录/解码器自检（无引擎，毫秒级；闭式解锚点=实机截图实测字节）
- `attribution [--glb X.glb] [--camera 名]` —— 机位下每个 piece：像素数/线性亮度分位数/暗像素占比/色度；可见集=下游判据的求值域
- `faces --piece 名 [--shadow] [--env]` —— 六面闭式解预测（观测域字节）vs 观测亮度簇；解释不了的簇=发现
- `switch --allow-piece a,b` —— 开关影响域：域外**必须逐位为零** + 域内必须确实变了（两条对偶，堵"0 差异"的两种解释）
- `probe [--x 640] [--y 360] [--light Lx,Ly,Lz] [--tol 2]` —— 闭式解探针：某像素**该**是多少 vs 实机截图逐字节对拍（`just analytic` = 本命令 + `qa/configs/analytic/` 固定配置；12.3b 起接替已退役的 analytic_ref.py）
- `check [--promoted]` / `refuse [ssim|...]` / `note "事后真相"`

**三条硬约束（工具替你守，但写新判据时你也要守）**：① 判据只在 objectID 反解出的**可见集**上求值；
② 统计一律线性域，比较域=观测域（预测字节先过完整显示链，ACES 有通道串扰没有比例关系）；
③ 每次真实调用 `note` 回填真相——晋升凭据在 `qa/log/imgqa_calls.jsonl`，判据晋升四条：真实
bug/变异上变红过、跨 ≥20 次无关改动不误报、容差有理由、有反向用例。**SSIM/PSNR/全局 epsilon
是登记过的反模式**（`imgqa refuse` 会给理由），别重新发明。

**已知边界**：亮度聚类 gap=0.02 会把亮度相近的两面并成一簇，并簇对"每簇可解释"假通过——
并簇时人工看面预测表（如 −X=0.2376 vs +Z=0.2221）；id↔piece 用 primitiveIndex+1 顺序假设 +
闭式解色度弱校验（纯颜色最近邻不可靠：ACES 饱和移位与 piece 间色度距离同量级，实测踩过）。

## 阴影验收判据（2026-09-29 三轮踩坑固化，Phase 11 批 11.2）

前 8 轮阴影修复里指标全绿而 bug 全程在场过两次（5a-7 验的是退化数据、5a-8c 的选级 forward
整个是错的）。**开关对拍（on vs off 只变暗）+ 基线稳定 + D1 小，三者合起来仍会骗人**。以下为硬要求：

1. **阳性锚点**：先指定"某机位下某物体**必须**有影"，逐条断言。没有正向锚点的验收不算验收。
2. **机位对必须含"同 position 纯转向"**（只改 target）：平移与旋转是独立自由度，5a-8b 的
   三机位只验了平移，旋转分量整整漏了一轮。
3. **机位对必须含"远离相机、跨越多级"的取景**（如街道远景/拱门内远街）。只贴着相机看，
   会落进最小级联的盒内，把"大面积走全亮旁路"的坏路径整个罩住（5a-8c 就是这么骗过 5a-8 的）。
4. **定位手段优先级：三色哨兵 > 数值日志 > 推导**。把 `shadow.slang` 的"出块/越界"返回改成
   哨兵值、在 `model_pbr_ibl.frag.slang` 末尾分色直出（红=uv.x 越界 / 绿=uv.y / 蓝=ndc.z，
   块内受光=白、有影=黑），**一步**就能分清"选错级"与"盒装不下"；再把级号编进哨兵亮度
   就能直接读出片元选了哪一级。
5. **恒等式日志是指纹**：两个"本该不同"的量恒等（如 `eyeOffset ≡ r·1.02`）= 其中一个分支
   从未生效。数值日志不只看"变不变"，要看"哪些量死也不变"。
6. **每条新逻辑都要有阳性测试**：新分支在当前取景下差异为 0 时，临时把参数推到极端值
   （如淡出 k 改 1）确认管道真的通——"0 差异"既可能是"没影响"，也可能是"没接上"。
7. 注释里的"数学断言"（如"w=0 无平移项混入"）要有一条探针判据钉住；写完推导就用探针核对。
