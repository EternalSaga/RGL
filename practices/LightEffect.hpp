
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

class GlobalLight : public Renderer {
    std::shared_ptr<Shader> shader;
    std::unique_ptr<Texture> checkboarder;

    std::shared_ptr<Camera> cam;
    glm::mat4 modelMatrix{1.0f};
    std::unique_ptr<VAO> vao;
    size_t mvertCount;
    size_t midxOffset;

    DirectionalLight light;

   public:
    GlobalLight(std::shared_ptr<Camera> cam);
    void operator()() override;
};

class TestEntity : public Renderer {
    
    std::unique_ptr<SceneManager> scene;
    std::shared_ptr<Shader> shader;
    std::shared_ptr<Camera> cam;
    std::unique_ptr<Texture> checkboarder_doraemon;
    
   public:
    TestEntity(std::shared_ptr<Camera> cam);
    void operator()() override;
};

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
