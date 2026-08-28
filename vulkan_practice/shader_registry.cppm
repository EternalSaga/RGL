module;
#include <vulkan/vulkan_raii.hpp>
export module shader_registry;

import rlstd;
import rllogger;

import vk_device;
import vk_resources;
import shader;
import scene_config;
import material_types_generated;
import vkerror;

namespace RGL {

// 管线表一条（Phase 4 批次 2：从 vkrenderer.cppm 迁入，Vulkan 层零 entt）：
// shader 风格名 + 其 RenderPipeline（shader object + 烘焙的 descriptor heap binding mapping）
// + 显式 push constant 元数据（L5 解决：argsSize/pushDataSet 替代旧 "set 0 被两种 struct
// 隐式共享"）。argsSize = 该管线 push constant 字节数（统一 DrawArgs 超集 = 96B scalar 布局，
// material_types_generated static_assert 与 shader 反射一致）；pushDataSet = 0（显式）。
export struct PipelineEntry {
	std::string name;
	RGL::RenderPipeline pipeline;
	uint32_t argsSize = sizeof(RGL::DrawArgs);
	uint32_t pushDataSet = 0;
};

// 管线表（Phase 4：从 Renderer 迁出，零 entt——只认 slot/资产 id，不认场景实体）：
//   "cube" 条目恒建（批次 3：画不画 = cube entity 有无，slot 经 cube 资产记录取）；
//   模型条目资产上传期经 addModelEntry 动态加入，按 (风格, 顶点 slot, 纹理 slot) 组合去重
//   ——binding mapping 是 shader object 级状态（无法运行时换），同组合的映射恒等，
//   故共享一条条目而非每 primitive 一条（Phase 4 验收：392 primitive → 12 条）。
// 渲染 system 经资产记录的 pipelineIndex（数值）取条目，录制路径零字符串查找（E2.4）。
// 条目下标写回资产记录经 bindToAsset / bindPipelineToAsset（启动期一次）。
export class ShaderRegistry {
   public:
	// 构造即建 "cube" 条目（需 Resources 的 cube 资产 + descriptor heap）；specs 来自
	// configs/scene.lua（name → SPV 路径），按 name 索引。
	ShaderRegistry(RGL::DeviceContext& device, RGL::Resources& res,
		std::span<const RGL::PipelineSpec> specs)
		: device(device),
		  res(res) {
		for (const auto& spec : specs)
			styles[spec.name] = spec;
		addCubeEntry();
	}

	// 条目名 → 下标（启动期解析用：bindPipelineToAsset / 防御检查；每帧路径经资产
	// 记录的 pipelineIndex 直接取，零字符串查找）
	[[nodiscard]] std::optional<uint32_t> pipelineIndex(std::string_view name) const {
		auto it = indexByName.find(std::string(name));
		return it == indexByName.end() ? std::nullopt : std::optional<uint32_t>(it->second);
	}
	// 下标 → 条目（Renderer draw 循环取整条目：pushDataSet + pipeline）
	[[nodiscard]] const PipelineEntry& entry(uint32_t index) const { return entries[index]; }
	// 下标 → 管线（llm_debug 离屏重渲经 Renderer::pipelineAt 委托到此）
	[[nodiscard]] const RGL::RenderPipeline& pipelineAt(uint32_t index) const { return entries[index].pipeline; }

