#include "PointLight.hpp"
#include "Light.hpp"
#include "Entity.hpp"
namespace RGL {
namespace glcore {


PointLight::PointLight(const glm::vec3& lightColor, const glm::vec3& ambientColor, float spotIntensity, float mK2, float mK1, float mKC): Light(lightColor, ambientColor, spotIntensity) {

	this->lightColor = lightColor;
	this->ambientColor = ambientColor;
	this->specularIntensity = spotIntensity;
	this->mK2 = mK2;
	this->mK1 = mK1;
	this->mKC = mKC;


}
void PointLight::setShaderUniforms(UniformComponent& uniforms) const {
	
    uniforms["lightColor"] = lightColor;
    uniforms["ambient"] = ambientColor;
    uniforms["spotIntensity"] = this->specularIntensity;

    uniforms["k1"] = mK1;
    uniforms["k2"] = mK2;
    uniforms["kc"] = mKC;

}

}  // namespace glcore
}  // namespace RGL
