module;
#include <vulkan/vulkan_raii.hpp>
#include <fmt/format.h>
export module shader;

import rlstd;
import types_generated;
import vkerror;

namespace RGL {

export [[nodiscard]] std::expected<std::vector<uint32_t>, Error>
loadShaderSpv(const std::string& filename) {
	std::ifstream file(filename, std::ios::ate | std::ios::binary);
	if (!file.is_open()) {
		return std::unexpected(Error{ErrorCode::ShaderLoadFailed,
			fmt::format("failed to open file: {}", filename)});
	}
	size_t fileSize = (size_t)file.tellg();
	std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
	file.close();
	return buffer;
}

export struct ShaderEffect {
	std::vector<uint32_t> meshStageSpv;
	std::vector<uint32_t> fragStageSpv;
};

// 动态状态（Dynamic State）：与 shader object 解耦的"pass 状态"值集合。
// 从 RenderPipeline 移出（原 recordRenderCommands 内烘焙），由调用点（Renderer）
// 按 pass 传入——同一 mesh shader 可在不同 pass 用不同状态（Phase 7 G-buffer 多 pass）。
// 默认值 = 当前 cube 的 opaque pass 状态（不透明、写深度、无混合、单采样）。
// 注意：viewport/scissor 依赖窗口宽高，不在此结构，由 applyDynamicState 的 width/height 参数设置。
export struct RenderState {
	vk::PolygonMode polygonMode = vk::PolygonMode::eFill;
	vk::CullModeFlagBits cullMode = vk::CullModeFlagBits::eNone;
	vk::FrontFace frontFace = vk::FrontFace::eCounterClockwise;
	bool rasterizerDiscard = false;
	bool depthBiasEnable = false;
	float depthBiasSlope = 0.0f;
	float depthBiasScale = 0.0f;
	bool depthTest = true;
	bool depthWrite = true;
	vk::CompareOp depthCompareOp = vk::CompareOp::eLess;
	bool depthBoundsTest = false;
	bool stencilTest = false;
	vk::SampleCountFlagBits rasterizationSamples = vk::SampleCountFlagBits::e1;
	uint32_t sampleMask = 0xFFFFFFFF;
	bool alphaToCoverage = false;
	bool alphaToOne = false;
	bool colorBlend = false;
	vk::ColorComponentFlags colorWriteMask =
		vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
		vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
	bool logicOp = false;
};

// 设置全部动态状态（viewport/scissor 用 width/height，其余用 state）。
// spec 要求相关 set 命令在 draw 之前调用即可（draw 时采样），与 bindShadersEXT 的先后无约束。
export void applyDynamicState(vk::raii::CommandBuffer& cmd, uint32_t width, uint32_t height,
	const RenderState& state) {
	// Viewport & scissor
	vk::Viewport viewport(0.0f, static_cast<float>(height),
		static_cast<float>(width), -static_cast<float>(height),
		0.0f, 1.0f);
	vk::Rect2D scissor({0, 0}, {width, height});
	cmd.setViewportWithCount(viewport);
	cmd.setScissorWithCount(scissor);

	// Rasterization
	cmd.setPolygonModeEXT(state.polygonMode);
	cmd.setCullMode(state.cullMode);
	cmd.setFrontFace(state.frontFace);
	cmd.setRasterizerDiscardEnable(state.rasterizerDiscard);
	cmd.setDepthBiasEnable(state.depthBiasEnable);
	cmd.setDepthBias(state.depthBiasScale, state.depthBiasSlope, 0.0f);

	// Depth & stencil
	cmd.setDepthTestEnable(state.depthTest);
	cmd.setDepthWriteEnable(state.depthWrite);
	cmd.setDepthCompareOp(state.depthCompareOp);
	cmd.setDepthBoundsTestEnable(state.depthBoundsTest);
	cmd.setStencilTestEnable(state.stencilTest);

	// Multisampling
	cmd.setRasterizationSamplesEXT(state.rasterizationSamples);
	cmd.setSampleMaskEXT(state.rasterizationSamples, state.sampleMask);
	cmd.setAlphaToCoverageEnableEXT(state.alphaToCoverage);
	cmd.setAlphaToOneEnableEXT(state.alphaToOne);

	// Color blending：enable 只是开关——factors/equation 是独立的动态状态，不设置则
	// 保持默认值（≈不混合），开了 colorBlend 也不产生混合效果（Phase 5 透明排序接线）。
	// 标准 alpha 混合：RGB = src·srcAlpha + dst·(1-srcAlpha)，A = srcA（替换）
	if (state.colorBlend) {
		// hpp 包装类无聚合初始化（仅默认/拷贝/Vk* 构造）——裸 C 结构体逐字段填后包一层。
		// 注意 v341 头：单 colorBlendOp/alphaBlendOp 字段（非 src/dst 各一，新版头才拆分）
		VkColorBlendEquationEXT raw{};
		raw.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		raw.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		raw.colorBlendOp = VK_BLEND_OP_ADD;
		raw.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		raw.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		raw.alphaBlendOp = VK_BLEND_OP_ADD;
		cmd.setColorBlendEquationEXT(0, vk::ColorBlendEquationEXT(raw));
	}
	cmd.setColorBlendEnableEXT(0, state.colorBlend);
	cmd.setColorWriteMaskEXT(0, state.colorWriteMask);
	cmd.setLogicOpEnableEXT(state.logicOp);
}

// binding mapping 烘焙参数（Phase 5.5 聚合：env 追加后构造参数膨胀到 13 个，改 struct
// 数据驱动表达 cube/model/IBL 形态）。heap 槽 = 绝对 slot（resource heap），sampler 走
// **字节偏移**（sampler heap，OpTypeSampler 映射的 spec 正确字段 heapOffset，见
// RenderPipeline 内注释）——换算（slot × descriptorSize）在调用点（ShaderRegistry）做。
export struct PipelineBindings {
	uint32_t descriptorSize = 0;   // resource heap 步长（max(buffer, image) 运行时查询）
	uint32_t slotVertex = 0;       // binding 0：顶点 storage buffer 槽
	uint32_t slotIndex = 0;        // binding 1（非 textured 形态）：索引槽
	uint32_t slotTexture = 0;      // binding 1（textured 形态）：SAMPLED_IMAGE 槽
	uint32_t samplerHeapOffset = 0;  // binding 2（textured）：sampler heap 字节偏移
	uint32_t slotSceneData = 0;    // binding 3（hasSceneData）：scene data storage 槽
	uint32_t slotEnv = 0;          // binding 4（hasEnv）：env cubemap SAMPLED_IMAGE 槽
	uint32_t envSamplerHeapOffset = 0;  // binding 5（hasEnv）：sky sampler heap 字节偏移
	bool textured = false;
	bool hasSceneData = false;
	bool hasEnv = false;
};

// RenderPipeline：纯 shader object 包装（mesh + frag 两个 VkShaderEXT + 烘焙的
// descriptor heap binding mapping）。动态状态不再内聚于此——见 RenderState / applyDynamicState。
export class RenderPipeline {
	vk::raii::ShaderEXT meshShader = nullptr;
	vk::raii::ShaderEXT fragShader = nullptr;

