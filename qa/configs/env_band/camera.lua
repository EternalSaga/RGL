-- 掠射机位：离地 1 m、俯角 3.0°、垂直 fov 45°。三个数与 tools/qa_scenes.py 的
-- ENV_BAND_H / ENV_BAND_VIEW_ELEV / ENV_BAND_FOV_V 以及 env_band_hdr.py 的带子仰角**同源**。
--
-- 为什么俯角必须 == env 带子仰角：平面 N=+Y 的反射方向仰角 = −视线仰角，于是"仰角 θ_b 的
-- 水平带"在地面上的镜像 = 恒定视线仰角 −θ_b 的一条水平线；相机俯角取 α = θ_b ⟹ 镜像线
-- 正好落在主点行（1280×720 的第 360 行），闭式可预测（manifest.extra.probe.mirrorRow）。
--
-- target 是闭式写出来的，不是手调的：
--   target = position + 10·(0, −sin3°, −cos3°) = (0, 1 − 0.5233595624, −9.9862953475)
-- 视线 forward 的仰角因此**精确** = −3°（不是"看起来差不多"），镜像线行号才敢当已知量用。
--
-- near/far：地面铺到 z=−300（远端落在黑色天穹里，边缘两侧都黑 = 不可见，见场景 honesty），
-- far=1000 足够包住；near=0.1 与 analytic 探针同档。地面是单平面、无共面重叠，
-- 深度精度在这组参数下不构成风险（镜像线在 19.08 m 处，远离 near）。
return {
    mode = "fps",
    position = { 0.0, 1.0, 0.0 },
    target = { 0.0, 0.4766404376, -9.9862953475 },
    fovDegrees = 45.0,
    near = 0.1,
    far = 1000.0,
    trackball = { sensitivity = 0.005, zoom = 0.03 },
    fps = { speed = 0.4, sensitivity = 0.003 },
}
