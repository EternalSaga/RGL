#pragma once
#include "GLObj.hpp"
#include <memory>

#include <glm/glm.hpp>
namespace RGL {
namespace glcore {

struct InstanceComponent {
    std::unique_ptr<VBO> instanceVBO;
    std::vector<glm::mat4> instanceMatrices;
};


namespace InstanceHelper {

    InstanceComponent makeRandomInstanceComponent(size_t numInstances);
}

}
}
