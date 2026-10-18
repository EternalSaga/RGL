-- blend_sort 机位（透明排序靶）：**必须与 tools/qa_scenes.py 的 BLEND_SORT_CAM 逐字相同**。
--
-- 判据区（far-only / overlap / near-only / bg-only）的像素坐标是 harness 按
-- 「相机在 (0,0,6) 沿 −Z 正视、fov_v=45°」解析算出来的；这里改一个数，四个采样点
-- 就量到别的几何上，而闭式预测照旧"自洽"——那是一条会给出假绿的错。
-- 所以 manifest 里也记了同一组相机值，harness 先对账再算投影（不一致即拒跑）。
--
-- 视线 forward = normalize(target − position) = (0,0,−1) **精确**，无俯角无偏航
-- ⟹ 世界 x/y 到屏幕 px 是纯线性映射（不必建 view 矩阵），闭式里少一个误差源。
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
