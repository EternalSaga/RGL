#pragma once
#include <glm/fwd.hpp>
#include <glm/glm.hpp>
#include "Shader.hpp"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/random_generator.hpp>
namespace RGL {
namespace glcore {
class Light {
    boost::uuids::uuid uuid;
   public:
    Light() {
        uuid = boost::uuids::random_generator()();
    }
    virtual void setShaderUniforms(Shader* shader)=0;
    virtual ~Light() = default;
    boost::uuids::uuid getUUID();
};

class DirectionalLight : public Light {
    glm::vec3 lightDirection;
    glm::vec3 lightColor;
    glm::vec3 ambientColor;
    //镜面反射强度
    float specularIntensity;

   public:
    DirectionalLight(const glm::vec3& lightDirection, const glm::vec3& lightColor, const glm::vec3& ambientColor, float specularIntensity, float spotIntensity);
    ~DirectionalLight() = default;
    void setShaderUniforms(Shader* shader);
};
}  // namespace glcore

}  // namespace RGL
