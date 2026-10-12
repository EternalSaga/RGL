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
    -- 为什么保持**独立键**（批 14.6 裁决）：它有非 FSR 消费者——批 14.4 的 TAA 逐像素
    -- 重投影线要它，与「谁做抗锯齿」无关。但 aa 为 fsr 时它被**隐含打开**（FSR 的
    -- motionVectors 输入就是那张图）——隐含只发生在解析期一处，本键因此仍是单一真相源。
    -- **创建期量**：改它要走重启。
    -- 生效现场看启动日志 "velocity (RGBA16F, 像素单位 prev−cur): WxH slot N"。
    velocity_field = false,

    -- ── 批 14.6：抗锯齿三态开关——「谁做抗锯齿」是**一个槽位**，不是两个布尔 ──
    --   "off" = 出厂默认：无 TAA 无 FSR（**红线 A 那张图** `fdece9d0…`，逐位不变回归基准）
    --   "fsr" = FSR3-upscaler 做抗锯齿（vendor context + 图外 compute + 四张私有图；
    --           隐含打开 velocity_field；它自己的调参在下面 `fsr` 块里）
    --   "taa" = 自研 TAA 做抗锯齿（图形态开关；原 scene.lua 的 taa.enabled 键已删，
    --           调参留在 scene.lua 的 `taa` 块：clip_mode / feedback / jitter_scale / …）
    -- 缺键 = "off"。未知值 = **启动 fail-fast**（present_mode 的既有纪律，不静默回落：
    -- 回落的失效形态 = 你以为开着抗锯齿在量图、其实跑在关档，而画面只是"糊了一点"）。
    -- 为什么是三态互斥而不是两个布尔（拍板记录）：两个布尔的失效形态 = 「两个都开然后
    -- 引擎静默关掉一个」（旧 resolveFsrTemporal 强制 FSR 时域关，配置里却还写着 true）
    -- ——申报值与生效值分家，看配置会以为开着；三态让这种写法**结构上不存在**。
    -- 生效现场看启动日志 "fsr temporal: …" / "taa (RGBA16F): …" / "FsrUpscaler: context 就绪"。
    aa = "off",

    -- FSR 自己的调参（批 14.6 收块：原扁平的 fsr_upscaler / fsr_temporal / fsr_jitter 三键
    -- **已删且无双读兼容**——两个真相源是本项目反复登记的反模式；upscaler 总开关的语义
    -- 由上面 aa 的槽位承担）。仅 aa 为 fsr 时被消费；默认 true/true = FSR 的**设计点**：
    --   temporal  true = 让 vendor 累积历史；false = 每帧 reset = 纯空间 EASU+RCAS，
    --            输出是帧号纯函数（⟹ 可做逐位回归）。纯空间是**消融臂**，要显式写 false。
    --   jitter    true = 把亚像素 Halton 抖动接进投影并申报给 FSR。FSR3-upscaler 是
    --            **抖动相机**时域升频器，没有亚像素抖动的输入不在它的设计点上；周期取
    --            vendor 内部相位数 = 8·(上屏宽/渲染宽)²（0.75 档实测 14），幅度满幅 ±0.5px
    --            ——不走 taa.jitter_scale，那是 TAA 的调参旋钮（两个独立旋钮）。
    -- ⚠ 申报给 FSR 的不是抖动注入量，而是经 `contentShiftPx` 换算的**内容位移**
    --   （两者 **Y 轴反号**：本引擎投影注入符号与官方伪码相反，再叠加负高视口）；
    --   地面真值实测的裁决与完整推导链 → `docs/FSR输入契约.md` §3。
    -- 历史教训（防反复）：temporal 曾长期默认关，理由「实测在我们的输入口径下退化」——
    --   **该结论已作废**（根因 = Lanczos LUT 缺陷 §1.18 + 申报 Y 反号 §1.19，都已修）。
    --   修好后 0.75 档两轴判据（对原生 1.0 参照取 L1 / 边缘，小=好/大=好）：
    --     时域+抖动 L1 **3.763** / 边缘 9.48 ← 最好；纯空间 4.234 / 8.49；双线性 4.335 / 8.68。
    --   ⚠ 别拿 mean|∇| 单指标判好坏：`render_scale < 1` 时它与质量**反相关**（块状伪影
    --   本身就是大梯度），全文 → phase14_plan §1.19。
    fsr = {
        temporal = true,
        jitter = true,
    },

    -- ── 批 15.1：SSR（屏幕空间反射）总开关——与上面 aa 三态键**正交** ──
    -- aa 管「谁做抗锯齿」（TAA/FSR），本块管 SSR 这个独立特性；两键可任意组合
    --（SSR 开 × aa 三档都要分别冒烟，默认档不覆盖非默认组合）。
    --   enabled = false（默认）= 渲染图里**根本没有** ssr pass（装饰器不被调用），不领
    --     ssr_trace / ssr_applied RT、不建管线条目 ⟹ 交付路径逐位不变（**结构性**旁路，
    --     不是"有但旁路"——运行时分支挡不住 slang 对源码差异的浮点重排，教训 = light.frag
    --     的 IZB_CONSUME 注释）。
    --   enabled = true = 每帧多**两个**全屏 pass（批 15.3 子步 B 拆 trace + apply）：
    --     trace 半分辨率 march 写 premultiplied ssr_trace，apply 全分辨率做粗糙度感知
    --     模糊后加进成片写 ssr_applied，下游 tonemap/taa/ao_apply 自动改读它。
    --     **deferred-only**：forward 图无 G-buffer，装饰器跳过并 warn（D1 对等豁免 →
    --     docs/phase15_plan.md §0.1）。
    -- **创建期量**：改它要走重启。生效现场看启动日志 "ssr_trace (RGBA16F, …): WxH slot N"
    -- 与 "ssr (RGBA16F, …): WxH slot N" 两行。
    ssr = {
        enabled = false,

        -- ⚠ 本块并存**两类量**（Phase 15 批 15.1 参数面起）：上面 `enabled` 是**创建期量**（决定渲染
        -- 图里有没有 ssr pass、要不要领 ssr_applied RT，改它要重启）。下面六项是**每帧量**——
        -- 每帧经 FrameDrawList 写进 scene data 尾区的 ssrParams(@722336) / ssrGate(@722352)，
        -- shader 读内存：不重编译着色器、不重建管线、不改图形态。
        -- 但 runtime.lua **没有热重载**（RuntimeConfig 只在启动期 load() 一次，热重载只套
        -- scene.lua / camera.lua / log_level.json）⟹ 改这六项同样要重启进程才读得到，
        -- 只是"生效"这一步不需要重建任何东西。
        -- 默认值唯一真相源 = C++ 的 SsrTuning 成员初始化器（rgl/runtime_config.cppm）；本文件
        -- 不写 fallback 常量，缺块/缺键 = 直接用那份默认值（两处定义迟早漂移）。
        --
        -- 下面前五项 = 批 15.1 那五枚 shader 常量的原值（行尾常量名 = 它当初住在哪）。口径与
        -- 两条不变式（bias_px < thickness_px、rough_full < rough_cutoff）→ ssr_trace.frag.slang
        -- 头注释；越界**不钳制**——退化档是反向消融的诊断手段，clamp 掉 = 消融静默失效。
        max_distance_radii = 0.52,  -- kMaxDistanceRadii：射线长度上限 = sceneRadius × 本值（0.52 = bistro 饱和判据定，非靶反推）
        thickness_px = 12.0,        -- kThicknessPx：命中判据的屏幕空间厚度容差，单位 = 像素
                                    -- （15.3C 重定 6.0→12.0：窗口须跟住 march 网格，15.3B 半分辨率
                                    -- 后一步穿深翻倍；读数与反向消融 → runtime_config.cppm 字段注释）
        bias_px = 0.6,              -- kBiasPx：命中点外推防自粘，单位 = 像素（须 < thickness_px）
        rough_full = 0.15,          -- kRoughFull：粗糙度低于此 = 满强度反射
        rough_cutoff = 0.40,        -- kRoughCutoff：粗糙度高于此 = 不反射（中间 smoothstep 过渡）
        blur_scale = 8.0,           -- 批 15.3B：粗糙度感知模糊半径 = roughness × 本值（**全分辨率 px**，
                                    -- ssr_apply 螺旋 taps；0.05→0.4px ≈ 无模糊、0.35→2.8px）。
                                    -- 0 = 合法消融档（纯双线性上采样）。写进 @722352.z
    },

    -- ── 批 15.2 湿面参数（6 枚全部是**每帧量**；子步 B 起消费者已接线）──
    -- 本块**没有创建期开关**：湿面调制不是独立 pass，是 deferred_geometry 内的 G-buffer 调制
    -- （压暗 albedo / 粗糙度端点 / 法线扰动写进 G-buffer，light 与 ssr 读 G-buffer 自动同源），不改图形态；
    -- `strength = 0` 就是它的门关档（合法消融档，配置层不校验不 clamp，理由 → readWetBlock）。
    -- 消费者 = deferred_geometry.frag.slang「湿面调制」段（15.2B 接线，**默认开**）：出厂画面
    -- 含湿面调制（红线 A = `54e40665…`）；`strength = 0` 实测逐位 = 换锚前的旧图（`5ff793bf…`）。
    -- 消融/扫描注入 = `just shot --set strength=0`（tools/smoke.py，不碰配置副本）。
    -- 湿面**身份**（哪些面湿）不占本块键 = 1 − smoothstep(ssr.rough_full, ssr.rough_cutoff,
    -- roughness)，与 SSR 粗糙度门控同区间同源（资产已标定 → docs/phase15_plan.md §2.4 修正块）。
    -- 默认值唯一真相源 = C++ 的 WetTuning 成员初始化器（rgl/runtime_config.cppm）；本文件
    -- 不写 fallback 常量，缺块/缺键 = 直接用那份默认值（两处定义迟早漂移）。
    wet = {
        strength = 1.0,            -- 总开关兼强度：0 = 关（消融档写得出来是判据前提）；**默认开**（用户拍板）
        dark = 0.6,                -- 湿区 albedo 压暗系数（×0.5–0.7 区间取中；结案「只加高光不压暗」的旋钮）
        rough_lo = 0.05,           -- 水洼粗糙度（水膜表面；须 < rough_hi，shader 侧不变式，配置层不校验）
        rough_hi = 0.35,           -- 湿石粗糙度（资产湿面均匀 0.122 给不了空间结构，由程序化调制）
        ripple_amp = 0.3,          -- 水膜法线扰动幅度（0 = 无扰动）
        puddle_freq_radii = 12.0,  -- 水洼噪声频率 = 本值 / sceneRadius（尺度无关；bistro r≈1.16 → 特征尺寸 ≈0.1 世界单位）
    },

    -- ── ★0-b1 簇化渲染路径（meshlet 产物）总门，**默认关** ──
    -- 门关 = 逐位不变是硬护栏（docs/mdi_plan.md P5 拍板）。逐资产门**不在本键**——
    -- = 「该资产有没有簇产物」（§7.8 拍板 3：产物构建期生成、缺失 → 回落现行路径）。
    -- ★1-a1ii-α 起 enable 开 = 恒 L0 簇**影子叠加**（同 objectIndex 重画一遍，正常档与底
    -- 共面 + eLess ⟹ 零像素写入）；默认值唯一真相源 = RuntimeConfig 成员初始化。
    meshlet = {
        enable = false,
        -- ★1-a1ii-α 阳性对照档（判据 3）：叠加改用 DEBUG_OFFSET SPV 变体（clip.z 拉向
        -- 相机 + 翻法线）⟹ 叠加真正写像素，图像必变。enable 关时无意义。
        debug = false,
    },

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
