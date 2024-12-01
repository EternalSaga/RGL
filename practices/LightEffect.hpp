
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
    glcore::ShaderManager* shaderManager;
   public:
    PhongSPMaskExec(std::shared_ptr<Camera> cam);
    void operator()() override;
};

class PointLightExec : public Renderer {
	std::unique_ptr<SceneManager> scene;
    std::shared_ptr<Shader> pointLightShader;
    std::shared_ptr<Shader> whiteShader;
    std::shared_ptr<Camera> cam;
    std::unique_ptr<Texture> box_spMask;
    
	public:
    PointLightExec(std::shared_ptr<Camera> cam);
	void operator()() override;
};


}  // namespace practice
}  // namespace RGL
