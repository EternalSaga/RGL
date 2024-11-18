#pragma once
#include <glm/fwd.hpp>
#include <glm/glm.hpp>
#include "Shader.hpp"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/random_generator.hpp>
#include "EnttRegistry.hpp"
namespace RGL {
namespace glcore {

struct DirectionalLightComponent {
    glm::vec3 lightDirection;
    glm::vec3 lightColor;
    glm::vec3 ambientColor;
    // 镜面反射强度
    float specularIntensity;
};


class DirectionalLight : public SingleReg {
    entt::entity entity;

   public:
    DirectionalLight(const glm::vec3& lightDirection, const glm::vec3& lightColor, const glm::vec3& ambientColor, float specularIntensity, float spotIntensity, std::shared_ptr<Shader> shader);
    ~DirectionalLight() = default;

};

class PointLight {
    float mK2;
    float mK1;
    float mKC;
};

}  // namespace glcore

}  // namespace RGL
