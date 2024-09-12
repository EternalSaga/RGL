
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
namespace RGL {
namespace practice {

using namespace glcore;

class GlobalLight : public Renderer {
    std::unique_ptr<Shader> shader;
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

}  // namespace practice
}  // namespace RGL
