-- 场景配置（启动时加载，字段均可选，缺省回落默认值）
-- model 为空字符串 = 不加载模型，仅渲染 cube
-- pipelines 每条: name（管线表条目名）+ mesh/frag 的 SPV 相对路径（相对 build/rgl/）
return {
    -- model = "models/grass_variations.glb",  -- 快速排查模型（4 primitive / 1 张内嵌 PNG，全 BLEND）
    -- model = "models/castle_of_loarre.glb",  -- Loarre 城堡：11 primitive / 1 纹理，包围球 center=(-3.4,0.6,17.7) r=59.7
    -- model = "models/kyoto_fushimi_inari_taisha_temple.glb",  -- 伏见稻荷大社：18 primitive / 3 纹理，包围球 center≈(0,0,0) r=73.3
    model = "models/bistro_exterior.glb",  -- Phase 7 D1 测试主场景：2.8M 三角 / 1591 primitive / 395 纹理

    -- Phase 6 批次 5：entities 列表（非空 = 权威场景构成，覆盖 model/cube 旧字段；
    -- 缺省 = 由旧字段合成，行为不变）。type = "model"（file 必填）| "cube"；
    -- position/rotation(欧拉角度 X→Y→Z)/scale(均匀) 全可选，缺省 identity
    entities = {
        { type = "model", file = "models/bistro_exterior.glb", scale = 0.01 },
    },

    -- Phase 6 批次 5：entities 列表（非空 = 权威场景构成，覆盖 model/cube 旧字段；
    -- 缺省 = 由旧字段合成，行为不变）。type = "model"（file 必填）| "cube"；
    -- position/rotation(欧拉角度 X→Y→Z)/scale(均匀) 全可选，缺省 identity
    -- entities = {
    --     { type = "model", file = "models/kyoto_fushimi_inari_taisha_temple.glb" },
    --     { type = "model", file = "models/grass_variations.glb", position = { 47.3, 0, -169.3 }, rotation = { 0, 174.6, 0 }, scale = 0.32 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 163.0, 0, 0.5 }, rotation = { 0, 169.3, 0 }, scale = 0.44 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -119.9, 0, 122.7 }, rotation = { 0, 98.2, 0 }, scale = 0.45 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -18.1, 0, -142.0 }, rotation = { 0, 193.8, 0 }, scale = 0.42 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 54.1, 0, 144.5 }, rotation = { 0, 289.8, 0 }, scale = 0.32 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 54.2, 0, -155.7 }, rotation = { 0, 303.9, 0 }, scale = 0.33 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 145.1, 0, 96.1 }, rotation = { 0, 289.7, 0 }, scale = 0.36 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 104.6, 0, 69.9 }, rotation = { 0, 228.6, 0 }, scale = 0.32 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 149.8, 0, -47.2 }, rotation = { 0, 72.3, 0 }, scale = 0.41 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -118.0, 0, 142.0 }, rotation = { 0, 327.4, 0 }, scale = 0.38 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -102.0, 0, -130.8 }, rotation = { 0, 290.0, 0 }, scale = 0.49 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 136.7, 0, 24.7 }, rotation = { 0, 216.6, 0 }, scale = 0.33 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -99.3, 0, -62.3 }, rotation = { 0, 316.8, 0 }, scale = 0.38 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 121.0, 0, 109.2 }, rotation = { 0, 112.4, 0 }, scale = 0.3 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -120.4, 0, 12.3 }, rotation = { 0, 75.6, 0 }, scale = 0.47 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -35.5, 0, -105.2 }, rotation = { 0, 280.1, 0 }, scale = 0.25 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -90.8, 0, 158.2 }, rotation = { 0, 223.3, 0 }, scale = 0.33 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -101.3, 0, 98.3 }, rotation = { 0, 45.7, 0 }, scale = 0.5 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 135.7, 0, 47.7 }, rotation = { 0, 268.7, 0 }, scale = 0.35 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 128.7, 0, -15.5 }, rotation = { 0, 352.5, 0 }, scale = 0.47 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -87.1, 0, -158.9 }, rotation = { 0, 161.0, 0 }, scale = 0.46 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 43.2, 0, -145.0 }, rotation = { 0, 169.5, 0 }, scale = 0.43 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -98.3, 0, 63.2 }, rotation = { 0, 268.7, 0 }, scale = 0.32 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -172.2, 0, 13.4 }, rotation = { 0, 62.0, 0 }, scale = 0.26 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 112.2, 0, 110.1 }, rotation = { 0, 130.0, 0 }, scale = 0.31 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -175.7, 0, 55.2 }, rotation = { 0, 91.7, 0 }, scale = 0.35 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 115.4, 0, -109.3 }, rotation = { 0, 96.5, 0 }, scale = 0.37 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -120.3, 0, 16.3 }, rotation = { 0, 135.9, 0 }, scale = 0.33 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 45.8, 0, -127.9 }, rotation = { 0, 237.3, 0 }, scale = 0.49 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 124.0, 0, -65.1 }, rotation = { 0, 217.1, 0 }, scale = 0.33 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -119.6, 0, 88.9 }, rotation = { 0, 285.6, 0 }, scale = 0.48 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -28.0, 0, 106.9 }, rotation = { 0, 161.8, 0 }, scale = 0.26 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 168.8, 0, 32.3 }, rotation = { 0, 40.4, 0 }, scale = 0.43 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -119.8, 0, 141.1 }, rotation = { 0, 307.3, 0 }, scale = 0.27 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 65.5, 0, 173.9 }, rotation = { 0, 220.8, 0 }, scale = 0.36 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 35.3, 0, -171.5 }, rotation = { 0, 177.0, 0 }, scale = 0.37 },
    -- },

    -- 4K 黑屏 UAF 复验（2026-08-28）：真实 4K 纹理（非合成 TEXTURE_FORCE_SIZE）+ 目标 <2048（llm 1280x720）
    -- = §十 4K 黑屏的精确配置；master（TexturePool 生命周期已修）上复跑，黑 = 独立内核问题，正常 = §十 定性改写为 UAF
    -- model = "models/transparent.glb",  -- Phase 5 透明排序测试（无纹理，4 primitive：2 OPAQUE + 2 BLEND α=0.25）
    pipelines = {
        {
            name = "cube",
            mesh = "generated_shaders/cube.mesh.spv",
            frag = "generated_shaders/cube.frag.spv",
        },
        {
            name = "model",
            mesh = "generated_shaders/model.mesh.spv",
            frag = "generated_shaders/model.frag.spv",
        },
        {
            name = "model_pbr",
            mesh = "generated_shaders/model.mesh.spv",  -- mesh 复用 model.mesh.spv（Phase 4 批次 3）
            frag = "generated_shaders/model_pbr.frag.spv",
        },
        {
            name = "model_phong",
            mesh = "generated_shaders/model.mesh.spv",  -- Phase 5 批次 3：基础 Phong 路线，同 mesh
            frag = "generated_shaders/model_phong.frag.spv",
        },
        {
            name = "model_pbr_ibl",
            mesh = "generated_shaders/model.mesh.spv",  -- Phase 5.5：PBR + IBL 环境光照，同 mesh
            frag = "generated_shaders/model_pbr_ibl.frag.spv",
        },
        {
            name = "model_vegetation",
            mesh = "generated_shaders/model.mesh.spv",  -- Phase 8 植被：PBR + 背光透射 SSS，同 mesh，forward-only
            frag = "generated_shaders/model_vegetation.frag.spv",
        },
        {
            name = "model_light",
            mesh = "generated_shaders/model.mesh.spv",  -- Phase 8 光源本色：emission 主导 + 去金属镜面，同 mesh，forward-only
            frag = "generated_shaders/model_light.frag.spv",
        },
        {
            name = "deferred_geometry_vegetation",
            mesh = "generated_shaders/model.mesh.spv",  -- Phase 8 批次 F：植被 deferred 几何变体（class 1 → emissive.a）
            frag = "generated_shaders/deferred_geometry_vegetation.frag.spv",
        },
        {
            name = "deferred_geometry_light",
            mesh = "generated_shaders/model.mesh.spv",  -- Phase 8 批次 F：光源 deferred 几何变体（class 2 → emissive.a）
            frag = "generated_shaders/deferred_geometry_light.frag.spv",
        },
        {
            name = "skybox",
            mesh = "generated_shaders/skybox.mesh.spv",  -- Phase 5.5 批次 4：全屏大三角采 env cubemap
            frag = "generated_shaders/skybox.frag.spv",
        },
    },

    -- Phase 4 批次 3：风格名→管线规格名映射（loader 输出 unlit 字段派生风格，缺失回落默认）
    -- Phase 5 批次 3：pbr 改 "model_phong" 即切基础 Phong 路线（两条光照并存）
    -- Phase 5.5：pbr 改 "model_pbr_ibl" = IBL 环境光照路线（须配 environment，否则启动契约报错）
    styles = {
        unlit = "model",
        pbr = "model_pbr_ibl",
        vegetation = "model_vegetation",  -- Phase 8：植被 → 背光透射 SSS
        light = "model_light",            -- Phase 8：光源 → 本色（去金属镜面）
    },

    -- Phase 8 批次 F：材质名连连看规则表（有序，首个命中生效；材质名含 pattern → 风格键）。
    -- 材质名 = glTF 官方元数据（loader 透出 materialName）；bistro 全 132 材质：Foliage_ 前缀
    -- = 植被（含 OPAQUE 花床/枝干），Emissive 含串 = 10 光源，其余落 pbr。
    material_styles = {
        { pattern = "Emissive", style = "light" },
        { pattern = "Foliage", style = "vegetation" },
    },
    -- Phase 8 批次 F：风格→deferred 路线规格名（未列风格回落 deferred_geometry 本体）；
    -- 变体把类别写进 G-buffer emissive.a，light pass 分支复刻 forward 专属视觉
    deferred_styles = {
        vegetation = "deferred_geometry_vegetation",
        light = "deferred_geometry_light",
    },

    -- Phase 5.5 环境 cubemap（缺段 = 不启用）。faces 按 Vulkan 层序 0-5 = (+X,−X,+Y,−Y,+Z,−Z)
    -- 排列，素材面顺序定案表见 docs/archive/phase5_5_plan.md（rainbow 方案 B：ft,bk,up,dn,rt,lf；
    -- nightSky 层序 = 1,3,5,6,2,4）。intensity 进 scene data @48；skybox = 背景天穹（批次 4）。
    -- Phase 8 批次 ②b：hdr 非空 = 优先走 Radiance .hdr 环境（CPU 等距柱状→RGBA16F cubemap
    -- 重投影，官方 Bistro 素材 san_giuseppe_bridge）；素材 gitignore，文件缺失自动告警回落
    -- faces PNG 路径。hdr_size = 重投影面边长（2 的幂，默认 2048）。
    -- intensity：官方 Falcor 配方为 10，但那是对其 irradiance 卷积归一后的校准值；本工程
    -- 简化 IBL 直接采 cubemap top-mip 平均（env 线性均值本就 ≈0.56），×10 必过曝。实测
    -- 1.0 时 forward/deferred 建筑与背景曝光正常（docs/phase8_plan.md ②b/③a 记录）。
    environment = {
        hdr = "env/san_giuseppe_bridge_4k.hdr",
        hdr_size = 2048,
        dir = "skybox/rainbow",
        faces = { "rainbow_ft.png", "rainbow_bk.png", "rainbow_up.png",
                  "rainbow_dn.png", "rainbow_rt.png", "rainbow_lf.png" },
        intensity = 1.0,      -- env 强度（④a 后 = SH 卷积全量天空辐照，p50≈4 的天空 π 卷积
                              -- ≈13；1.0 时天光 ≈14× 太阳光 = 影面全蓝，故历史一度压到 0.2）。
                              -- **2026-09-29 按"同材质阴影/受光比"重标定回 1.0**：0.2 时阴影面
                              -- 只有受光的 0.2%（线性域）——墙面 albedo 浅灰 150 却渲成 p50=13，
                              -- 即"楼房背光面过暗"的根因。户外散射光合理带 0.03~0.10，
                              -- 1.0 实测 0.037，1.3 起过曝。IBL 通路本身无故障（env 响应单调线性）。
        skybox = true,
    },

    -- Phase 5 批次 2：定向光（全字段可选回落 scene.cppm Light 默认值；世界空间）
    light = {
        direction = { 0.5, -0.75, -0.45 },  -- 照射方向（消费时归一化；从相机侧上方照）
        color = { 1.0, 0.98, 0.95 },
        intensity = 12.0,  -- 2026-09-29：15→12。env 抬到 1.0 后天空补光变足，太阳相对降一档
                           -- 保住对比（扫描表：sun12/env1.0/exp1.0 → 墙阴影 46、饱和 0.26%）
        ambient = { 0.25, 0.25, 0.28 },
    },

    spin = false,  -- 场景整体时间旋转（true 便于肉眼判断渲染对错；false 静止便于排查遮挡）
    -- 帧驱动 spin（诊断激励门，默认关 = 逐位今天）：spin_frame_angle ≠ 0（rad/帧）时
    -- angle = spin_frame_angle × 帧号（**纯函数**，与帧时长无关 ⟹ 跨进程可复现的帧序列，
    -- 时域判据的参考图才能与候选图逐帧对齐；dt 累积随帧时长漂 = 不可复现，
    -- tools/imgqa/temporal.cpp 头注释）。spin_axis 覆写旋转轴（默认 (0.5,1,0) = 今天；
    -- 诊断激励用绕 up 轴 yaw = {0,1,0}）。消费方 = imgqa temporal-motion。
    -- spin_frame_angle = 0.0,
    -- spin_axis = { 0.5, 1.0, 0.0 },
    cube = false,  -- 是否渲染硬编码 cube（Phase 1 遗留，原点 ±0.5，与模型互穿时建议关掉）
    exposure = 1.0,  -- tonemap 曝光（ACES 前线性倍率，缺省 1.0 = 直通）。2026-09-29：1.2→1.0。
                     -- env 从 0.2 抬到 1.0 后画面整体亮度本就上来，曝光回到中性 1.0，
                     -- 避免双重提亮把全图推到过曝区（饱和像素 0.78%→0.26%）。

    -- Phase 11 批 11.4：SSAO（ambient occlusion）施加强度。ao pass（state="ao"）读深度
    -- 重建世界坐标算**可见度**（1 = 通透）写 r8_unorm RT，tonemap 以 lerp(1, ao, strength)
    -- 在曝光前相乘。strength = 0 → 逐位旁路（实测三个 shader 版本间 strength=0 截图 sha256
    -- 相同 = BIT-IDENTICAL 基线成立）；0..1 之间连续可调。
    -- 0.5 = 2026-09-29 Bistro 外景 look-dev 值：灌木体块/雨篷接触/踏板车接地的分离感出来
    -- 而不过火（1.0 时深色立面与灌木被压得太死）。
    -- 半径/采样数/增益等**形状**参数是 ao.frag.slang 内的常量（look-dev 值），不走配置。
    ao = {
        strength = 0.5,
    },

    -- 批 12.6 TAA 时域抗锯齿。缺段 = 全默认即关；enabled 是**图形态**开关
    -- （关 = 图里没有 taa/taa_commit pass，比「pass 常驻 + shader 旁路」更强），改完要重启。
    --
    -- 2026-10-03 曾拍板默认开，**当天实测后撤回**。两条账（本机 = AMD Radeon 8060S /
    -- RADV STRIX_HALO，bistro 1280x720 headless，同构建同会话 A/B；数字绑机器）：
    -- ① 性能不是障碍：frame p50 26.72 → 27.06ms（+0.335ms = +1.3%），gpu.frame 仅
    --    +0.064ms（GPU 侧几乎免费），代价全在 CPU record（+0.42ms，新增 zone 只有
    --    taa_commit blit）；该机 CPU-bound 余量 16.2ms。
    -- ② 画质**不合格**（真实场景首测；此前判据全跑在合成靶 + env 关闭上，外推失败）：
    --    · **震**：静止场景 TAA 关逐帧 15/15 对逐位相同，开则逐帧平均 |Δ| 2.36/255、
    --      57% 像素每帧在变（用户肉眼 = "地震"）。不是 AO（ao.strength=0 只降到 2.20）、
    --      不是随帧漂移（8 帧 vs 150 帧 −48.5% vs −49.5%）。
    --    · **糊且糊过头**：梯度能量 −49.2%。用 `feedback=1.0` 连拍 16 帧取平均构造出
    --      **理想 16 相位超采样**作独立参考——它只该糊 −24.2%，实测 TAA 糊 −49.2%
    --      （梯度比 0.671）⟹ **比自身应收敛到的结果还糊 1.5 倍**，多出来的不是抗锯齿代价。
    --    · 合成靶的 √16=4.00× 抑制在真实场景只有 2.22×（不同算子，不直接对标，但方向一致）。
    --    · 消融：`jitter_scale=0` 时糊只剩 −3.4% ⟹ 代价随抖动幅度而来。
    --    · 已修（2026-10-03）：**历史重采样复利糊化**——历史就是累积图，每帧一次双线性
    --      重采样 = 老贡献被反复低通。改精确 4×4 Catmull-Rom 后糊度对历史年龄的依赖**消失**
    --      （feedback 0.12/0.02：−49.7/−57.0% → −32.4/−32.4%），出厂值 −49.7% → −32.4%。
    --    · **仍在震**（CR 后逐帧 |Δ| 反升到 4.0/255）：主因是**输出本就长在抖动空间**
    --      ——out_t = O_t(uv − jitter_t)，整幅图随 jitter 相位每帧平移 ≤1px，植被类高频
    --      场景下这就是"地震"。⚠ 曾据「实机/软件EMA = 8.66×」判为多余机制，**该参考无效**
    --      （软件 EMA 在未抖基空间累积，引擎在抖动空间，不是同一个量）；已撤回。
    --      ✅ 2026-10-03 批 13.1d **该机制已用绝对口径独立证实**（不再是推测）：见下面
    --      「整幅游动 = 机制不是 bug」那条与 docs/phase13_plan.md §1.9。
    --    · 次因 = resolve 端零锐化补偿（sharpen/unsharp 零命中），只能解释部分糊、不解释震。
    -- 开的前置 = phase13 批 13.1（判据 = 上面那条「理想 N 相位平均」参考）+ 13.3。
    --
    -- 2026-10-03 批 13.1c/d 实测更新（两条根因，全案见 docs/phase13_plan.md §1.8/§1.9）：
    --  · 「边缘闪烁」= **Catmull-Rom 历史核写成奇函数**（三次项 a³ 应为 |a|³）⟹ t=0 权重
    --    (0,1,−3,8)、Σ=6 ⟹ 2D 直流增益 36× ⟹ 历史被 saturate 顶界后 AABB 判越界 ⟹ 98.3% 像素
    --    每帧拒收历史 = 输出退化单帧。**已修**（判据 = imgqa taa-swim 的 temporal-identity：
    --    零抖动 + 静止 ⟹ TAA 开必须等价于关；修复前离零抖动基图 1.337/255，修复后 0.016）。
    --  · 「整幅游动」= **机制不是 bug**：输出在抖动空间，out_t(uv) = F(uv − jitter_t) ⟹ 整幅
    --    画面随抖动相位平移。实测现行工作点相邻帧游动 **0.717px 均值 / 1.051px 最大**（闭式期望
    --    0.667px），且 feedback=1（完全不累积）时 0.674px ⟹ **时域累积压不掉它**。
    --    已消（2026-10-04 批 13.2）：`base_space = true`（默认）把偏移折进 tonemap 采样坐标，
    --    输出落回基网格，实测游动 0.643 → **0.060px**（判据 `imgqa taa-swim` 的 temporal-base-space）。
    --  · 口径更正（2026-10-04，此前本注释方向写反）：**图像内容位移 c_t = jitter_scale·(+jx, −jy)**
    --    （glm 列主序 `proj[2][0]` = clip.x 的 v.z 系数 + clip.w=−v.z 翻号 ⟹ Δndc=+2j/W；y 被
    --    负高视口再翻一次）。探针 `jitterDeltaPx` 返回的是 **−c_t**（`freeShift` 口径，模长一致
    --    所以历次读数不受影响）。裁决依据 = 门开态游动归零（符号若反会**翻倍**到 ~1.33px）。
    --  · ⚠ **批 13.5：上面「已消」只消了显示那一半，`base_space` 语义已改义**。
    --    13.2 把偏移折进 tonemap ⟹ 输出落回基网格，但**历史仍住抖动空间** ⟹ 累积内部每帧按
    --    `δ_t = c_t − c_{t−1}`（闭式 0.667px）亚像素重采样历史 ⟹ 静止机位仍有「**固定频率
    --    （= 抖动周期 16）+ 固定位置（边缘）**」的 shimmer（实测交付相邻帧 9.07% 像素 >2LSB，
    --    且 lag-16 同相位逐位相同 ⟹ 变化 100% 由相位索引决定）。
    --    13.5 = 把 un-jitter **前移到 taa pass 输入侧** + 历史采样修正 c_{t−1} ⟹ 累积发生在
    --    基空间 ⟹ 静止机位历史**点采样零重采样**。tonemap 侧偏移已删（留着 = 双重 un-jitter）。
    --    `base_space` 现义 = **基空间累积开关**（门关 = 改前的 `base_space=false` 形态，
    --    同构建可 A/B）。全案 → `docs/taa_history_space_plan.md`。
    -- 2026-10-05 补记：**默认关的现行真实理由 = BIT-IDENTICAL 可预测性，不是画质。**
    -- 上面那两条画质账（震 / 糊 −49.2%）已在 13.1c/d · 13.1e · 13.2 · 13.5 修到游动 0.060px，
    -- "画质不合格"这个理由**已经不成立**，留着会让人去修一个修好的东西。真卡点是：TAA 开 =
    -- 抖动相位每帧变 ⟹ 「单帧 vs 固定基准」的位比必然失效。**但这不是死结**，三个位级出口
    -- 实测都在（见 docs/还债总账.md 债-46）：① `lag-16` 同相位逐位相同 ⇒ 相位可锁 ⇒ 序列可位比；
    -- ② `temporal-identity` 恒等式门（jitter_scale=0 + feedback=1 ⟹ 开=关）；③ warm=2 vs warm=128
    -- 预热无关性。SSIM/PSNR 已在总账 §3 否决（L1 对模糊守恒），**出口是序列级位比不是感知指标**。
    -- 三者都还没落地成基准 = 债-46。
    taa = {
        enabled = false,
        -- feedback = 0.12,        -- 本帧权重（Filament 同默认 ⇒ ≈19 帧收敛）
        -- jitter_scale = 1.0,     -- 抖动幅度倍率，1 = ±0.5px
        -- motion_compensation = true,  -- prevSpin 刚体重投影（关 = 纯相机重投影）
        -- reprojection = true,      -- 重投影消融门（诊断，默认开 = 逐位今天）：关 = 历史按
        --                             -- 「同基位置」采样（prevViewProj := 本帧矩阵 + 位移清零
        --                             -- + motion 单位阵）。相机运动激励下读数必须显著变差
        --                             -- （imgqa temporal-motion 的 M3）；静止机位下关与不关逐位相同。
        --                             -- ⚠ 与 motion_compensation 的分工：那条只关 prevSpin 场景旋转
        --                             -- 补偿（相机不动时是恒等阵 = 消融无效），本条关相机重投影本身。
        -- diag_reject = false,      -- 历史拒收诊断门（默认关）：开 = taa.frag 把逐像素「历史怎么了」
        --                             -- 码编进 taa_out 的 alpha（0 收下 / 0.25 盒裁剪 / 0.5 盒拒收 /
        --                             -- 0.75 重投影拒收 / 1 无历史），RGB 逐位不变。回读 =
        --                             -- POST /shot {"attachment":"taa_out"}（消费方 = imgqa temporal-motion）。
        -- ── 批 13.7 静态机位历史权重提升（static boost，总门默认**关**）──
        -- 机制账：α=0.12 的 EMA 等效窗 ≈8 帧 < Halton(2,3) 抖动周期 16，16 帧周期分量按
        -- |H| = α/|1−(1−α)e^{−j2π/16}| = 0.312 漏过 31% ⟹ 静止机位 1px 细线「固定频率 +
        -- 固定位置」shimmer。机位连续静止（cameraStatic 且历史可用）达 static_ramp 帧后，
        -- 把**本帧权重**线性压到 static_feedback ⟹ 等效窗 1/floor ≫ 16。线性域已验证
        -- 收敛值离理想 16 相位平均只差 1.5–4% 且不随窗变长恶化（不拿糊换安静）。
        -- ⚠ 方向红线（与 feedback 同语义，主 agent 翻过一次车）：static_feedback 是
        --   **本帧权重**（taa.frag: lerp(clipped, cur, x)）⟹ **调小 = 更信历史**，
        --   「提升历史」= 「把本值调小」。
        -- 任何跳切 / resize / TAA 关 ⟹ 计数器立刻归 0 = 立刻回到 feedback（不拿鬼影换安静）。
        -- ⚠ 已知边界（待记债）：静止判据只看相机位姿、不认识场景内运动（无 velocity
        --   buffer，唯一保护 = 既有邻域盒裁剪）——静态机位 + 动物，累积越长鬼影越重。
        -- static_boost = false,     -- 总门（默认关 = 逐位今天的 feedback 行为）
        -- static_feedback = 0.01,   -- 静止地板（**本帧权重**；0.01 ⇒ 等效窗 ≈100 帧）。
        --   档位是 2026-10-05 扫出来的（aa_target 交付态 base_space=true，meanSwing / 梯度比）：
        --   门关 0.00446/0.968 · 0.05→0.00226/0.953 · 0.02→0.00110/0.941 · 0.01→0.00062/0.932
        --   三档梯度比全在 [0.85,1.02] 内 ⟹ 按「带内取残差最低」拍 0.01（判据变体出数，
        --   `imgqa temporal-residual --static-boost <地板> --base-space`）。
        -- static_ramp = 16,         -- 连续静止多少帧爬满地板（= 抖动周期 16）
        -- ── 历史裁剪消融门（批 13.11，默认 clip_mode=0 = **逐位今天**）──
        -- 根因（`调试历史.md` 2026-10-06 实测）：`clipToAabb` 在 **blend 之前**替换历史，而混合是
        -- `lerp(clipped, cur, α)` ⟹ **一次裁剪 = 一次历史重置**（clipped 的权重是 1−α）。1px 高对比
        -- 细线的 box5 在 16 个抖动相位间**双峰**（有杆 / 没杆）⟹「历史是否被重置」由相位决定
        -- ⟹ 相位依赖方差以 100% 权重注入输出。用户机位实测：盒裁剪 6.60% 帧均面积扛 **97.8%**
        -- 残余质量（`塌回 centroid` 恰 0.00%），抑制比 ρ = 0.2501 vs 闭式 0.0827。
        -- 上面那句「唯一保护 = 既有邻域盒裁剪」因此是**双刃**：它既是鬼影的唯一保护，也是静止
        -- 闪烁的主要来源——所以本门是**消融/修法实验位**，不是给用户调画质用的。
        -- clip_mode = 0,            -- 0 现行 / 1 关盒 / 2 扩1.5× / 3 box9 / 4 软裁剪 / 5 扩K× / 6 信任门
        --                           -- 7 luma 记忆（🔴 自带极限环）/ 8 mem 门控软裁剪（🔴 鬼影否决）
        --                           -- 9 mem 接受 + ext 斜率（🔴 债-62 结案：单标量接受域无解）
        --                           -- 10 臂 8 接受域 + **α 交还逐像素证据**（当前候选：boost 的长窗
        --                             原本按「相机静止」发给全画面，现改由裁剪权重 w 逐像素授权）
        -- clip_soft = 1.0,          -- mode=4→λ · mode=5→扩盒倍数 K · mode=6→γ_up 覆盖 · mode=7/8/9/10→decay
        --   （mode=7/8/9 的 0 = 用**推导值 1 − 1/16**：记忆覆盖一整个抖动周期 = 全体相位的支撑集）
        --   （**无量纲**）。弹簧不是墙：单个相位的误判只能搬动 λ 份历史，而真鬼影仍收敛、只慢 1/λ 帧。
        --   ⚠ 选值安全线：鬼影衰减 ≈ 1/λ 帧 vs 累积窗 ≈ 1/α 帧 ⟹ **λ < α 时盒不再是鬼影刹车**
        --   （那时鬼影由混合决定 = 债-49 风险区）。判据消费方 = `imgqa temporal-residual --clip`。
        -- 首轮 A/B 实测（2026-10-06，brickfence）：关盒 ρ = 0.0804 vs 闭式 ρ* = 0.0827（偏 0.97×）
        -- ⟹ 拿掉裁剪后 TAA **精确**按 EMA 闭式工作；而绕 centroid 扩 1.5× 与 box9 几乎无效
        -- （ρ 只从 0.2501 到 0.22/0.21）⟹ 被夹的历史不是「差一点出盒」而是**出盒很远**：高频细节上
        -- 5 抽头空间盒的尺度远小于累积历史的时域变化量。所以「把盒放大一点」这条路已被证伪，
        -- 需要的是让盒**记住近几帧出现过什么**（时域），不是把空间邻域撑大。
        -- mode=6 **历史信任门**（当前主推修法）：把「本位置最近有多常通过本测试」存进 taa_out.a
        -- （既有 taa_out→taa_hist blit 携带 ⟹ 零新附件），按 lerp(hist, hardClip, 1−trust) 调校
        -- 裁剪咬合深度。γ_up = 1/16（= **抖动周期**：相位没走完一个周期无从估计误报率）、
        -- γ_dn = 0.5（≈2 帧塌，须远快于最短累积窗 1/α，否则鬼影先于保护出现）。
        -- 为什么通用：单帧信息**原理上**分不开「合法亚像素细线」与「陈旧历史」（单帧里逐字节
        -- 同形）⟹ 只能引入时域状态；而"最近多常通过本测试"就是本测试自己的误报率，与场景无关。
        -- ⚠ 与 diag_reject **互斥**（抢同一个 alpha）：diag 开时信任门失效、退化成 mode 0。
    },
}
