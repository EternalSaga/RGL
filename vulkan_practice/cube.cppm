module;
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>
export module cube;
export struct Vertex {
    float position[3];
    float color[3];
};

// 2. CPU 端硬编码一个立方体的 8 个顶点
export const std::vector<Vertex> cubeVertices = {
    {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{ 0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    {{ 0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f,  0.5f, -0.5f}, {1.0f, 1.0f, 0.0f}},
    {{-0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 1.0f}},
    {{ 0.5f, -0.5f,  0.5f}, {0.0f, 1.0f, 1.0f}},
    {{ 0.5f,  0.5f,  0.5f}, {1.0f, 1.0f, 1.0f}},
    {{-0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 0.0f}}
};


export const std::vector<uint32_t> cubeIndices = {
    0, 1, 2,  2, 3, 0, // 前
    1, 5, 6,  6, 2, 1, // 右
    5, 4, 7,  7, 6, 5, // 后
    4, 0, 3,  3, 7, 4, // 左
    3, 2, 6,  6, 7, 3, // 顶
    4, 5, 1,  1, 0, 4  // 底
};

export struct PushConstants {
    glm::mat4 mvp;
    uint64_t verticesHandle;
    uint64_t indicesHandle;
};