#include "InstanceComponent.hpp"
#include <random>
#include <glm/gtc/matrix_transform.hpp>
#include "GLCheckError.hpp"
#include "Helpers.hpp"

namespace RGL {
namespace glcore {

namespace InstanceFactory {

// 职责一：生成随机变换矩阵 (现在参数更灵活)


std::vector<glm::mat4> generateRandomTransforms(
    size_t numInstances, 
    const glm::vec3& minPos, 
    const glm::vec3& maxPos,
    float minScale, 
    float maxScale) 
{
    std::vector<glm::mat4> matrices;
    matrices.reserve(numInstances); // 提前预留空间，避免循环中重分配

    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> xDist(minPos.x, maxPos.x);
    std::uniform_real_distribution<float> yDist(minPos.y, maxPos.y);
    std::uniform_real_distribution<float> zDist(minPos.z, maxPos.z);
    std::uniform_real_distribution<float> scaleDist(minScale, maxScale);
    std::uniform_real_distribution<float> rotDist(0.0f, 360.0f);

    for (size_t i = 0; i < numInstances; ++i) {
        glm::mat4 model = glm::mat4(1.0f);
        glm::vec3 position(xDist(rng), yDist(rng), zDist(rng));
        model = glm::translate(model, position);
        model = glm::rotate(model, glm::radians(rotDist(rng)), glm::vec3(0.0f, 1.0f, 0.0f));
        float scale = scaleDist(rng);
        model = glm::scale(model, glm::vec3(scale));
        matrices.push_back(model);
    }
    return matrices;
}

// 职责二：根据给定的矩阵数据，创建GPU组件
InstancedRenderingComponent createComponent(const std::vector<glm::mat4>& instanceMatrices) {
    if (instanceMatrices.empty()) {
        // 处理空数据的情况
        return {nullptr, 0};
    }

    auto instanceVBO = std::make_unique<glcore::VBO>();
    glCall(glNamedBufferData, 
           *instanceVBO, 
           instanceMatrices.size() * sizeof(glm::mat4), 
           instanceMatrices.data(), 
           GL_STATIC_DRAW);

    return { std::move(instanceVBO), instanceMatrices.size() };
}

} // namespace InstanceFactory

}  // namespace glcore
}  // namespace RGL