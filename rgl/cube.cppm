module;
export module cube;

import rlstd;

namespace RGL {

/**
 * 内建几何顶点（position + color，24 字节；无法线/UV，供 builtin_geometry 上传
 * cube 等程序化几何资产使用）。
 */
export struct Vertex {
	float position[3];
	float color[3];
};

/** 立方体 8 角顶点（±0.5 范围），各顶点带可区分 RGB 色（R/G/B/Y/M/C/W/K）便于识别面。 */
export const std::vector<Vertex> cubeVertices = {
	{{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
	{{0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
	{{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},
	{{-0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 0.0f}},
	{{-0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 1.0f}},
	{{0.5f, -0.5f, 0.5f}, {0.0f, 1.0f, 1.0f}},
	{{0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}},
	{{-0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 0.0f}}};

/**
 * 立方体 6 面 × 2 三角形 = 36 个索引（面归属见行内注释）。
 *
 * ⚠️ **绕序是"从外面看顺时针"**——法线朝内。旧注释写"逆时针绕序"是错的。
 * 实测（批 12.1 单测 tests/test_cube_geometry.cppm 钉住，12/12 三角形）：
 * `dot(cross(B−A, C−A), 三角形重心) ≡ −0.5`，即叉积指向立方体**中心**。
 * 在 GL/Vulkan 默认 `frontFace = eCounterClockwise` 下，这意味着这些三角形
 * **从外面看是背面**。
 *
 * 后果（当前无害，但有前提）：`state_kit` 的 opaque 段 `cullMode = eNone`，
 * 绕序不影响结果；transparent 段是 `eBack` ⟹ 剔掉的恰是**朝外**的面，保留
 * 内部面 —— 封闭网格轮廓仍正确、单层混合的目标（见 state_kit 注释）也仍达成，
 * 但**带纹理的透明体会显示内侧**（看到的是远侧内壁）。真要修就是翻转索引顺序，
 * 那会改动 raster 结果与截图基线，属独立决策，不在"改注释"范围内。
 */
export const std::vector<uint32_t> cubeIndices = {
	0, 1, 2, 2, 3, 0,  // 前
	1, 5, 6, 6, 2, 1,  // 右
	5, 4, 7, 7, 6, 5,  // 后
	4, 0, 3, 3, 7, 4,  // 左
	3, 2, 6, 6, 7, 3,  // 顶
	4, 5, 1, 1, 0, 4   // 底
};

}  // namespace RGL
