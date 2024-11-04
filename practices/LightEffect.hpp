
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
    std::unique_ptr<SceneManager> scene;
    std::shared_ptr<Shader> shader;
    std::shared_ptr<Camera> cam;

	std::unique_ptr<Texture> box_spMask;

   public:
    PhongSPMaskExec(std::shared_ptr<Camera> cam);
    void operator()() override;
};

class SpotLightExec : public Renderer {
	std::unique_ptr<SceneManager> scene;
    std::shared_ptr<Shader> spotLightShader;
    std::shared_ptr<Camera> cam;
    std::unique_ptr<Texture> box_spMask;

	public:
    SpotLightExec(std::shared_ptr<Camera> cam);
	void operator()() override;
};


}  // namespace practice
}  // namespace RGL
