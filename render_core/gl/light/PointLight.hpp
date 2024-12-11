#pragma once


#include <glm/glm.hpp>
#include "Shader.hpp"
#include "EnttRegistry.hpp"

namespace RGL {
namespace glcore {




class PointLight {
    float mK2;
    float mK1;
    float mKC;
	public:
    PointLight(const glm::vec3& lightColor, const glm::vec3& ambientColor, float specularIntensity, float mK2, float mK1, float mKC, glm::vec3 position);
};
}
}  // namespace RGL
