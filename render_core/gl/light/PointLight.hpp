#pragma once


#include <glm/glm.hpp>
#include "Shader.hpp"
#include "Light.hpp"

namespace RGL {
namespace glcore {




class PointLight :public Light {
    float mK2;
    float mK1;
    float mKC;


	glm::vec3 lightColor;
    glm::vec3 ambientColor;



    float spotIntensity;
	public:
    PointLight(const glm::vec3& lightColor, const glm::vec3& ambientColor, float spotIntensity, float mK2, float mK1, float mKC);

	virtual void setShaderUniforms(UniformComponent& uniforms) const;

};
}
}  // namespace RGL
