-- configs/runtime.lua — 运行/窗口模式（2026-09-08 起环境变量全面转配置）
-- 全部字段可选，缺文件/缺段 = 回落默认（交互窗 800x600、不截图）。
-- 注：驱动级 env（VK_DRIVER_FILES / prime offload）不在此列——由 Justfile run-* 配方注入，
-- 非应用配置。Mesa ≥26.2.3 起 RADV 无需 RADV_EXPERIMENTAL。
return {
    -- headless：VK_EXT_headless_surface，脱 SDL/compositor（CI/agent 冒烟/截图）
    headless = false,
    headless_size = { 1280, 720 },

    -- 交互窗尺寸（llm 模式用 llm_shot.size、headless 用 headless_size，此值被覆盖）
    win_size = { 800, 600 },
    -- render_scale（Phase 14 批 14.3）：内部渲染分辨率倍率 = 渲染网格 / 窗口像素。
    --   1.0  = 默认，渲染网格与后缓冲逐像素相同（**交付路径逐位不变**）
    --   0.75 / 0.5 = 内部按 3/4、1/2 分辨率渲染，由 tonemap pass 采样 scene_hdr 时
    --                线性放大到窗口（该 pass 是全屏大三角 + linear sampler，归一化 uv
    --                采样 ⟹ 放大免费，没有额外的 pass 或 RT）
    -- 合法区间 [0.25, 1.0]（越界 = 钳制 + warn，非有限值 = 回落 1.0；上限不给 >1 是
    -- 因为超采样没有任何批次需要，留着只会多一条永不被测的路径）。
    -- **创建期量**：RT / 深度图按它分配，改它要走重启，热重载不生效。
    -- 与 imgui / 点选 / 鼠标**无关**（那些恒在窗口像素空间）。
    -- 与债-37 的 Wayland 分数缩放正交：那条管「逻辑点 → 窗口像素」，本键管「窗口像素 → 渲染像素」。
    -- 生效现场看启动日志 "render scale = ... → render WxH (swapchain WxH)"。
    render_scale = 1.0,
    -- velocity_field（Phase 14 批 14.4）：逐像素速度场（motion vectors）总开关。
    -- 关（默认）= 渲染图里**根本没有** velocity pass（装饰器不被调用），不领 RT、不建
    -- 管线条目 ⟹ 交付路径逐位不变（结构性，不是"有但旁路"）。开 = 每帧多一个全屏 pass
    -- （读 depth 写 RGBA16F）+ 一张渲染分辨率 RGBA16F RT；口径 = 渲染分辨率像素 · prev−cur · y 向下。
    -- **创建期量**：改它要走重启。目前尚无消费者（批 14.5 接 FSR upscaler 才用得上）。
    -- 生效现场看启动日志 "velocity (RGBA16F, 像素单位 prev−cur): WxH slot N"。
    velocity_field = false,
    -- Phase 14 批 14.5：FSR 3.1.4 upscaler 总开关（创建期量，热重载不生效）。
    -- 关（默认）= 零构造零命令 = 画面逐位不变。**依赖 velocity_field**：velocity 没进图
    -- 时本键自动失效（判据取实际图形态，不取本键）。
    fsr_upscaler = false,
    -- 批 14.5 第二批：FSR 的**时域累积**开关。仅在 fsr_upscaler 为真时有意义。
    -- 关 = 每帧 reset = 只用 EASU+RCAS 的**纯空间**上采样，输出是帧号纯函数（⟹ 可做逐位回归）。
    -- 开 = 让 vendor 累积历史。**必须与 fsr_jitter 一起开**——FSR3-upscaler 是**抖动相机**
    --   时域升频器，没有亚像素抖动的输入不在它的设计点上（§1.19 实测：0.5 档无抖动档读图
    --   是块状锯齿**最差**的一张，虽然它的 mean|∇| 最高）。
    --   判据（0.75 档，对「原生 1.0 不接 FSR」参照取 L1，小=好；两轴同时占优才算数）：
    --     时域+抖动 L1 **3.763** / 边缘 9.48  ← 最好
    --     纯空间     L1 4.234 / 边缘 8.49
    --     双线性     L1 4.335 / 边缘 8.68
    --   ⚠ 别拿 mean|∇| 单指标判好坏：`render_scale<1` 时它与质量**反相关**（块状伪影本身就是
    --   大梯度），全文见 phase14_plan §1.19。
    -- 🔴 **与 TAA 互斥**（不是偏好，是结构关系：两个时域滤波器串联 = 双重模糊）。同开时引擎
    --   会出 warn 并**强制本键失效**（`resolveFsrTemporal`），不需要你自己避。
    -- 曾长期默认关，理由是「实测在我们的输入口径下退化、根因未定位」——**该结论已作废**：
    --   根因 = 申报给 FSR 的是投影注入量而 FSR 要的是内容位移（**Y 反号**），已修，见 §1.19。
    fsr_temporal = false,

    -- 批 14.5 第四批：把亚像素 Halton 抖动接进投影矩阵并申报给 FSR（`jitterOffset`）。
    -- FSR3-upscaler 是**抖动相机**时域升频器：没有亚像素抖动的输入不在它的设计点上。
    -- 只在 fsr_upscaler 真的接起来时生效；**TAA 开时由 TAA 自己的抖动接管**（幅度走
    --   taa.jitter_scale、周期 16），本键的相位序列不参与决策。
    -- 周期取 vendor 内部的相位数（= 8·(上屏宽/渲染宽)²，2× 档为 32），幅度取满幅 ±0.5px
    --   ——不用 taa.jitter_scale，那是 TAA 的调参旋钮。
    -- 单独开它而不开 fsr_temporal 只会让输入图整体亚像素平移，要复现设计点 = 两条一起开。
    -- ⚠ **申报给 FSR 的不是这个量**，而是它经 `contentShiftPx` 换算后的**内容位移**
    --   （两者 **Y 轴反号**：本引擎的投影注入符号与官方伪码相反，再叠加负高视口）。
    --   地面真值实测的裁决与完整推导链 → `docs/FSR输入契约.md` §3。
    --   TAA 开时色输入是已在基空间的 `taa_out` ⟹ 申报自动归零（否则 FSR 再 un-jitter 一次）。
    -- 🔴 本条曾登记「接上抖动并没有救回时域档 ⟹ 设计点外假设已被否证」——**该结论作废**：
    --   它是在 Lanczos LUT 缺陷（§1.18，把一切压在 ~3.5 地板）与申报 Y 反号（§1.19）两个
    --   缺陷**都在场**时量出来的。两者修好后，时域+抖动是 `render_scale < 1` 各臂里最好的。
    fsr_jitter = false,

    -- DIAG：隐藏窗启动、第 N 帧后显示（可见窗 fault 排查；-1 = 立即显示）
    show_after = -1,

    -- present_mode：窗口交换链 present 模式（批 9.3 用户追加，headless 恒 eFifo 忽略此键）：
    -- "immediate" = 现状（硬要求 eImmediate，Wayland compositor 管帧同步）
    -- "fifo"      = eFifo（vsync 阻塞、帧率 = 刷新率；present_timing 双模式对比测量用）
    present_mode = "immediate",

    -- imgui 面板字体（Phase 10b 批 11b.1，仅 diagnostics.imgui=true 时生效）：
    -- ui_font = 字体文件绝对路径，空 = 自动探测链（MiSans Light → MiSans → Noto CJK
    -- → 文泉驿 → Windows 微软雅黑/黑体，链表见 imgui_layer_impl kUiFontCandidates）；
    -- 中文渲染需系统字体有对应字形 + imgui[freetype]（CFF 轮廓，如 Noto CJK）。
    ui_font = "",
    ui_font_size = 18.0,   -- 字号 px（imgui 内建 13 太挤；Light 字重发虚就调大或改用 MiSans-Regular）

    -- llm_debug 一次性截图（agent 自主工作：渲染 → 截图 → 退出）：
    -- 截图只改 llm_debug 开关（true/false）；file/size/delay_frames 常驻 llm_shot。
    -- 相机位置不在本文件——编辑 camera.lua 顶层 position/target/fovDegrees。
    -- ⚠ 改本文件 build 副本**不会**自动复原：copy_if_different 只源更新才覆盖，副本比源新
    -- 就永远不覆盖（2026-10-03 实测踩坑：某次冒烟删了 scene.lua 的 environment 块，之后
    -- 每次运行都没天穹，排查时先被当成代码回归）。冒烟完请立刻复原：
    --   cp configs/*.lua build/rgl/configs/     （或 touch configs/<改过的那个源文件>）
    -- 首选别走这条路：`just smoke` / `just shot` 用 RGL_CONFIG_DIR 跑一次性配置
    -- （tools/smoke.py），不碰本文件也不碰副本，因此没有需要复原的状态。
    llm_debug = false,
    llm_shot = {
        file = "screenshots/llm_debug.png",
        size = { 1280, 720 },
        delay_frames = 8,  -- 第 N 帧渲染完成后截图（0 = 首帧）
    },
}
