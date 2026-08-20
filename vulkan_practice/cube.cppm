module;
#include <cstdint>
export module cube;

import rlstd;

namespace RGL {

export struct Vertex {
	float position[3];
	float color[3];
};

export const std::vector<Vertex> cubeVertices = {
	{{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
	{{0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
	{{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},
	{{-0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 0.0f}},
	{{-0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 1.0f}},
	{{0.5f, -0.5f, 0.5f}, {0.0f, 1.0f, 1.0f}},
	{{0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}},
	{{-0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 0.0f}}};

export const std::vector<uint32_t> cubeIndices = {
	0, 1, 2, 2, 3, 0,  // 前
	1, 5, 6, 6, 2, 1,  // 右
	5, 4, 7, 7, 6, 5,  // 后
	4, 0, 3, 3, 7, 4,  // 左
	3, 2, 6, 6, 7, 3,  // 顶
	4, 5, 1, 1, 0, 4   // 底
};

}  // namespace RGL
