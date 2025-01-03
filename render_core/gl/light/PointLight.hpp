#pragma once


#include <glm/glm.hpp>
#include "Shader.hpp"
#include "Light.hpp"

namespace RGL {
namespace glcore {

	struct PointLightComponent {
    float mK2;
    float mK1;
    float mKC;
	};

	void updatePointLight();


}
}  // namespace RGL