    public:
	// bindings.textured 缺省 = cube 形态（2 条目：binding 0/1）。
	// textured=true = 模型形态（3 条目，Phase 3）：binding 1 → SAMPLED_IMAGE 描述符槽
	//（resource heap，slotTexture），binding 2 → sampler heap 槽（OpTypeSampler 映射，
	// heapOffset 字节偏移——独立 sampler 的 spec 正确字段，见下方注释；
	// vkCmdBindSamplerHeapEXT 每帧绑定）。
	// 注意：sampler 走独立 sampler heap 而非 pEmbeddedSampler——2026-08-27 spike
	// 交叉测试两者三驱动均 PASS（docs/heap_texture_research.md §九），此处维持独立
	// sampler heap（docs/phase3_plan.md R1 回退链的既选路径，已验证可用）。
	// hasSceneData=true（Phase 5 批次 2，模型形态）追加 binding 3 → scene data storage
	// buffer 槽（光照 + 对象矩阵表，Resources::ensureSceneData；模型 mesh/unlit frag 不
	// 声明 binding 3 = 映射空转无副作用，同 mesh 阶段不声明 binding 1/2 的既证行为）。
	// hasEnv=true（Phase 5.5）追加 binding 4/5 → env cubemap 槽 + sky sampler 偏移
	//（仅 import env.slang 的风格消费——pbr/phong/unlit 不声明 = 映射空转，同上既证行为）。
	RenderPipeline(vk::raii::Device& device, const ShaderEffect& effect,
		const PipelineBindings& bindings) {
		const uint32_t descriptorSize = bindings.descriptorSize;
		VkDescriptorSetAndBindingMappingEXT mappings[6]{};
		// 顺序填充（Phase 5.5 批次 4 修正：原固定下标 [0/1/2/3/4/5] 假设 textured=true——
		// 天空盒首个 textured=false+hasSceneData+hasEnv 组合下 mappings[2] 留空却计入
		// count → VVL "sType must be..." + envSampler 被挤出 count → "no mapping for
		// binding 5"。改 running index 按实际启用的 binding 连续填，firstBinding 显式携带）
		uint32_t n = 0;
		const auto add = [&](uint32_t firstBinding, vk::DeviceSize heapOffset,
			vk::DeviceSize arrayStride) {
			auto& m = mappings[n++];
			m.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
			m.descriptorSet = 0;
			m.firstBinding = firstBinding;
			m.bindingCount = 1;
			m.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
			m.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
			m.sourceData.constantOffset.heapOffset = heapOffset;
			m.sourceData.constantOffset.heapArrayStride = arrayStride;
		};

		add(0, bindings.slotVertex * descriptorSize, descriptorSize);
		// binding 1：非纹理形态 = 索引 slot；纹理形态 = SAMPLED_IMAGE 描述符槽（resource heap）
		add(1, (bindings.textured ? bindings.slotTexture : bindings.slotIndex) * descriptorSize,
			descriptorSize);
		if (bindings.textured) {
			// OpTypeSampler 映射：sampler heap 槽（字节偏移，共享一份 sampler 描述符）。
			// spec（descriptorheaps.adoc :1664-1665 + VUID 11253）：独立 OpTypeSampler 的
			// sampler heap 偏移写 heapOffset（"mapped resource or sampler"）；samplerHeapOffset
			// 字段仅组合 image sampler 用（:1671-1674）——2026-08-27 交叉测试定位的字段误用
			//（spike 首版同款，docs/heap_texture_research.md §九.2 根因 2）
			add(2, bindings.samplerHeapOffset, 0);
		}
		if (bindings.hasSceneData)
			add(3, bindings.slotSceneData * descriptorSize, descriptorSize);
		if (bindings.hasEnv) {
			// Phase 5.5：binding 4 → env cubemap（resource heap SAMPLED_IMAGE 槽）；
			// binding 5 → sky sampler（sampler heap 字节偏移，OpTypeSampler 映射规则同 binding 2）
			add(4, bindings.slotEnv * descriptorSize, descriptorSize);
			add(5, bindings.envSamplerHeapOffset, 0);
		}
		const uint32_t mappingCount = n;

		VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{};
		mappingInfo.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT;
		mappingInfo.mappingCount = mappingCount;
		mappingInfo.pMappings = mappings;

		VkShaderCreateInfoEXT meshCreateInfoRaw{};
		meshCreateInfoRaw.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
		meshCreateInfoRaw.pNext = &mappingInfo;
		meshCreateInfoRaw.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT | VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT;
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
	// 绑定 mesh + frag shader object 到命令缓冲（未使用的传统 stage 传 VK_NULL_HANDLE）。
	// 只做 bindShadersEXT——动态状态由 applyDynamicState 单独设置（调用点在 Renderer）。
	void bindShaders(vk::raii::CommandBuffer& cmd) const {
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
	}
};

}  // namespace RGL