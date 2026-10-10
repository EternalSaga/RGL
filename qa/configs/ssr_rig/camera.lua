-- ssr_rig 机位：**俯角必须精确等于 tools/qa_scenes.py 的 SSR_PITCH_DEG（8°）**。
--
-- 闭式倒影凸包（manifest 的 probe.reflectionHullPx）是在"相机 = (0, 1.6, 0)、俯角 8°、
-- fov 45°"下算的；俯角差 0.1° → 判据区在画面里挪 ~10 行（pxPerDegElevation = 720/45 = 16），
-- 判据就会量到错误的像素上。target 取"沿视线方向 10 m 处"，引擎按
-- `pitch = asin(normalize(target − position).y)` 反解（camera_config_impl 的 readCameraVec3s），
-- 所以这里写的是**精确值**而不是四舍五入的近似。
--   target.y = 1.6 − 10·sin(8°) = 0.208269
--   target.z = −10·cos(8°)      = −9.902681
return {
    mode = "fps",
    position = { 0.0, 1.6, 0.0 },
    target = { 0.0, 0.208269, -9.902681 },
    fovDegrees = 45.0,
    near = 0.1,
    far = 8000.0,
    trackball = { sensitivity = 0.005, zoom = 0.03 },
    fps = { speed = 0.4, sensitivity = 0.003 },
}
