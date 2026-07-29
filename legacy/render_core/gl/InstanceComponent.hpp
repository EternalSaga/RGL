#pragma once
#include <vector>



#include <glm/glm.hpp>

namespace RGL {
namespace glcore {

namespace InstanceFactory {

std::vector<glm::mat4> generateRandomTransforms(
    size_t numInstances,
    const glm::vec3& minPos,
    const glm::vec3& maxPos,
    float minScale,
    float maxScale);

}
}  // namespace glcore
}  // namespace RGL