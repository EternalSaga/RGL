#include <cassert>
#include <vector>
#pragma once


#include <GLTextures.hpp>
#include "ShaderManager.hpp"
namespace RGL {

namespace glcore {
class Material {
   protected:
    Texture* texture;
    std::string textureName;

   public:
    Material(Texture* texture, const std::string& textureName);
    virtual ~Material() = default;

    virtual void setShaderUniforms(UniformComponent& uniformComponent);
  
};

class PhongMaterial : public Material {
    // 光斑指数
    GLfloat shiness;

   public:
    PhongMaterial(Texture* texture,const std::string& textureName, GLfloat shiness);
    virtual ~PhongMaterial() = default;

    void setShaderUniforms(UniformComponent& uniformComponent) override;
};

class PhoneWithSPMask : public Material {
    GLfloat shiness;

    const std::string maskName = "spMask";
    
   public:
    PhoneWithSPMask(Texture* texture, const std::string& textureName, GLfloat shiness);

    void setShaderUniforms(UniformComponent& uniformComponent) override;
};

class WhiteMaterial:public Material {
   public:
    WhiteMaterial();

};


}  // namespace glcore

}  // namespace RGL
