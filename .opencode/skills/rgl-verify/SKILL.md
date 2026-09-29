---
name: rgl-verify
description: 运行 RGL 程序做 headless 冒烟、llm_debug 截图多模态读图、BIT-IDENTICAL 截图回归比对。触发词：运行、冒烟、截图、验证渲染、回归、headless、llm_debug、dump_gbuf。也适用于排查 "Log config file not found" 等启动问题。
---

# RGL 运行验证（冒烟 / 截图 / 回归）

## 运行环境

- **CWD 必须是 `build/rgl/`**：程序按相对路径读取 `configs/` 和 `generated_shaders/*.spv`，其他目录启动报 "Log config file not found"
- Mesa ≥26.2.3：RADV 无条件 advertise `VK_EXT_descriptor_heap`，**无需** `RADV_EXPERIMENTAL=heap`（`just run*` 也不再注入）
- 运行时资源由构建自动同步：`configs/` 增量拷贝（`copy_if_different`，改 config 后 `just build` 即生效；**新增** config 文件需 `just config`）；SPV 直接编译到 `build/rgl/generated_shaders/`
- **build 端 configs 副本会被下次 `just build` 按内容比对静默覆盖复原**——调试临时改 `build/rgl/configs/`（副本），调试完 `just build` 复原；别改仓库源文件
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

1. 改 `build/rgl/configs/runtime.lua` 为 `headless = true`（build 副本临时改，下次 `just build` 自动复原）
2. `cd build/rgl && timeout 8 ./rgl`
3. 正常应零 `[error]`；deferred 路线另改 build 副本 `render_graph.lua` 的 `variant="deferred"`

## llm_debug 截图（多模态读图）

1. build 副本 `configs/runtime.lua` 改 `headless=true` + `llm_debug=true`
2. 直接跑二进制 → 延迟 N 帧存 PNG（路径打日志）并退出 → 读图
3. `just build` 复原配置

- 相机位置 = `camera.lua` 顶层 position/target/fovDegrees
- 截图尺寸 = swapchain extent（headless 跟随 llm_shot.size）
- 诊断直读 G-buffer 通道 = `diagnostics.lua` `dump_gbuf=0..3`

## 截图回归方法学（2026-09-08）

BIT-IDENTICAL 比对须**同会话双构建**（`git stash` 基线构建 vs 改动构建）；跨日/跨全量重建存在 ±1 LSB 叶缘噪声（增量 .o vs 全量 codegen 微差，见 `docs/调试历史.md` 2026-09-08 条目）。

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
