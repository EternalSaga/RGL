#pragma once
#include "GLObj.hpp"
#include "GLTextures.hpp"
#include "Helpers.hpp"
#include "Shader.hpp"
#include "VertexDescriptor.hpp"
#include "api_types.hpp"
#include <glad/glad.h>
#include <memory>
#include <vector>
#include "Shader.hpp"
#include "GLObj.hpp"
#include "GLTextures.hpp"
namespace RGL {
namespace glcore {
class EBOExec : public Renderer {
  std::unique_ptr<EBO> ebo;
  std::unique_ptr<VAO> vao;
  std::unique_ptr<VBO> vbo;
  std::unique_ptr<Shader> shader;

public:
  EBOExec();
  virtual ~EBOExec() = default;
  void operator()() override;

private:
};


class ColorfulTriangle : public Renderer {
  std::unique_ptr<EBO> ebo;
  std::unique_ptr<VAO> vao;
  std::unique_ptr<VBO> vbo;
  std::unique_ptr<Shader> shader;

public:
  ColorfulTriangle();
  virtual ~ColorfulTriangle() = default;
  void operator()() override;
};

class InterLeavedBuffer : public Renderer {
  std::unique_ptr<VBO> posColorVbo;
  std::unique_ptr<VAO> vao;
  std::unique_ptr<Shader> shader;

public:
  InterLeavedBuffer();
  void operator()() override;
  virtual ~InterLeavedBuffer() = default;
};

 class SetColorByUniform : public Renderer {
  std::unique_ptr<VBO> vbo;
  std::unique_ptr<VAO> vao;
  std::unique_ptr<Shader> shader;

public:
  SetColorByUniform();

  void operator()() override;
};

class TexturePractice : public Renderer {
  std::unique_ptr<VBO> vbo;
  std::unique_ptr<VAO> vao;
  std::unique_ptr<Shader> shader;
  std::unique_ptr<EBO> ebo;
  std::unique_ptr<Texture> texture;

public:
  TexturePractice();

  void operator()() override;
  virtual ~TexturePractice() = default;
};


class RectangleTexture : public Renderer {
  std::unique_ptr<VBO> vbo;
  std::unique_ptr<VAO> vao;
  std::unique_ptr<Shader> shader;
  std::unique_ptr<EBO> ebo;
  std::unique_ptr<Texture> texture;

public:
  RectangleTexture();
  void operator()() override;
};

class Blend : public Renderer {
  std::unique_ptr<VBO> vbo;
  std::unique_ptr<VAO> vao;
  std::unique_ptr<Shader> shader;
  std::unique_ptr<EBO> ebo;
  std::unique_ptr<Texture> grass_land_noise;

public:
  Blend();
  void operator()() override;
};
} // namespace glcore
} // namespace RGL
