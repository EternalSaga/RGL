-- 分析探针机位：从 +Z 正视 back_wall（法线 +Z，位于 z=-5）。
-- 视线 forward = normalize(target-position) = (0,0,-1) **精确**，
-- 故屏幕中心像素的 V = (0,0,1)、入射点 P = (0,5,-5)、N = (0,0,1)。
-- 三者共线是刻意设计：闭式解里 D/F/G 全部退化到常数，任何 1/π 约定或
-- Fresnel 项的偏差都会直接暴露在数值上。
return {
    mode = "fps",
    position = { 0.0, 5.0, 15.0 },
    target = { 0.0, 5.0, -5.0 },
    fovDegrees = 45.0,
    near = 0.1,
    far = 8000.0,
    trackball = { sensitivity = 0.005, zoom = 0.03 },
    fps = { speed = 0.4, sensitivity = 0.003 },
}
