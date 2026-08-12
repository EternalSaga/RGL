module;
#include <vulkan/vulkan_raii.hpp>
export module shader;

import rlstd;
import types_generated;
import vkerror;

namespace RGL {

export [[nodiscard]] std::expected<std::vector<uint32_t>, Error>
loadShaderSpv(const std::string &filename) {
  std::ifstream file(filename, std::ios::ate | std::ios::binary);
  if (!file.is_open()) {
    return std::unexpected(Error{ErrorCode::ShaderLoadFailed,
        "failed to open file: " + filename});
  }
  size_t fileSize = (size_t)file.tellg();
  std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
  file.seekg(0);
  file.read(reinterpret_cast<char *>(buffer.data()), fileSize);
  file.close();
  return buffer;
}

export struct ShaderEffect {
  std::vector<uint32_t> meshStageSpv;
  std::vector<uint32_t> fragStageSpv;
};

export class RenderPipeline {

  vk::raii::ShaderEXT meshShader = nullptr;
  vk::raii::ShaderEXT fragShader = nullptr;

public:
  RenderPipeline(vk::raii::Device &device, const ShaderEffect &effect,
                  uint32_t descriptorSize, uint32_t slotVertex, uint32_t slotIndex) {
    VkDescriptorSetAndBindingMappingEXT mappings[2]{};

    mappings[0].sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
    mappings[0].descriptorSet = 0;
    mappings[0].firstBinding = 0;
    mappings[0].bindingCount = 1;
    mappings[0].resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
    mappings[0].source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
    mappings[0].sourceData.constantOffset.heapOffset = slotVertex * descriptorSize;
    mappings[0].sourceData.constantOffset.heapArrayStride = descriptorSize;

    mappings[1].sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
    mappings[1].descriptorSet = 0;
    mappings[1].firstBinding = 1;
    mappings[1].bindingCount = 1;
    mappings[1].resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
    mappings[1].source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
    mappings[1].sourceData.constantOffset.heapOffset = slotIndex * descriptorSize;
    mappings[1].sourceData.constantOffset.heapArrayStride = descriptorSize;

    VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{};
    mappingInfo.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT;
    mappingInfo.mappingCount = 2;
    mappingInfo.pMappings = mappings;

    VkShaderCreateInfoEXT meshCreateInfoRaw{};
    meshCreateInfoRaw.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
    meshCreateInfoRaw.pNext = &mappingInfo;
    meshCreateInfoRaw.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
    meshCreateInfoRaw.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    meshCreateInfoRaw.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
    meshCreateInfoRaw.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    meshCreateInfoRaw.codeSize = effect.meshStageSpv.size() * sizeof(uint32_t);
    meshCreateInfoRaw.pCode = effect.meshStageSpv.data();
    meshCreateInfoRaw.pName = "main";
    vk::ShaderCreateInfoEXT meshCreateInfo(meshCreateInfoRaw);

    VkShaderCreateInfoEXT fragCreateInfoRaw{};
    fragCreateInfoRaw.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
    fragCreateInfoRaw.pNext = &mappingInfo;
    fragCreateInfoRaw.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
    fragCreateInfoRaw.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragCreateInfoRaw.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    fragCreateInfoRaw.codeSize = effect.fragStageSpv.size() * sizeof(uint32_t);
    fragCreateInfoRaw.pCode = effect.fragStageSpv.data();
    fragCreateInfoRaw.pName = "main";
    vk::ShaderCreateInfoEXT fragCreateInfo(fragCreateInfoRaw);

    std::vector<vk::ShaderCreateInfoEXT> createInfos = {meshCreateInfo, fragCreateInfo};
    auto shaders = device.createShadersEXT(createInfos);
    meshShader = std::move(shaders[0]);
    fragShader = std::move(shaders[1]);
  }
  void recordRenderCommands(vk::raii::CommandBuffer &cmd, uint32_t width,
                            uint32_t height) {
    // Bind shader stages; unused traditional stages must be set to VK_NULL_HANDLE
    std::array<vk::ShaderStageFlagBits, 7> stages = {
        vk::ShaderStageFlagBits::eVertex,
        vk::ShaderStageFlagBits::eTessellationControl,
        vk::ShaderStageFlagBits::eTessellationEvaluation,
        vk::ShaderStageFlagBits::eGeometry,
        vk::ShaderStageFlagBits::eTaskEXT,
        vk::ShaderStageFlagBits::eMeshEXT,
        vk::ShaderStageFlagBits::eFragment};

    std::array<vk::ShaderEXT, 7> shaderHandles = {
        vk::ShaderEXT(nullptr), vk::ShaderEXT(nullptr),
        vk::ShaderEXT(nullptr), vk::ShaderEXT(nullptr),
        vk::ShaderEXT(nullptr),
        vk::ShaderEXT(*meshShader),
        vk::ShaderEXT(*fragShader)};

    cmd.bindShadersEXT(stages, shaderHandles);

    // Viewport & scissor
    vk::Viewport viewport(0.0f, static_cast<float>(height),
                          static_cast<float>(width), -static_cast<float>(height),
                          0.0f, 1.0f);
    vk::Rect2D scissor({0, 0}, {width, height});
    cmd.setViewportWithCount(viewport);
    cmd.setScissorWithCount(scissor);

    // Rasterization
    cmd.setPolygonModeEXT(vk::PolygonMode::eFill);
    cmd.setCullMode(vk::CullModeFlagBits::eNone);
    cmd.setFrontFace(vk::FrontFace::eCounterClockwise);
    cmd.setRasterizerDiscardEnable(false);
    cmd.setDepthBiasEnable(false);
    cmd.setDepthBias(0.0f, 0.0f, 0.0f);

    // Depth & stencil
    cmd.setDepthTestEnable(true);
    cmd.setDepthWriteEnable(true);
    cmd.setDepthCompareOp(vk::CompareOp::eLess);
    cmd.setDepthBoundsTestEnable(false);
    cmd.setStencilTestEnable(false);

    // Multisampling
    cmd.setRasterizationSamplesEXT(vk::SampleCountFlagBits::e1);
    cmd.setSampleMaskEXT(vk::SampleCountFlagBits::e1, 0xFFFFFFFF);
    cmd.setAlphaToCoverageEnableEXT(false);
    cmd.setAlphaToOneEnableEXT(false);

    // Color blending
    cmd.setColorBlendEnableEXT(0, VK_FALSE);
    cmd.setColorWriteMaskEXT(0,
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
    cmd.setLogicOpEnableEXT(false);
  }
};

}  // namespace RGL