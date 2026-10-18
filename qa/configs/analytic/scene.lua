-- 分析探针场景（批 12.2d）：**直接光之外的每一项都归零或关掉**。
--
-- 配套参考实现 = imgqa probe（把 shadeDirect + ACES + 硬件 sRGB 独立复刻一遍，作为
-- 交叉校验的第二实现；12.3b 起接替已删除的 tools/analytic_ref.py）。跑法：
--   ./build/tools/imgqa/imgqa probe   （--light 覆写会连引擎配置一起改，见 cmdProbe）
-- 实测（本机 RADV，2026-09-30，forward 时代）：**逐字节零误差**命中——退化光向 (0,0,-1) 下
-- 屏幕中心 = (113,56,249)，40° 倾斜光下 = (88,43,244)。
-- 2026-10-18 起本靶走 **deferred**（探针重立双锚，docs/图变体收敛_plan.md §6.1-1）：
-- 着色输入过 G-buffer 8bit 量化，期望字节与 forward 时代差在 ±1 LSB 量级（重立实测见
-- probe 输出与判据台账）。
-- 刻意用 **40° 倾斜光**而不是垂直光：垂直光下 N≡V≡L 使 D/F/G 退化成常数，
-- 判据虽好算但弱；倾斜后 NdotL=0.766、H≠N，BRDF 三项全展开，判别力更强。
-- 期望 L（指向光源）= -direction = (-0.642788, 0, 0.766044)。
--   environment.intensity = 0  → IBL 漫反射 + 镜面两路同时归零（shader 里统一乘；
--                                deferred 的 light.frag env 分支同样 ×env.intensity）。
--                                原「关键 3：skybox = false 无背景」双保险已随 skybox 标志
--                                删除（2026-10-18 刀②.2）——deferred 本就不读它；天穹由
--                                intensity=0 一起关（light.frag sky 分支门控 mipRange>0 &&
--                                intensity>0，§6.2），且屏幕中心本来就被墙挡满）
--   light.ambient = 0          → 平环境光项归零
--   ao.strength = 0            → lerp(1,ao,0) 位级 = 1.0（BIT-IDENTICAL 旁路；AO 施加住在
--                                tonemap pass）
--   exposure = 1.0             → ACES 前的线性倍率取中性
--   shadow.lua mode = "none"    → 阴影因子恒 1（批 N6b 起阴影键住 shadow.lua）
-- 剩下的只有：color = lightColor × NdotL × (albedo/π + GGX_spec)
return {
    entities = {
        { type = "model", file = "models/cornell_box.glb" },
    },
    environment = {
        hdr = "env/san_giuseppe_bridge_4k.hdr",
        hdr_size = 2048,
        intensity = 0.0,     -- ← 关键 2：IBL 两路统一归零（天穹同闸，见头注）
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
