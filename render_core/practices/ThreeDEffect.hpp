
#pragma once
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

namespace RGL {
namespace practice {

using namespace glcore;
class BasicTransform : public Renderer {
  std::unique_ptr<VBO> vbo;
  std::unique_ptr<VAO> vao;
  std::unique_ptr<Shader> shader;
  std::unique_ptr<EBO> ebo;
  std::unique_ptr<Texture> grass_land_noise;
  glm::mat4 trans;

  inline void doTransform() {
    trans = glm::rotate(trans, glm::radians(1.0f), glm::vec3(0, 0, 1));
  }

public:
  BasicTransform();
  void operator()() override;
};

} // namespace practice
} // namespace RGL
