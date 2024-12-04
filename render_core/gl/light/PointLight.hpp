#pragma once


#include <glm/glm.hpp>
#include "Shader.hpp"
#include "EnttRegistry.hpp"

namespace RGL {
namespace glcore {

struct PointLightComponent {
    float mK2;
    float mK1;
    float mKC;
};


class PointLight : public SingleReg {
    entt::entity entity;
	public:
    PointLight(const glm::vec3& lightColor, const glm::vec3& ambientColor, float specularIntensity, float mK2, float mK1, float mKC, glm::vec3 position);
};
}
}  // namespace RGL