	// 模型管线条目（资产上传期）：用管线规格创建 RenderPipeline，binding mapping 显式烘焙
	// 顶点 slot + 纹理 slot（binding 1 = resource heap 的 SAMPLED_IMAGE 描述符槽、
	// binding 2 = sampler heap 槽——OpTypeSampler 映射，共享一份 sampler 描述符，
	// 偏移 = samplerSlot * samplerHeap.getDescriptorSize()）。
	// 首参 specName = 管线规格名（Phase 4 批次 3：风格→规格名映射在 main.cpp 经 scene.lua
	// styles 表完成，本函数只认规格名）；按 (规格, 顶点 slot, 纹理 slot) 组合去重——
	// 同组合返回既有条目下标（不重复建 shader object）；新组合建条目并记入 indexByName /
	// 去重表。返回条目下标，调用方经 bindToAsset 写回资产记录（renderListSystem 经资产表
	// 解析管线，无字符串约定）。失败（规格缺失 / SPV 加载失败等）抛出（同构造期路径）。
	[[nodiscard]] uint32_t addModelEntry(const std::string& specName, uint32_t vertexSlot,
		uint32_t textureSlot, uint32_t samplerSlot) {
		const auto key = std::make_tuple(specName, vertexSlot, textureSlot);
		// DIAG（Phase 4 批次 2 判别）：RGL_NO_DEDUP=1 禁用去重（每 primitive 独立条目，不共享
		// shader object）——测"共享 shader object（dedup）"是否是 artifact 根因
		if (std::getenv("RGL_NO_DEDUP") == nullptr) {
			if (auto it = modelEntryBySlot.find(key); it != modelEntryBySlot.end())
				return it->second;
		}

		const RGL::PipelineSpec* spec = findSpec(specName.c_str());
		if (!spec)
			throw std::runtime_error("pipeline spec not found: " + specName);
		const uint32_t descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		const uint32_t samplerHeapOffset =
			samplerSlot * res.getSamplerHeap().getDescriptorSize();
		auto entry = PipelineEntry{
			specName + ".v" + std::to_string(vertexSlot) + ".t" + std::to_string(textureSlot),
			RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), descriptorSize,
				vertexSlot, vertexSlot, textureSlot, samplerHeapOffset, true)};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		modelEntryBySlot[key] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: {} (vertex slot {} / texture slot {}) → 下标 {}",
				specName, vertexSlot, textureSlot, index);
		return index;
	}

	// 条目下标写回资产记录（启动期一次）
	void bindToAsset(uint32_t assetId, uint32_t entryIndex) {
		res.getAssetTable().setPipelineIndex(assetId, entryIndex);
	}
	// 条目名 → 下标写回资产记录（启动期一次；条目缺失 = 配置/构建不一致，直接报错）
	void bindPipelineToAsset(const char* name, uint32_t assetId) {
		auto idx = pipelineIndex(name);
		if (!idx)
			throw std::runtime_error(std::string("pipeline entry not found: ") + name);
		res.getAssetTable().setPipelineIndex(assetId, *idx);
	}

   private:
	// "cube" 管线条目（恒建——画不画 = cube entity 有无，数据驱动；slot 经 cube 资产
	// 记录取，布局由反射生成的偏移常量钉死 vertex=0/index=1）
	void addCubeEntry() {
		const RGL::PipelineSpec* cubeSpec = findSpec("cube");
		if (!cubeSpec)
			throw std::runtime_error("pipeline spec not found: cube");
		const auto& cubeAsset = res.getAssetTable().get(res.cubeAssetId());
		const uint32_t descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		auto entry = PipelineEntry{
			"cube",
			RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*cubeSpec), descriptorSize,
				cubeAsset.heapSlot, *cubeAsset.indexHeapSlot)};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
	}

	// 把一条 pipeline 规格对应的 mesh/fragment SPV 载入 ShaderEffect（SPV 路径来自 SceneConfig）
	static RGL::ShaderEffect loadShaderEffect(const RGL::PipelineSpec& spec) {
		auto meshResult = RGL::loadShaderSpv(spec.meshSpv);
		if (!meshResult) throw std::runtime_error(meshResult.error().describe());
		auto fragResult = RGL::loadShaderSpv(spec.fragSpv);
		if (!fragResult) throw std::runtime_error(fragResult.error().describe());

		RGL::ShaderEffect effect;
		effect.meshStageSpv = std::move(*meshResult);
		effect.fragStageSpv = std::move(*fragResult);
		return effect;
	}

	// 按名查找 pipeline 规格（scene.lua 缺条目时回落默认 SPV 路径，同原 Renderer 行为）
	const RGL::PipelineSpec* findSpec(const char* name) const {
		auto it = styles.find(name);
		if (it != styles.end())
			return &it->second;
		if (std::strcmp(name, "cube") == 0) {
			static const RGL::PipelineSpec defaultCube{
				"cube", "generated_shaders/cube.mesh.spv", "generated_shaders/cube.frag.spv"};
			return &defaultCube;
		}
		if (std::strcmp(name, "model") == 0) {
			static const RGL::PipelineSpec defaultModel{
				"model", "generated_shaders/model.mesh.spv", "generated_shaders/model.frag.spv"};
			return &defaultModel;
		}
		if (std::strcmp(name, "model_pbr") == 0) {
			static const RGL::PipelineSpec defaultModelPbr{
				"model_pbr", "generated_shaders/model.mesh.spv",
				"generated_shaders/model_pbr.frag.spv"};
			return &defaultModelPbr;
		}
		return nullptr;
	}

	RGL::DeviceContext& device;
	RGL::Resources& res;
	std::map<std::string, RGL::PipelineSpec> styles;          // 风格名 → SPV 规格（scene.lua）
	std::map<std::string, uint32_t> indexByName;              // 条目名 → 下标（启动期 name 解析）
	std::map<std::tuple<std::string, uint32_t, uint32_t>, uint32_t> modelEntryBySlot;  // 去重表
	std::vector<PipelineEntry> entries;                       // 管线表（下标 = 绘制契约）
};

}  // namespace RGL
