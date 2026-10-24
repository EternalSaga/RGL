-- blend_lit 机位（透明段 lit 覆盖靶）：**必须与 tools/qa_scenes.py 的 BLEND_LIT_CAM 逐字相同**。
--
-- 三个 pane 的屏幕区域是 harness 按「相机在 (0,0,6) 看向原点、fov_v=45°」解析算出来的；
-- 这里改一个数，采样框就量到别的几何上，而"双臂像素不同"这条判据照旧会绿 —— 那是一条
-- 会给出假绿的错。所以 manifest 的 blendLit.camera 里也记了同一组值，harness 先对账
-- 再算投影（不一致即拒跑，不静默按其中一份跑）。
--
-- 视线 forward = normalize(target − position) = (0,0,−1) **精确**，无俯角无偏航
-- ⟹ 世界 x/y 到屏幕 px 是纯线性映射（不必建 view 矩阵），投影算子里少一个误差源。
-- 引擎侧口径已核：camera.cppm 注「yaw=0, pitch=0 → forward=(0,0,−1)」，与本文件一致。
return {
    mode = "fps",
    position = { 0.0, 0.0, 6.0 },
    target = { 0.0, 0.0, 0.0 },
    fovDegrees = 45.0,
    near = 0.1,
    far = 100.0,
    trackball = { sensitivity = 0.005, zoom = 0.03 },
    fps = { speed = 0.4, sensitivity = 0.003 },
}
