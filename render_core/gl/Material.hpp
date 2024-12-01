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
    ShaderManager* shaderManager;
   public:
    Material(Texture* texture, const std::string& textureName);
    virtual ~Material() = default;
    Shader* shader;

    virtual void setShaderUniforms();
    virtual std::vector<std::string> uniformNames() const = 0;
  
};

class PhongMaterial : public Material {
    // 光斑指数
    GLfloat shiness;

   public:
    PhongMaterial(Texture* texture,const std::string& textureName, GLfloat shiness);
    virtual ~PhongMaterial() = default;
    void setShaderUniforms() override;
    std::vector<std::string> uniformNames() const override;
};

class PhoneWithSPMask : public Material {
    GLfloat shiness;

    const std::string maskName = "spMask";
    
   public:
    PhoneWithSPMask(Texture* texture, const std::string& textureName, GLfloat shiness);
    void setShaderUniforms() override;
    std::vector<std::string> uniformNames() const override;
    
};

class WhiteMaterial:public Material {
   public:
    WhiteMaterial();
    std::vector<std::string> uniformNames() const override;
};


}  // namespace glcore

}  // namespace RGL
