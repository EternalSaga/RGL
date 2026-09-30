-- 分析探针场景（批 12.2d）：**直接光之外的每一项都归零或关掉**。
--
-- 配套参考实现 = imgqa probe（把 shadeDirect + ACES + 硬件 sRGB 独立复刻一遍，作为
-- 交叉校验的第二实现；12.3b 起接替已删除的 tools/analytic_ref.py）。跑法：
--   ./build/tools/imgqa/imgqa probe   （--light 覆写会连引擎配置一起改，见 cmdProbe）
-- 实测（本机 RADV，2026-09-30）：**逐字节零误差**命中——退化光向 (0,0,-1) 下
-- 屏幕中心 = (113,56,249)，40° 倾斜光下 = (88,43,244)。
-- 刻意用 **40° 倾斜光**而不是垂直光：垂直光下 N≡V≡L 使 D/F/G 退化成常数，
-- 判据虽好算但弱；倾斜后 NdotL=0.766、H≠N，BRDF 三项全展开，判别力更强。
-- 期望 L（指向光源）= -direction = (-0.642788, 0, 0.766044)。
--   environment.intensity = 0  → IBL 漫反射 + 镜面两路同时归零（shader 里统一乘）
--   environment.skybox = false → 无背景贡献（屏幕中心本来也被墙挡住，双保险）
--   light.ambient = 0          → 平环境光项归零
--   ao.strength = 0            → lerp(1,ao,0) 位级 = 1.0（BIT-IDENTICAL 旁路）
--   exposure = 1.0             → ACES 前的线性倍率取中性
--   diagnostics.shadowPass = false → 阴影因子恒 1
-- 剩下的只有：color = lightColor × NdotL × (albedo/π + GGX_spec)
return {
    -- 显式钉住交付风格（缺表回落 model_pbr = 非交付路径——12.3b 查实：探针此前一直
    -- 测的是非交付 frag；12.3b 调试中曾被 git checkout 误毁，此为恢复重钉）
    styles = { unlit = "model", pbr = "model_pbr_ibl" },
    entities = {
        { type = "model", file = "models/cornell_box.glb" },
    },
    environment = {
        hdr = "env/san_giuseppe_bridge_4k.hdr",
        hdr_size = 2048,
        intensity = 0.0,     -- ← 关键 2：IBL 两路统一归零
        skybox = false,      -- ← 关键 3：无背景
    },
    light = {
        -- 照射方向 (0,0,-1) ⟹ 指向光源的 L = -direction = (0,0,1) ≡ 探针面上的 N ≡ V
        direction = { 0.642788, 0.0, -0.766044 },  -- 绕 Y 转 40%（L 不再与 N 共线）
        color = { 1.0, 1.0, 1.0 },
        intensity = 12.0,
        ambient = { 0.0, 0.0, 0.0 },   -- ← 关键 4
    },
    spin = false,
    cube = false,
    exposure = 1.0,          -- ← 关键 5
    ao = { strength = 0.0 }, -- ← 关键 6
}
