#include <cassert>
#include <vector>
#pragma once

#include "Shader.hpp"
#include <GLTextures.hpp>
namespace RGL {

namespace glcore {
class Material {
   protected:
    Texture* texture;
    std::string textureName;
   public:
    Material(Texture* texture, Shader* shader, const std::string& textureName);
    virtual ~Material() = default;
    Shader* shader;
    std::map<std::string, float> uniformMap;
    virtual void setShaderUniforms();
};

class PhongMaterial : public Material {
    // 光斑指数
    GLfloat shiness;

   public:
    PhongMaterial(Texture* texture, Shader* shader, const std::string& textureName, GLfloat shiness);
    virtual ~PhongMaterial() = default;
    void setShaderUniforms() override;
};

class PhoneWithSPMask : public Material {
    GLfloat shiness;

    const std::string maskName = "spMask";
   public:
    PhoneWithSPMask(Texture* texture, Shader* shader, const std::string& textureName, GLfloat shiness);
    void setShaderUniforms() override;
};

class WhiteMaterial:public Material {
   public:
    WhiteMaterial(Shader* shader);
};


}  // namespace glcore

}  // namespace RGL
