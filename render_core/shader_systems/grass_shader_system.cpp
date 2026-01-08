#include "grass_shader_system.hpp"
#include <glad/glad.h>
#include <array>
#include <memory>
#include "GLTextures.hpp"
#include "Shader.hpp"
#include "grass_fragment_shader_template.hpp"

#include "UBO.hpp"
#include "SSBO.hpp"
#include "Material.hpp"

namespace RGL {
namespace glcore {

void InstancedGrassShaderSystem::update() {
    // Render logic here
    auto view = singleReg->view<const grass_fragment::InstanceData, grass_fragment::CameraBlock, grass_fragment::DirectionLight, grass_fragment::MaterialIndices, UBOs, SSBO, ShaderRef, std::shared_ptr<AssetMaterialData>>();
    view.each([](entt::entity entity, const grass_fragment::InstanceData& instanceData, grass_fragment::CameraBlock& cameraBlock, grass_fragment::DirectionLight& directionLight, grass_fragment::MaterialIndices& materialIndices, UBOs& ubos, SSBO& ssbo, ShaderRef shaderRef, std::shared_ptr<AssetMaterialData> material) {
	ssbo.updateBufferSubData(instanceData.data(), instanceData.size() * sizeof(grass_fragment::InstanceData), 0);
	(*ubos)["CameraBlock"]->setUniform(cameraBlock.data());
	(*ubos)["DirectionLight"]->setUniform(directionLight.data());
	(*ubos)["MaterialIndices"]->setUniform(materialIndices.data());


	std::array<GLuint, 1> texture{*(material->getTextures()[TextureUsageType::BASE_COLOR])};
	grass_fragment::SamplerArray_Mytextures::setUniform(*shaderRef, texture);
    });
}

}  // namespace render_core
}  // namespace RGL  