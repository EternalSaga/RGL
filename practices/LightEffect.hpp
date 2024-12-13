
#pragma once
#include "Camera.hpp"
#include "GLObj.hpp"
#include "GLTextures.hpp"

#include "Shader.hpp"

#include "api_types.hpp"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <memory>
#include "Light.hpp"

#include "Entity.hpp"
namespace RGL {
namespace practice {

using namespace glcore;


class PhongSPMaskExec : public Renderer {

    std::shared_ptr<Shader> shader;
    std::shared_ptr<Camera> cam;
	std::unique_ptr<Texture> box_spMask;
   public:
    PhongSPMaskExec(std::shared_ptr<Camera> cam);
    void operator()() override;
};

class PointLightExec : public Renderer ,public SingleReg{

    std::shared_ptr<Shader> pointlightShader;
    std::shared_ptr<Shader> whiteShader;
    std::shared_ptr<Camera> cam;
    std::unique_ptr<Texture> box_spMask;

	std::unique_ptr<CommonEntity> sphereEntity;
	public:
    PointLightExec(std::shared_ptr<Camera> cam);
	void operator()() override;
};


}  // namespace practice
}  // namespace RGL
