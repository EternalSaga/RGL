-- nm_rig 机位：离地 1 m、**俯角 25°**、垂直 fov 45°。三个数与 tools/qa_scenes.py 的
-- NM_CAM_H / NM_PITCH_DEG / NM_FOV_V 以及窄带 env 的带子仰角**同源**（env 生成脚本
-- env_band_hdr.py 已随刀②.2 归档，git history 可查）。
--
-- ## 为什么是 25° 而不是 env_band 的掠射 3°
--
-- 俯角 25° + fov_v 45° ⟹ 画面视线仰角范围 [−2.53°, −47.47°]（闭式见
-- manifest.extra.probe.viewElevDegTop/Bottom），**地平线在画外、全画幅皆地面**。三条硬收益：
--   ① deferred 路线可用。deferred 的天穹合成在 light.frag.slang:193-203 内联，既**不乘
--      env.intensity** 也**不读 skybox 标志** ⟹ 画幅里只要有一天穹像素，"env 关掉归零"
--      那一行实验就永远不归零（env_band 那一轮正是为此被迫锁 forward）。画幅里没有
--      天穹像素 = 那条路线不对称根本不被触发。
--   ② 可用像素从 env_band 的 49 行涨到整窗（speck 是逐像素计数，口径直接翻 26 倍）。
--   ③ 不需要掩膜：单材质整块地面 + 平坦面 N=+Y ⟹ amp=0 时反射仰角只依赖行号
--      ⟹ **画面逐行不变**，取整行中位就是闭式剖面。
--
-- ## 为什么俯角必须 == env 带子仰角
--
-- 平面 N=+Y 的反射仰角 = −视线仰角 ⟹ "仰角 θ_b 的水平带"在地面上的镜像 = 恒定视线
-- 仰角 −θ_b 的一条水平线。相机俯角取 α = θ_b ⟹ 镜像线**正好落在主点行**（1280×720 的
-- 第 359.5 行），闭式可预测（manifest.extra.probe.mirrorRow）。
--
-- target 是闭式写出来的，不是手调的：
--   target = position + 10·(0, −sin25°, −cos25°) = (0, 1 − 4.2261826174, −9.0630778704)
-- 视线 forward 的仰角因此**精确** = −25°（不是"看起来差不多"），镜像线行号才敢当已知量用。
--
-- near/far：地面 ±100 m，镜像线在 2.14 m 处（h/tan25°），顶行打到 22.9 m。
-- near=0.1 / far=1000 包住全部；深度精度不构成风险（无共面重叠）。
return {
    mode = "fps",
    position = { 0.0, 1.0, 0.0 },
    target = { 0.0, -3.2261826174, -9.0630778704 },
    fovDegrees = 45.0,
    near = 0.1,
    far = 1000.0,
    trackball = { sensitivity = 0.005, zoom = 0.03 },
    fps = { speed = 0.4, sensitivity = 0.003 },
}
