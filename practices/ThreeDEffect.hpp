
#pragma once
#include "Camera.hpp"
#include "GLObj.hpp"
#include "GLTextures.hpp"
#include "Helpers.hpp"
#include "Shader.hpp"
#include "StaticEffect.hpp"
#include "VertexDescriptor.hpp"
#include "api_types.hpp"
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <memory>
#include <vector>
#include "Geometry.hpp"
namespace RGL {
namespace practice {

using namespace glcore;
class BasicTransform : public Renderer {
  std::unique_ptr<VBO> vbo;
  std::unique_ptr<VAO> vao;
  std::unique_ptr<Shader> shader;
  std::unique_ptr<Texture> grass_land_noise;
  glm::mat4 trans;

  inline void doTransform() {
    trans = glm::rotate(trans, glm::radians(1.0f), glm::vec3(0, 0, 1));
  }

public:
  BasicTransform();
  void operator()() override;
};

class CameraTransform : public Renderer {
  std::unique_ptr<VBO> vbo;
  std::unique_ptr<VAO> vao;
  std::unique_ptr<Shader> shader;
  std::unique_ptr<Texture> doraemon;
  std::shared_ptr<Camera> cam;
  glm::mat4 transform{1.0f};
  public:
    CameraTransform(std::shared_ptr<Camera> cam);
    void operator()() override;
};


class DepthTestExecise : public Renderer {
  std::unique_ptr<VBO> vbo;
  std::unique_ptr<VAO> vao;
  std::unique_ptr<Shader> shader;
  std::unique_ptr<Texture> doraemon_nobita;

  std::shared_ptr<Camera> cam;
  glm::mat4 transform{1.0f};
  public:
    DepthTestExecise(std::shared_ptr<Camera> cam);
    void operator()() override;
};

class DrawCube : public Renderer
{
    
    std::unique_ptr<Shader> shader;
    std::unique_ptr<Texture> checkboarder;

    std::shared_ptr<Camera> cam;
    glm::mat4 transform{1.0f};
    std::unique_ptr<VAO> vao;
    size_t mvertCount;
    size_t midxOffset;
  public:
    DrawCube(std::shared_ptr<Camera> cam);
    void operator()() override;
};

} // namespace practice
} // namespace RGL
