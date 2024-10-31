#include "Material.hpp"
#include "rllogger.hpp"
namespace RGL {

namespace glcore {


void PhongMaterial::setShaderUniforms() {
    shader->setUniform("spotIntensity", static_cast<float>(shiness));
    this->shader->setUniform("sampler", this->texture->useTexture(this->textureName));
}

Material::Material(Texture* texture, Shader* shader, const std::string& textureName) {
    this->texture = texture;
    this->textureName = textureName;
    this->texture->useTexture(textureName);
    this->shader = shader;
}

void Material::setShaderUniforms() {
    auto logger = RLLogger::getInstance();
    logger->info("Default material setShaderUniforms do nothing.");
}
    
PhongMaterial::PhongMaterial(Texture* texture, Shader* shader, const std::string& textureName, GLfloat shiness) : Material(texture,shader,textureName), shiness(shiness) {

}
PhoneWithSPMask::PhoneWithSPMask(Texture* texture, Shader* shader, const std::string& textureName, GLfloat shiness) : Material(texture, shader, textureName), shiness(shiness) {

}
void PhoneWithSPMask::setShaderUniforms() {
    shader->setUniform("spotIntensity", static_cast<float>(shiness));
    this->shader->setUniform("sampler", this->texture->useTexture(this->textureName));
    this->shader->setUniform("spMask", this->texture->useTexture(this->maskName) );
}
WhiteMaterial::WhiteMaterial(Shader* shader):Material(nullptr,shader,""){

}
}  // namespace glcore
}  // namespace RGL
