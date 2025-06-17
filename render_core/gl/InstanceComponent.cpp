#include "InstanceComponent.hpp"
#include <random>
#include <glm/gtc/matrix_transform.hpp>
#include "GLCheckError.hpp"
#include "Helpers.hpp"

namespace RGL {
namespace glcore {

InstanceComponent InstanceHelper::makeRandomInstanceComponent(size_t numInstances) {
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> posDist(-50.0f, 50.0f);  // X,Z坐标分布范围
    std::uniform_real_distribution<float> scaleDist(0.5f, 1.5f);   // 缩放范围
    std::uniform_real_distribution<float> rotDist(0.0f, 360.0f);   // 旋转范围
    InstanceComponent instanceComponent;

    instanceComponent.instanceMatrices.resize(numInstances);
    for (size_t i = 0; i < numInstances; ++i) {
	glm::mat4 model = glm::mat4(1.0f);
	// 1. 平移
	glm::vec3 position(posDist(rng), 0.0f, posDist(rng));
	model = glm::translate(model, position);

	// 2. 旋转 (绕Y轴)
	model = glm::rotate(model, glm::radians(rotDist(rng)), glm::vec3(0.0f, 1.0f, 0.0f));

	// 3. 缩放
	float scale = scaleDist(rng);
	model = glm::scale(model, glm::vec3(scale));
      instanceComponent.instanceMatrices[i] = model;
    }
    auto instanceVBO = std::make_unique<glcore::VBO>();
    glCall(glNamedBufferData,*instanceVBO,sizeof(glm::mat4) * numInstances,instanceComponent.instanceMatrices.data(),GL_STATIC_DRAW);
    instanceComponent.instanceVBO = std::move(instanceVBO);
    return instanceComponent;
}
}  // namespace glcore
}  // namespace RGL