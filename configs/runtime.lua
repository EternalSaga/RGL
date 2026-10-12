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
        -- 每帧经 FrameDrawList 写进 scene data 尾区的 ssrParams(@723776) / ssrGate(@723792)，
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
                                    -- 0 = 合法消融档（纯双线性上采样）。写进 @723792.z
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

    -- ── ★0-b1 簇化渲染路径（meshlet 产物）总门，🔴 **★18-C 起默认开** ──
    -- 🔴 门关 = 逐位不变**仍是硬护栏**（docs/mdi_plan.md P5），但**出厂不再是门关态**（2026-10-12
    -- 用户拍板「默认开 D=5」）⟹ 要门关必须显式给 `enable = false` + `shadow_lod = -1`。
    -- 出厂默认截图锚随之从 54e40665…（门关）换成 95edc8cc…（本态）；两锚与三条已明写代价
    -- （+175.5MiB 显存 / +1.2s 启动 / 连带 α 叠加段白付 1.59ms）→ docs/mdi_plan.md §18.11。
    -- 逐资产门**不在本键** = 「该资产有没有簇产物」（§7.8 拍板 3：产物构建期生成、缺失 → 回落现行路径）。
    -- ★1-a1ii-α 起 enable 开 = 恒 L0 簇**影子叠加**（同 objectIndex 重画一遍，正常档与底
    -- 共面 + eLess ⟹ 零像素写入）；默认值唯一真相源 = RuntimeConfig 成员初始化。
    -- 🔴 ★1-a1ii-β-1 起本块四枚布尔走**严格类型**读：写成字符串/数字 = 启动失败并点名键，
    --   不再静默当键缺失（实测旧行为：`switch = "yes"` 跑完整程 errors=0 warnings=0，而实际
    --   跑的是关档 = 截图哈希判据整批假绿）。coplanar 的未知值 fail-fast 同纪律。
    meshlet = {
        enable = true,
        -- ★1-a1ii-α 阳性对照档（判据 3）：叠加改用 DEBUG_OFFSET SPV 变体（clip.z 拉向
        -- 相机 + 翻法线）⟹ 叠加真正写像素，图像必变。enable 关时无意义。
        debug = false,
        -- ★0.5-b2 共面双臂：叠加那批 draw 自带的深度比较档（动态状态，只作用叠加 draw，
        -- pass 与旧路径状态序列不动；off = 不碰 = 与门关逐 bit 同 = A1 构造性保证）。
        --   "off"   = 现行 eLess（共面 ⟹ 全拒零写入）
        --   "equal" = eEqual：共面像素放行写回 ⟹ 截图与门关逐位相同 = 簇路径属性/材质/
        --             切线手性与旧路径逐位同（盲区 = 不共面像素，EQUAL 不通过不写）
        -- 🔴 "ne" = eNotEqual：**不是判据，只能人工看**。它原本想验"位置逐位同 + 覆盖集同"，
        --   但叠加画的是该资产的**全套 primitive（含被遮挡面）**，而被遮挡面的深度永远 ≠ buffer
        --   里那个正面的深度 ⟹ NOT_EQUAL 在被遮挡区**恒放行**（实测放行 29.6% 屏 = 被遮挡面积，
        --   且链式写到最后画的 primitive 赢）。要救它得先把叠加限制到前向可见像素（stencil/prepass）。
        --   位置逐位同这条改由 equal 臂的**放行集**承担：实测 32.74% ≈ debug 覆盖足迹 34.27%。
        -- 未知值 = 启动 fail-fast（aa/present_mode 纪律：静默回落 = 四臂假绿的地基事故）。
        -- 与 debug/tint 正交；enable 关时无意义（叠加零 draw）。
        coplanar = "off",
        -- ★0.5-b2 双臂阳性对照用：叠加改用 TINT SPV 变体（着色输入 = 常数：normal/uv/
        -- tangent 全写死，clip 一 bit 不动 ⟹ 深度与覆盖集与底逐位同，只有"写回的颜色"
        -- 不同 ⟹ 逐 primitive 平色块）。coplanar=equal + tint 图必须变 = equal 臂的
        -- 非空洞证明；coplanar=off + tint 必须 = 基线（eLess 共面全拒）。
        -- 与 debug 同真时 tint 优先（warn 出声，不建组合变体）。enable 关时无意义。
        tint = false,
        -- ★1-a1ii-β-1 原位取代档：= true（且 enable = true）时，门开资产在主 opaque 段里
        -- 由簇路径 draw **1:1 原位**顶掉旧 mesh draw（同 objectIndex / 同 pushData / 同段内
        -- 位置，只换管线条目与 task 数 = 2×L0 簇数），而不是 α 那种「旧路径照画 + 共面影子
        -- 叠加」。原位而非另开一段的理由 = opaque 段序被 GPU culling 候选序对账强依赖。
        -- 与叠加互斥：本键开 = 叠加段清空（同一批几何不画两遍），装配层 warn 出声。
        -- 🔴 只在 diagnostics.cull_replay = "cpu"（默认档）生效；非默认档不切 + 启动 warn
        --   一次（gpu_sync 档 opaque 段由候选表重放，段内换管线不在那条路上）。
        -- 边界（β-1 不做）：阴影/CSM、picking、IZB mask 重放、透明排序仍走旧路径。
        switch = false,
        -- ★1-a2-i 逐簇视锥剔除：= true（且 enable = switch = true）时每帧一次 compute 逐 L0 簇
        -- 做保守视锥测试（与 CPU 参考剔除器 cullClustersLinear 同式），mesh shader 对不可见簇
        -- 早退。判据（🔴 主判据是零阈值的「cull=on 逐位 = cull=off」——保守剔除按构造不可能
        -- 改变交付图像）与定价 → docs/mdi_plan.md §17。单独开 = 无消费者，装配层 warn 出声。
        cull = false,
        -- ★18-B1 阴影段**簇路径**的 uniform 选级 D（docs/mdi_plan.md §18.5/§18.6/§18.7）：
        --   -1        = 本档关 ⟹ 一条阴影条目都不建、一行日志都不打，阴影全走既有旧 mesh 管线
        --               （逐位不变护栏，P5 拍板；🔴 出厂**不再是** -1，要它必须显式给）
        --    0        = 🔴 阳性对照 L0 臂（§18.6 判据 2）：同一新管线画每 prim 的 L0 前缀，
        --               先证明「管线画的是对的几何」，再谈 level D
        --    N >= 1    = uniform 选级 D=N：每 prim 画 min(D, 本 prim 最深级) 那一段连续子区间
        -- 🔴 **★18-C 起默认 5**（2026-10-12 用户拍板「默认开 D=5」；数据推荐档 = 5.10% L0 三角，
        -- §18.2 实测）。生效需要 enable = true（簇四段 buffer 的描述符槽由 enable 的接线提供）；
        -- 只给本键不给 enable 时装配层**出声**，不静默空开关。画质/成本对照表 → §18.9 末。
        -- 🔴 必须是整数（写成字符串/浮点 = 启动失败点名键）。
        shadow_lod = 5,
        -- ★18-B1 回落策略（D 超过某 prim 自身最深级时怎么办）："deepest" | "l0"。
        -- 🔴 它比 D 更主导成本（§18.2：D=7 下 deepest = 3.32% 而 l0 = 34.97% 的 L0 三角），
        -- 故未知值 = 启动 fail-fast（coplanar/aa/present_mode 纪律：静默回落 = 成本判据整批读错臂）。
        shadow_lod_fallback = "deepest",
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
