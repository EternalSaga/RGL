-- 变体 1「带边」场景（tools/qa_scenes.py 的 env_band + tools/env_band_hdr.py 的程序化 env）。
--
-- 要复现的现象：静止机位下「相位锁定的高亮噪点」（16 个 TAA 相位里恰好 1 个相位冲到 255）。
-- 机制假设 = 低 roughness 金属面的 IBL 镜面项反射了高对比 env 的极亮窄带，被抖动切成噪点。
--
-- ## 本配置的归零清单（每一项都是"把一条轴变成可写进文件名的自变量"）
--
--   environment.intensity = 1.0  ← **不自变量化地留着**：env 是本实验的唯一激励源，
--                                  归零它 = 「env 关掉」那一行实验（harness 按 cell 改）
--   environment.skybox  = false  → 无背景贡献，画面只剩地面（上半屏纯黑）
--   light.intensity     = 0.0    → **直射光关掉**（记录：选了"关掉"而非"留着"，理由 =
--                                  直射光会在金属面上叠一条高光带，与 env 窄带反射在同一个
--                                  像素上相加，speck/phase-lock 无法归因到 env。bistro 那侧
--                                  的消融已实测"直射光关掉白点数不变（49→49）"，关掉不改变
--                                  被测机制，只去掉混淆）
--   light.ambient       = 0      → 平环境光项归零
--   ao.strength         = 0      → AO 已三次排除，逐位旁路（lerp(1,ao,0)≡1.0），减少干扰
--   exposure            = 1.0    → ACES 前中性倍率
--   shadow.lua 的 mode = "none" + izb = false（见同目录 shadow.lua，批 N6b 起阴影键在此）
--   metallic = 1（在 glb 里）    → 漫反射项天然为 0 ⟹ 画面里**每一个亮像素都来自 env 镜面项**
--
-- 于是剩下的着色链只有一句：color = envSpec(R, roughness·mipRange) × envBrdfFactor × intensity
-- （forward：rgl/shaders/model_pbr_ibl.frag.slang:79-82；deferred：light.frag.slang:218-222，
--   两路该式逐字相同）——这是本 rig 能做单调性归因的全部底气。
--
-- ## env 的几何（改任何一个数都要同步改 tools/env_band_hdr.py 的参数）
--
-- 带子仰角 3.0° **必须等于相机俯角 3.0°**（camera.lua），否则镜面反射的镜像线跑出主点行。
-- 布局 = 等距柱状（W=2H，行 0 = 天顶），出处 rgl/texture_pool.cppm::loadEnvCubeMapHdr。
-- hdr_size = 重投影立方图**面边长**（不是源图边长；出处 scene_config.cppm:44 注释
-- 「重投影面边长」+ texture_pool.cppm:450 形参名 faceSize）。取 1024 而非默认 2048：
-- 启动期重投影 + GGX 预滤波（256 采样/texel）的耗时随面边长平方涨，1024 下 mip0 角分辨率
-- 已达 90/1024 = 0.088°/texel ≈ 1.4 像素（画面角分辨率 45/720 = 0.0625°/px），
-- 再细就被像素网格吃掉，换不来新信息只换来启动等待。
return {
    styles = { unlit = "model", pbr = "model_pbr_ibl" },
    entities = {
        { type = "model", file = "models/env_band.glb" },
    },
    environment = {
        hdr = "env/band_e3_w0.6_i4000.hdr",
        hdr_size = 1024,
        -- 0.0006 = **标定值**，不是手调。标定过程见 /tmp/opencode/band_calib.py：
        -- 带子 radiance 4000 经 GGX 预滤波后仍高出 ACES 拐点约 10³，intensity=1.0 时反射带
        -- 整片饱和成 41 行的 255 平台 —— 饱和把高频全削平，speck/phase-lock 恒 0 属于
        -- **测量无效**而非"机制不存在"。扫 intensity 到峰值 luma 234/230/223/214
        -- （四档全部 >200 且 satRows=0）才是可用工作点。
        -- ⚠ drop9（剖面从 luma 200 掉到 60 的行数）= 4~8 行，且**不随 intensity 变**
        --   （亮度只沿剖面平移两个阈值，不改剖面斜率）——这条不变量就是"调亮度救不回来"的
        --   观测证据，见最终报告。
        intensity = 0.0006,
        skybox = false,   -- ⚠ 只在 forward 生效，见 render_graph.lua 头注①
    },
    light = {
        direction = { 0.0, -1.0, 0.0 },
        color = { 1.0, 1.0, 1.0 },
        intensity = 0.0,          -- ← 直射光关（理由见头注）
        ambient = { 0.0, 0.0, 0.0 },
    },
    spin = false,
    cube = false,
    exposure = 1.0,
    ao = { strength = 0.0 },
    -- enabled 是**图形态**开关（关 = 图里没有 taa/taa_commit pass）。harness 按 cell 覆写
    -- enabled / jitter_scale；这里写开 = 本目录的"基线 cell"是 TAA 开的形态。
    taa = {
        enabled = true,
    },
}
