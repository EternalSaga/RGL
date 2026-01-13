#include "grass_shader_system.hpp"
#include <glad/glad.h>
#include <array>
#include <memory>
#include "GLTextures.hpp"
#include "Shader.hpp"
#include "grass_fragment.hpp"
#include "phong_ubo_instanced.hpp"
#include "UBO.hpp"
#include "SSBO.hpp"
#include "Material.hpp"

namespace RGL {
namespace glcore {

void InstancedGrassShaderSystem::update() {
    // Render logic here
    auto view = singleReg->view<const phong_ubo_instanced::InstanceData, phong_ubo_instanced::CameraBlock, grass_fragment::DirectionLight, grass_fragment::MaterialIndices, UBOs, SSBO, ShaderRef, std::shared_ptr<AssetMaterialData>>();
    view.each([](entt::entity entity, const phong_ubo_instanced::InstanceData& instanceData, phong_ubo_instanced::CameraBlock& cameraBlock, grass_fragment::DirectionLight& directionLight, grass_fragment::MaterialIndices& materialIndices, UBOs& ubos, SSBO& ssbo, ShaderRef shaderRef, std::shared_ptr<AssetMaterialData> material) {
	ssbo.updateBufferSubData(instanceData.data(), instanceData.size() * sizeof(phong_ubo_instanced::InstanceData), 0);
	(*ubos)[cameraBlock.getUboName()]->setUniform(cameraBlock.data());
	(*ubos)[directionLight.getUboName()]->setUniform(directionLight.data());
	(*ubos)[materialIndices.getUboName()]->setUniform(materialIndices.data());


	std::array<GLuint, 1> texture{*(material->getTextures()[TextureUsageType::BASE_COLOR])};
	grass_fragment::SamplerArray_MyTextures::setUniform(*shaderRef, texture);
    });
}

}  // namespace render_core
}  // namespace RGL  