#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "GLObj.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "Mesh.hpp"
namespace RGL
{
namespace glcore
{


class Cube : public Mesh
{
    float size;
    float halfSize;
    std::vector<GLfloat> positions;
    std::vector<GLfloat> uvs;
    std::vector<GLfloat> normals;
    
  public:
    Cube(float size);
};

class Sphere : public Mesh
{
  public:
    Sphere(float radius);
    ~Sphere() = default;

  private:
    std::vector<GLfloat> positions;
    std::vector<GLfloat> uvs;
};


class Plane : public Mesh{
  public:
    Plane(float width, float height);
    ~Plane() = default;
};

} // namespace glcore

} // namespace RGL
