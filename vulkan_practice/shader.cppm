module;
#include <fstream>
#include <string>
#include <vector>
#include <vulkan/vulkan_raii.hpp>
export module shader;

namespace RGL {

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
};

export class RenderPipeline {

  vk::raii::ShaderEXT meshShader = nullptr;
  vk::raii::ShaderEXT fragShader = nullptr;

public:
  vk::raii::PipelineLayout layout = nullptr;
  RenderPipeline(vk::raii::Device &device, const ShaderEffect &effect) {
    vk::ShaderCreateInfoEXT meshCreateInfo(
        {vk::ShaderCreateFlagBitsEXT::eDescriptorHeap},
        vk::ShaderStageFlagBits::eMeshEXT,
        vk::ShaderStageFlagBits::eFragment,
        vk::ShaderCodeTypeEXT::eSpirv,
        effect.meshStageSpv.size() * sizeof(uint32_t),
        effect.meshStageSpv.data(),
        "main",
        0, nullptr,
        0, nullptr,
        nullptr
    );

    vk::ShaderCreateInfoEXT fragCreateInfo(
        {vk::ShaderCreateFlagBitsEXT::eDescriptorHeap},
        vk::ShaderStageFlagBits::eFragment,
        {},
        vk::ShaderCodeTypeEXT::eSpirv,
        effect.fragStageSpv.size() * sizeof(uint32_t),
        effect.fragStageSpv.data(),
        "main",
        0, nullptr,
        0, nullptr,
        nullptr
    );

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

    // --- [1] 动态视口与裁剪 ---
vk::Viewport viewport(
    0.0f, 
    static_cast<float>(height), 
    static_cast<float>(width), 
    -static_cast<float>(height), 
    0.0f, 
    1.0f
);
    vk::Rect2D scissor({0, 0}, {width, height});
    cmd.setViewportWithCount(viewport);
    cmd.setScissorWithCount(scissor);

    // --- [2] 动态光栅化状态 ---
    cmd.setPolygonModeEXT(vk::PolygonMode::eFill);
    cmd.setCullMode(vk::CullModeFlagBits::eBack);
    cmd.setFrontFace(vk::FrontFace::eClockwise);
    cmd.setRasterizerDiscardEnable(false);

    // ✨【修复当前报错】显式关闭深度偏移（Depth Bias）
    cmd.setDepthBiasEnable(false);
    cmd.setDepthBias(0.0f, 0.0f,
                     0.0f); // 某些严格的驱动即使关闭了也要求调用一次设值

    // --- [3] 动态深度与模板测试 ---
    cmd.setDepthTestEnable(true);                // 开启深度测试
    cmd.setDepthWriteEnable(true);               // 允许写入深度
    cmd.setDepthCompareOp(vk::CompareOp::eLess); // 越近越渲染
    cmd.setDepthBoundsTestEnable(false);         // 关闭深度边界测试
    cmd.setStencilTestEnable(false);             // 关闭模板测试

    // --- [4] 动态多重采样 (Multisampling) ---
    cmd.setRasterizationSamplesEXT(vk::SampleCountFlagBits::e1);
    vk::SampleMask sampleMask = 0xFFFFFFFF;
    cmd.setSampleMaskEXT(vk::SampleCountFlagBits::e1, sampleMask);
    cmd.setAlphaToCoverageEnableEXT(false);
    cmd.setAlphaToOneEnableEXT(
        false); // ✨【提前防御】防止下一步报 AlphaToOne 未设置

    // --- [5] 动态颜色混合与逻辑操作 (Color Blending & Logic Op) ---
    cmd.setColorBlendEnableEXT(0, VK_FALSE); // 针对 Attachment 0 关闭混合

    vk::ColorComponentFlags colorWriteMask =
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    cmd.setColorWriteMaskEXT(0, colorWriteMask); // 允许写入 RGBA

    cmd.setLogicOpEnableEXT(false);
  }
};

}  // namespace RGL
