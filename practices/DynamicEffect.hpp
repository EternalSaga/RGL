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

namespace RGL
{
namespace glcore
{
class ColorfulShiningTriangle : public Renderer
{
    std::unique_ptr<EBO> ebo;
    std::unique_ptr<VAO> vao;
    std::unique_ptr<VBO> vbo;
    std::unique_ptr<Shader> shader;

  public:
    ColorfulShiningTriangle();
    virtual ~ColorfulShiningTriangle() = default;
    void operator()() override;
};

class MovingTriangle : public Renderer
{
    std::unique_ptr<EBO> ebo;
    std::unique_ptr<VAO> vao;
    std::unique_ptr<VBO> vbo;
    std::unique_ptr<Shader> shader;

  public:
    MovingTriangle();
    virtual ~MovingTriangle() = default;
    void operator()() override;
};

class MovingTexture : public Renderer
{
    std::unique_ptr<VBO> vbo;
    std::unique_ptr<VAO> vao;
    std::unique_ptr<Shader> shader;
    std::unique_ptr<EBO> ebo;
    std::unique_ptr<Texture> texture;

  public:
    MovingTexture();
    void operator()() override;
};

class BlendMipmap : public Renderer
{
    std::unique_ptr<VBO> vbo;
    std::unique_ptr<VAO> vao;
    std::unique_ptr<Shader> shader;
    std::unique_ptr<EBO> ebo;
    std::unique_ptr<Texture> grass_land_noise;

  public:
    BlendMipmap();
    void operator()() override;
};
} // namespace glcore
} // namespace RGL