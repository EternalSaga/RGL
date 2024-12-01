#include "Material.hpp"
#include "rllogger.hpp"
#include "ShaderManager.hpp"
namespace RGL {

namespace glcore {


void PhongMaterial::setShaderUniforms() {

	shaderManager->updateUniform("spotIntensity", static_cast<float>(shiness));
    shaderManager->updateUniform("sampler", this->texture->useTexture(this->textureName));

}

std::vector<std::string> PhongMaterial::uniformNames()const  {
    std::vector<std::string> names;
    names.push_back("spotIntensity");
    names.push_back("sampler");
    return names;
}

Material::Material(Texture* texture,const std::string& textureName) {
    this->texture = texture;
    this->textureName = textureName;
    this->texture->useTexture(textureName);
    this->shader = shader;
	this->shaderManager = ShaderManager::getInstance();
}

void Material::setShaderUniforms() {
    auto logger = RLLogger::getInstance();
    logger->info("Default material setShaderUniforms do nothing.");
}
    
PhongMaterial::PhongMaterial(Texture* texture,const std::string& textureName, GLfloat shiness) : Material(texture,textureName), shiness(shiness) {

}
std::vector<std::string> PhoneWithSPMask::uniformNames()const {
    std::vector<std::string> names;
    names.push_back("spotIntensity");
    names.push_back("sampler");
    names.push_back("spMask");

    return names;
}
PhoneWithSPMask::PhoneWithSPMask(Texture* texture, const std::string& textureName, GLfloat shiness) : Material(texture, textureName), shiness(shiness) {

}
void PhoneWithSPMask::setShaderUniforms() {
	shaderManager->updateUniform("spotIntensity", static_cast<float>(shiness));
    shaderManager->updateUniform("sampler", this->texture->useTexture(this->textureName));
	shaderManager->updateUniform("spMask", this->texture->useTexture(this->maskName));
}
WhiteMaterial::WhiteMaterial():Material(nullptr,""){
	
}
std::vector<std::string> WhiteMaterial::uniformNames() const {
    return std::vector<std::string>();
}
}  // namespace glcore
}  // namespace RGL
