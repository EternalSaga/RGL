#pragma once
#include <glm/fwd.hpp>
#include <glm/glm.hpp>
#include "Shader.hpp"
namespace RGL {
namespace glcore {
class Light {
   public:
    Light() = default;
    virtual ~Light() = default;
};

class DirectionalLight : public Light {
    glm::vec3 lightDirection;
    glm::vec3 lightColor;
    glm::vec3 ambientColor;
    float specularIntensity;

   public:
    DirectionalLight(const glm::vec3& lightDirection, const glm::vec3& lightColor, const glm::vec3& ambientColor, float specularIntensity);
    ~DirectionalLight() = default;
    void setShaderUniforms(Shader* shader) const;
};
}  // namespace glcore

}  // namespace RGL