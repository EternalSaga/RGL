
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
    glm::vec3 lightColor{1.0f, 0.9f, 0.9f};
    glm::vec3 globalLightDirection{1.0f, 1.0f, -1.0f};
    glm::vec3 ambientLightColor{0.1f, 0.1f, 0.1f};
    float specularIntensity = 0.5f;
   public:
    GlobalLight(std::shared_ptr<Camera> cam);
    void operator()() override;
};

}  // namespace practice
}  // namespace RGL
