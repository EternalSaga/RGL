
#include "StaticEffect.hpp"

namespace RGL {
namespace glcore {
using namespace io;
const std::vector<float> positions{-0.5f, -0.5f, 0.0f, 0.5f, -0.5f,
                                   0.0f,  0.5f,  0.5f, 0.0f};
const std::vector<float> colors{1, 0, 0, 0, 1, 0, 0, 0, 1};
const std::vector<float> pos_col_interleaved{-0.5f, -0.5f, 0.0f, 1, 0, 0,
                                             0.5f,  -0.5f, 0.0f, 0, 1, 0,
                                             0.0f,  0.5f,  0.0f, 0, 0, 1};

const std::vector<float> pos_uv_interleaved{
    -0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 0.5f, -0.5f, 0.0f,
    1.0f,  0.0f,  0.0f, 0.5f, 0.0f, 0.5f, 1.0f,
};

const std::vector<float> positions4{
    -0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 0.5f, 0.5f, 0.0f,
};
const std::vector<float> rectangle_pos_uv = {
    -0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 0.5f, -0.5f, 0.0f, 2.0f, 0.0f,
    -0.5f, 0.5f,  0.0f, 0.0f, 2.0f, 0.5f, 0.5f,  0.0f, 2.0f, 2.0f,
};
const std::vector<GLint> rectangle_indeces = {0, 1, 2, 2, 1, 3};
const std::vector<float> rectangle_pos_uv2 = {
    -0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 0.5f, -0.5f, 0.0f, 1.0f, 0.0f,
    -0.5f, 0.5f,  0.0f, 0.0f, 1.0f, 0.5f, 0.5f,  0.0f, 1.0f, 1.0f,
};
const std::vector<GLint> eboIndices{0, 1, 2, 2, 1, 3};
const std::vector<GLint> eboSimple{0, 1, 2};
EBOExec::EBOExec() {
  vbo = std::make_unique<VBO>();
  vbo->setData(positions4);
  ebo = std::make_unique<EBO>();
  ebo->setData(eboIndices);
  vao = std::make_unique<VAO>();

  ShaderSrcs shaders = {
      {SHADER_TYPE::VERTEX, {"shaders\\beginner\\shader.vert"}},
      {SHADER_TYPE::FRAGMENT, {"shaders\\beginner\\shader.frag"}}};

  shader = std::make_unique<Shader>(shaders);
  vao->setShaderProgram(*shader);
  vao->set(*vbo, 3, "aPos");
  vao->addEBO(*ebo);
}

void EBOExec::operator()() {
  shader->useProgram();
  glCall(glBindVertexArray, *vao);
  // glCall(glDrawElements, GL_TRIANGLES, 6, GL_UNSIGNED_INT,
  // 0);

  glCall(glDrawElements, GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

  glCall(glBindVertexArray, 0);
}
ColorfulTriangle::ColorfulTriangle() {
  vbo = std::make_unique<VBO>();
  vbo->setData(pos_col_interleaved);
  ebo = std::make_unique<EBO>();
  ebo->setData(eboSimple);
  vao = std::make_unique<VAO>();

  vao->addEBO(*ebo);

  ShaderSrcs shaders = {
      {SHADER_TYPE::VERTEX, {"shaders\\beginner\\outColor.vert"}},
      {SHADER_TYPE::FRAGMENT, {"shaders\\beginner\\colorful.frag"}}};
  shader = std::make_unique<Shader>(shaders);

  vao->setShaderProgram(*shader);

  vao->set(*vbo, 3, 6, 0, "inPos");

  vao->set(*vbo, 3, 6, 3, "inColor");
}
void ColorfulTriangle::operator()() {
  shader->useProgram();
  glCall(glBindVertexArray, *vao);

  glCall(glDrawElements, GL_TRIANGLES, 3, GL_UNSIGNED_INT, nullptr);

  glBindVertexArray(0);
}
InterLeavedBuffer::InterLeavedBuffer() : Renderer() {
  posColorVbo = std::make_unique<VBO>();
  posColorVbo->setData(pos_col_interleaved);

  vao = std::make_unique<VAO>();
  ShaderSrcs shaders = {
      {SHADER_TYPE::VERTEX, {"shaders\\beginner\\shader.vert"}},
      {SHADER_TYPE::FRAGMENT, {"shaders\\beginner\\shader.frag"}}};

  shader = std::make_unique<Shader>(shaders);

  vao->setShaderProgram(*shader);

  vao->set(*posColorVbo, 3, 6, 0, "aPos");

  vao->set(*posColorVbo, 3, 6, 3, "inColor");
}
void InterLeavedBuffer::operator()() {
  shader->useProgram();
  glCall(glBindVertexArray, *vao);
  glCall(glDrawArrays, GL_TRIANGLES, 0, 3);
}
SetColorByUniform::SetColorByUniform() {
  vbo = std::make_unique<VBO>();
  vbo->setData(positions);

  vao = std::make_unique<VAO>();
  ShaderSrcs shaders = {
      {SHADER_TYPE::VERTEX, {"shaders\\beginner\\UniformTriangle.vert"}},
      {SHADER_TYPE::FRAGMENT, {"shaders\\beginner\\UniformTriangle.frag"}}};
  shader = std::make_unique<Shader>(shaders);
  vao->setShaderProgram(*shader);

  vao->set(*vbo, 3, 3, 0, "inPos");
}
void SetColorByUniform::operator()() {
  shader->useProgram();
  shader->setUniform("ucolor", 1.0f, 1.f, 0.f);
  glCall(glBindVertexArray, *vao);
  glCall(glDrawArrays, GL_TRIANGLES, 0, 3);
}
TexturePractice::TexturePractice() {
  vbo = std::make_unique<VBO>();
  vbo->setData(pos_uv_interleaved);

  vao = std::make_unique<VAO>();
  ShaderSrcs shaders = {
      {SHADER_TYPE::VERTEX, {"shaders\\beginner\\uvshader.vert"}},
      {SHADER_TYPE::FRAGMENT, {"shaders\\beginner\\uvshader.frag"}}};
  shader = std::make_unique<Shader>(shaders);

  texture = std::make_unique<Texture>();
  {
    io::LoadedImg img("./assest/001.jpg");
      texture->set(img, "001", false);
  }

  vao->setShaderProgram(*shader);

  vao->set(*vbo, 3, 5, 0, "inPos");
  vao->set(*vbo, 2, 5, 3, "inUV");

  ebo = std::make_unique<EBO>();

  ebo->setData(eboSimple);

  vao->addEBO(*ebo);
}
void TexturePractice::operator()() {
  shader->useProgram();

  shader->setUniform("sampler", texture->useTexture("001"));
  glCall(glBindVertexArray, *vao);
  glCall(glDrawElements, GL_TRIANGLES, 3, GL_UNSIGNED_INT, nullptr);
}
RectangleTexture::RectangleTexture() {
  vbo = std::make_unique<VBO>();
  vbo->setData(rectangle_pos_uv);

  vao = std::make_unique<VAO>();
  ShaderSrcs shaders = {
      {SHADER_TYPE::VERTEX, {"shaders\\beginner\\uvshader.vert"}},
      {SHADER_TYPE::FRAGMENT, {"shaders\\beginner\\uvshader.frag"}}};
  shader = std::make_unique<Shader>(shaders);

  texture = std::make_unique<Texture>();
  {
    io::LoadedImg img("./assest/001.jpg");
      texture->set(img, "001", false);
  }

  vao->setShaderProgram(*shader);

  vao->set(*vbo, 3, 5, 0, "inPos");
  vao->set(*vbo, 2, 5, 3, "inUV");

  ebo = std::make_unique<EBO>();

  ebo->setData(rectangle_indeces);

  vao->addEBO(*ebo);
}
void RectangleTexture::operator()() {
  shader->useProgram();

  shader->setUniform("sampler", texture->useTexture("001"));
  glCall(glBindVertexArray, *vao);
  glCall(glDrawElements, GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
}
Blend::Blend() {
  vbo = std::make_unique<VBO>();
  vbo->setData(rectangle_pos_uv2);
  vao = std::make_unique<VAO>();
  ShaderSrcs shaders = {
      {SHADER_TYPE::VERTEX, {"shaders\\beginner\\uvshader.vert"}},
      {SHADER_TYPE::FRAGMENT, {"shaders\\beginner\\Blend.frag"}}};
  shader = std::make_unique<Shader>(shaders);
  grass_land_noise = std::make_unique<Texture>(3);
  {
    io::LoadedImg grassImg("./assest/grass.jpg");
    grass_land_noise->set(grassImg, "grass", true);
  }
  {
    io::LoadedImg landImg("./assest/land.jpg");
      grass_land_noise->set(landImg, "land", true);
  }
  {
    io::LoadedImg noiseImg("./assest/noise.jpg");
      grass_land_noise->set(noiseImg, "noise", true);
  }
  vao->setShaderProgram(*shader);
  auto desc = hana::make_tuple(VertexElement<float[3]>("inPos"),
                               VertexElement<float[2]>("inUV"));
  vao->setDSA_interleaved(*vbo, desc);
  ebo = std::make_unique<EBO>();
  ebo->setData(rectangle_indeces);
  vao->addEBO(*ebo);
}
void Blend::operator()() {
  shader->useProgram();

  shader->setUniform("grass", grass_land_noise->useTexture("grass"));
  shader->setUniform("mudLand", grass_land_noise->useTexture("land"));
  shader->setUniform("randomNoise", grass_land_noise->useTexture("noise"));
  glCall(glBindVertexArray, *vao);
  glCall(glDrawElements, GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
}
} // namespace glcore
} // namespace RGL
