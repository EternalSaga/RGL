module;
#include <fstream>
#include <string>
#include <vector>
#include <vulkan/vulkan_raii.hpp>
export module shader;

export std::vector<uint32_t> loadShaderSpv(const std::string &filename) {
  std::ifstream file(filename, std::ios::ate | std::ios::binary);
  if (!file.is_open()) {
    throw std::runtime_error("failed to open file: " + filename);
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

  // 未来扩展：在这里存放由 Slang 反射提取出来的元数据 (Reflect Data)
  // 例如：Push Constant 的大小、反射出的线程组大小等
  uint32_t pushConstantSize = 0;
};

export class RenderPipeline {

  vk::raii::PipelineLayout layout = nullptr;
  vk::raii::ShaderEXT meshShader = nullptr ;
  vk::raii::ShaderEXT fragShader = nullptr;

public:
  RenderPipeline(vk::raii::Device &device, const ShaderEffect &effect) {

    // 1. 配置 Push Constants
    // 虽然 Shader Object 可以独立指定布局，但为了后续 vkCmdPushConstants
    // 的兼容性， 依然建议创建一个 PipelineLayout。
    vk::PushConstantRange pushConstantRange(
        vk::ShaderStageFlagBits::eMeshEXT | vk::ShaderStageFlagBits::eFragment,
        0, effect.pushConstantSize);

    vk::PipelineLayoutCreateInfo layoutCreateInfo({}, 0, nullptr, 1,
                                                  &pushConstantRange, nullptr);
    layout = vk::raii::PipelineLayout(device, layoutCreateInfo);

    // 2. 创建 Mesh Shader Object 配置
    // 根据 VK_EXT_shader_object 规范，不再需要创建 ShaderModule，直接传递
    // SPIR-V 源码
    vk::ShaderCreateInfoEXT meshCreateInfo(
        {},                                // flags
        vk::ShaderStageFlagBits::eMeshEXT, // stage
        vk::ShaderStageFlagBits::eFragment, // nextStage: 告知驱动下一个阶段是
                                            // Frag，利于硬件优化
        vk::ShaderCodeTypeEXT::eSpirv,                 // codeType
        effect.meshStageSpv.size() * sizeof(uint32_t), // codeSize
        effect.meshStageSpv.data(),                    // pCode
        "main",                                        // pName (入口点)
        0, nullptr,            // setLayoutCount, pSetLayouts
        1, &pushConstantRange, // pushConstantRangeCount, pPushConstantRanges
        nullptr                // pSpecializationInfo
    );

    // 3. 创建 Fragment Shader Object 配置
    vk::ShaderCreateInfoEXT fragCreateInfo(
        {},                                 // flags
        vk::ShaderStageFlagBits::eFragment, // stage
        {},                                 // nextStage: 已经是最后一阶段，填空
        vk::ShaderCodeTypeEXT::eSpirv,      // codeType
        effect.fragStageSpv.size() * sizeof(uint32_t), // codeSize
        effect.fragStageSpv.data(),                    // pCode
        "main",                                        // pName
        0, nullptr,            // setLayoutCount, pSetLayouts
        1, &pushConstantRange, // pushConstantRangeCount, pPushConstantRanges
        nullptr                // pSpecializationInfo
    );

    // 4. 使用 vkCreateShadersEXT 批量创建着色器对象
    // 在 vk::raii 中，createShadersEXT 会返回一个
    // std::vector<vk::raii::ShaderEXT>
    std::vector<vk::ShaderCreateInfoEXT> createInfos = {meshCreateInfo,
                                                        fragCreateInfo};
    auto shaders = device.createShadersEXT(createInfos);
    meshShader = std::move(shaders[0]);
    fragShader = std::move(shaders[1]);
  }
  void recordRenderCommands(vk::raii::CommandBuffer &cmd, uint32_t width,
                            uint32_t height) {
    // 1. 绑定 Shader 对象的各个阶段
    // 规范要求：未使用的传统图形阶段（如 Vertex、Geometry
    // 等）如果不使用，必须显式绑定为 VK_NULL_HANDLE
    std::vector<vk::ShaderStageFlagBits> stages = {
        vk::ShaderStageFlagBits::eVertex,
        vk::ShaderStageFlagBits::eTessellationControl,
        vk::ShaderStageFlagBits::eTessellationEvaluation,
        vk::ShaderStageFlagBits::eGeometry,
        vk::ShaderStageFlagBits::eTaskEXT,
        vk::ShaderStageFlagBits::eMeshEXT,
        vk::ShaderStageFlagBits::eFragment};

    std::vector<vk::ShaderEXT> shaderHandles = {
        vk::ShaderEXT(nullptr),     vk::ShaderEXT(nullptr),
        vk::ShaderEXT(nullptr),     vk::ShaderEXT(nullptr),
        vk::ShaderEXT(nullptr),
        vk::ShaderEXT(*meshShader), // 解引用 RAII 对象获取 handle
        vk::ShaderEXT(*fragShader)};

    cmd.bindShadersEXT(stages, shaderHandles);

    // 2. 动态设置以前硬编码在 Pipeline 里的所有状态

    // 动态视口与裁剪
    vk::Viewport viewport(0.0f, 0.0f, static_cast<float>(width),
                          static_cast<float>(height), 0.0f, 1.0f);
    vk::Rect2D scissor({0, 0}, {width, height});
    cmd.setViewportWithCountEXT(viewport);
    cmd.setScissorWithCountEXT(scissor);

    // 动态光栅化状态 (对应你原代码的 rasterizer)
    cmd.setPolygonModeEXT(vk::PolygonMode::eFill);
    cmd.setCullMode(vk::CullModeFlagBits::eBack);
    cmd.setFrontFace(vk::FrontFace::eCounterClockwise);
    cmd.setRasterizerDiscardEnable(false);

    // 动态多重采样 (对应你原代码的 multisampling)
    cmd.setRasterizationSamplesEXT(vk::SampleCountFlagBits::e1);
    vk::SampleMask sampleMask = 0xFFFFFFFF;
    cmd.setSampleMaskEXT(vk::SampleCountFlagBits::e1, sampleMask);
    cmd.setAlphaToCoverageEnableEXT(false);

    // 动态颜色混合 (对应你原代码的 colorBlending 和 colorBlendAttachment)
    vk::Bool32 blendEnable = VK_FALSE;
    cmd.setColorBlendEnableEXT(0,
                               blendEnable); // 针对 Attachment 0 关闭混合

    vk::ColorComponentFlags colorWriteMask =
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    cmd.setColorWriteMaskEXT(0, colorWriteMask);

    // 拓扑结构 (尽管使用 Mesh Shader，规范仍要求必须设置默认拓扑以通过校验层)
    cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
    cmd.setPrimitiveRestartEnable(false);
  }
};
