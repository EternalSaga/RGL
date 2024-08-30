#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "GLObj.hpp"
namespace RGL
{
namespace glcore
{
class CommonGeometry
{
  protected:
    std::vector<GLfloat> positions;
    std::vector<GLfloat> uvs;
    std::vector<GLfloat> position_uvs; // 融合pos和uv，少一个vao
    std::vector<int> indices;
    size_t indicesCount;
    std::unique_ptr<VAO> vao;
    std::unique_ptr<VBO> vbo;

  public:
    CommonGeometry() : positions(), uvs(), indices(), indicesCount(0)
    {
    }

    std::tuple<size_t, size_t> getIdicesCountAndOffset();

    
    std::unique_ptr<VAO> getVAO();

    virtual ~CommonGeometry() = default;
};

class Cube : public CommonGeometry
{
    float size;
    float halfSize;

  public:
    Cube(float size,GLuint shaderProgram);
};

class Sphere : public CommonGeometry
{
  public:
    Sphere(float radius)
    {
    }
    ~Sphere() = default;

  private:
};

} // namespace glcore

} // namespace RGL
