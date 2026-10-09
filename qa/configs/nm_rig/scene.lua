-- nm_rig（normal map 受控靶）场景配置。生成器 = tools/qa_scenes.py 的 scene_nm_rig，
-- env = tools/env_band_hdr.py。**一个配置一个进程**：normal map 的幅度/频率与 roughness
-- 全部烘在 glb 里（材质只有一个），所以自变量换一档 = 换 glb 文件 = 起一个新进程。
--
-- ## 本靶存在的理由（一条构造性事实，不是"想多测一个场景"）
--
-- rgl/shaders/model_pbr_ibl.frag.slang:52-53：无 normal map 时
--   geoN = worldNormal(M, normalize(input.normal));  N = geoN
-- ⟹ **shading 法线与 specular AA 的输入是同一个量**。而 specular AA（normalmap.slang 的
-- normalFiltering，variance 0.15 / threshold 0.2，与 Filament surface_shading_lit.fs:220
-- 同式同常数）喂的是**几何**法线，平坦面上 ddx/ddy ≈ 0 ⟹ 零增强。
-- 于是「法线高频变化而 specular AA 完全不察觉」那一类噪点**在没有 normal map 的靶上
-- 构造不出来**——不是没测到，是不存在。env_band 的 honesty 已把这条写成自身局限。
--
-- ## 归零清单（每一项都是"把一条轴变成可写进文件名的自变量"）
--
--   light.intensity = 0 / light.ambient = 0  → 直射光与平环境光全关。留下的着色链只有
--       color = envSpec(R, roughness·mipRange) × envBrdfFactor(NoV, r, F0) × env.intensity
--       （forward model_pbr_ibl.frag.slang:79-82 与 deferred light.frag 该式逐字相同）
--       ——这是本靶能做单调性归因的全部底气。
--   ao.strength = 0     → AO 已三次排除，lerp(1,ao,0) ≡ 1.0 逐位旁路
--   skybox = false      → 本靶画幅里本来就没有天穹像素（俯角 25° 把地平线推到画外），
--                         这条只是与 env_band 保持同一形态；deferred 不读它，也不受影响
--   exposure = 1.0      → ACES 前中性倍率
--   shadow.lua 的 mode = "none" + izb = false（同目录 shadow.lua，批 N6b 起阴影键在此）
--   metallic = 1（在 glb 里）→ 漫反射项天然为 0 ⟹ 画面里**每一个亮像素都来自 env 镜面项**
--
-- ## 亮度纪律（本轮实测教训，违反即测量无效）
--
-- speck 是**逐像素计数**且对整体亮度**非单调**（变暗让"4 邻域 ≤60"更容易成立 ⟹ 中段
-- 暴涨；bistro 湿面实测 intensity 1.0→0.5 时 speck 2245→21710）。所以：
--   · intensity 在本靶**只标定一次**（标定过程见 harness 的 calib 阶段），此后所有 cell
--     共用同一个数，**任何跨 cell 比较都不许改它**；
--   · 换分辨率即换口径（speck 逐像素计数），全套数字绑 1280×720。
--
-- ## env 的几何（改任何一个数都要同步改 tools/env_band_hdr.py 的参数）
--
-- 带子仰角 25.0° **必须等于相机俯角 25.0°**（camera.lua），否则镜像线跑出主点行。
-- 布局 = 等距柱状（W=2H，行 0 = 天顶），出处 rgl/texture_pool.cppm::loadEnvCubeMapHdr。
-- hdr_size = 重投影立方图**面边长**（不是源图边长；scene_config.cppm:44 + texture_pool
-- 形参 faceSize）。取 1024 ⟹ mip 级数 11、mipRange = 10 ⟹ roughness 0.12 落在 lod 1.2。
--
-- ⚠ **源图必须是 1024×512**（`--width 1024 --height 512`，即 0.3516°/行）。生成命令：
--   python3 tools/env_band_hdr.py --out build/rgl/env/band_e25_w0.6_i4000.hdr \
--       --elev 25 --band 0.6 --intensity 4000
-- 源图若是 64×32（5.625°/行），"0.6° 窄带"实际是 **11.25° 宽带**：画面亮区宽到 10° 且
-- 不随 roughness 变、交付域背景亮到"4 邻域 ≤60"永不成立 ⟹ phase-lock 恒 0，整轮结论
-- 反向（2026-10-05 实测踩过，根因 = 旧版 --selftest 直接覆写 --out，现已改临时目录）。
-- 复核一条命令：python3 /tmp/opencode/nm/hdr_rows.py <该 .hdr>（应回 1024x512、2 行、
-- 仰角中心 25.137°）。
return {
    styles = { unlit = "model", pbr = "model_pbr_ibl" },
    entities = {
        -- harness 按 cell 改这一行（幅度/频率/roughness 全在文件名里）。
        -- ⚠ `file` 必须**独占一行**：harness 的补丁正则锚在行首（NP.patch 的 ^[ \t]*key=），
        --   与 `type = "model"` 同行会命中 0 次直接抛错（本轮实测）。
        {
            type = "model",
            file = "models/nm_rig_a02.00_b001_r0.12_blocks_t64_s0.300.glb",
        },
    },
    environment = {
        hdr = "env/band_e25_w0.6_i4000.hdr",
        hdr_size = 1024,
        -- 0.003 = 标定值（2026-10-05 重标，标定表见 /tmp/opencode/nm/calib2.py）：
        --   0.0006 → peakL 164（够不到 speck 的 L≥200 判据）/ 0.003 → peakL 234、meanL 37.7、
        --   sat255=0 / 0.01 → peakL 250 / 0.03 → sat255=23376（**饱和 = 测量无效**）。
        -- 旧值 0.0006 是在 env 被 tools/env_band_hdr.py --selftest 覆写成 64×32（源带实际
        -- 11.25° 宽）期间标的，源带修回 0.703° 后通量差 16 倍，故整表重标。
        -- 饱和会把高频削平成 255 平台 ⟹ speck 恒 0 属**测量无效**而非"机制不存在"。
        intensity = 0.003,
        skybox = false,
    },
    light = {
        direction = { 0.0, -1.0, 0.0 },
        color = { 1.0, 1.0, 1.0 },
        intensity = 0.0,
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
