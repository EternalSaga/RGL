#pragma once
#include "GLObj.hpp"
#include <memory>

#include <glm/glm.hpp>
namespace RGL {
namespace glcore {

struct InstancedRenderingComponent {
    std::unique_ptr<VBO> instanceVBO; // 指向GPU数据的句柄
    size_t instanceCount;             // 实例数量，用于glDraw*Instanced
};


namespace InstanceFactory {
    std::vector<glm::mat4> generateRandomTransforms(
    size_t numInstances, 
    const glm::vec3& minPos, 
    const glm::vec3& maxPos,
    float minScale, 
    float maxScale);
   InstancedRenderingComponent createComponent(const std::vector<glm::mat4>& instanceMatrices);
}

}
}
