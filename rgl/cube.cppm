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

/** 立方体 6 面 × 2 三角形 = 36 个索引（逆时针绕序，面归属见行内注释）。 */
export const std::vector<uint32_t> cubeIndices = {
	0, 1, 2, 2, 3, 0,  // 前
	1, 5, 6, 6, 2, 1,  // 右
	5, 4, 7, 7, 6, 5,  // 后
	4, 0, 3, 3, 7, 4,  // 左
	3, 2, 6, 6, 7, 3,  // 顶
	4, 5, 1, 1, 0, 4   // 底
};

}  // namespace RGL
