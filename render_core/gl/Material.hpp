#include <cassert>
#include <vector>
#pragma once
#include "GLTextures.hpp"
#include "Shader.hpp"
namespace RGL {

namespace glcore {
class Material {
   protected:
    Texture* texture;
    std::string textureName;
   public:
    Material(Texture* texture,Shader* shader, const std::string& textureName);
    virtual ~Material() = default;
    Shader* shader;
    std::map<std::string,float> uniformMap;
    virtual void setShaderUniforms() = 0;
};

class PhongMaterial : public Material {
    //光斑指数
    GLfloat shiness;

   public:
    PhongMaterial(Texture* texture,Shader* shader, const std::string& textureName, GLfloat shiness);
    virtual ~PhongMaterial() = default;
    void setShaderUniforms() override;
};

}  // namespace glcore

}  // namespace RGL
