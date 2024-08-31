#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "GLObj.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
namespace RGL
{
namespace glcore
{
class CommonGeometry
{
  protected:

    std::vector<GLfloat> position_uvs; // 融合pos和uv，少创建一个vao
    std::vector<int> indices;
    size_t indicesCount;
    std::unique_ptr<VAO> vao;
    std::unique_ptr<VBO> vbo;



  public:
    CommonGeometry() :  indices(), indicesCount(0)
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
    std::vector<GLfloat> positions;
    std::vector<GLfloat> uvs;
  public:
    Cube(float size,GLuint shaderProgram);
};

class Sphere : public CommonGeometry
{
  public:
    Sphere(float radius, GLuint shaderProgram);
    ~Sphere() = default;

  private:
    std::vector<GLfloat> positions;
    std::vector<GLfloat> uvs;
};

} // namespace glcore

} // namespace RGL
