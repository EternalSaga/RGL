#include "Material.hpp"

namespace RGL {

namespace glcore {


void PhongMaterial::setShaderUniforms() {
    shader->setUniform("spotIntensity", static_cast<float>(shiness));
}

Material::Material(Texture* texture,Shader* shader, const std::string& textureName) {
    this->texture = texture;
    this->textureName = textureName;
    this->texture->useTexture(textureName);
    this->shader = shader;
    this->shader->setUniform("sampler", this->texture->useTexture(textureName));
}
PhongMaterial::PhongMaterial(Texture* texture,Shader* shader, const std::string& textureName, GLfloat shiness) : Material(texture,shader, textureName), shiness(shiness) {
}
}  // namespace glcore
}  // namespace RGL