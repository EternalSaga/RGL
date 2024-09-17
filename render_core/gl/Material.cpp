#include "Material.hpp"

namespace RGL {

namespace glcore {


void PhongMaterial::setShaderUniforms(Shader* shader) {
    shader->setUniform("spotIntensity", static_cast<float>(shiness));
}

Material::Material(Texture* texture, const std::string& textureName) {
    this->texture = texture;
    this->textureName = textureName;
    this->texture->useTexture(textureName);
}
PhongMaterial::PhongMaterial(Texture* texture, const std::string& textureName, GLfloat shiness) : Material(texture, textureName), shiness(shiness) {
}
}  // namespace glcore
}  // namespace RGL