#include <cassert>
#include <vector>
#pragma once
#include "GLTextures.hpp"
#include "Shader.hpp"
namespace RGL {

namespace glcore {
class Material {
    Texture* texture;
    std::string textureName;
   public:
    Material(Texture* texture, const std::string& textureName);
    virtual ~Material() = default;

    std::map<std::string,float> uniformMap;
    virtual void setShaderUniforms(Shader* shader) = 0;
};

class PhongMaterial : public Material {
    //光斑指数
    GLfloat shiness;

   public:
    PhongMaterial(Texture* texture, const std::string& textureName, GLfloat shiness);
    virtual ~PhongMaterial() = default;
    void setShaderUniforms(Shader* shader) override;
};

}  // namespace glcore

}  // namespace RGL
