#include "Material.hpp"
#include "rllogger.hpp"
#include "ShaderManager.hpp"
namespace RGL {

namespace glcore {



void PhongMaterial::setShaderUniforms(DiscreteUniforms& uniformComponent) {

    uniformComponent["sampler"] = this->texture->useTexture(this->textureName);

}

Material::Material(Texture* texture,const std::string& textureName) {
    this->texture = texture;
    this->textureName = textureName;
    this->texture->useTexture(textureName);
}


void Material::setShaderUniforms(DiscreteUniforms& uniformComponent) {
    auto logger = RLLogger::getInstance();
    logger->info("Default material setShaderUniforms do nothing.");
}
    
PhongMaterial::PhongMaterial(Texture* texture,const std::string& textureName) : Material(texture,textureName) {

}

void PhoneWithSPMask::setShaderUniforms(DiscreteUniforms& uniformComponent) {
    uniformComponent["sampler"] = this->texture->useTexture(this->textureName);
    uniformComponent["spMask"] = this->texture->useTexture(this->maskName);
}
PhoneWithSPMask::PhoneWithSPMask(Texture* texture, const std::string& textureName) : Material(texture, textureName){

}

WhiteMaterial::WhiteMaterial():Material(nullptr,""){
	
}

}  // namespace glcore
}  // namespace RGL
