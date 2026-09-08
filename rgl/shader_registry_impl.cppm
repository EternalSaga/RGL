module;
#include <fmt/format.h>
module shader_registry:impl;

import shader_registry;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vk_device;
import vk_resources;
import shader;
import scene_config;
import material_types_generated;
import vkerror;
import asset_table;
import types_generated;

namespace RGL {

// addModelEntry/addSkyEntry/addCubeEntry/loadShaderEffect 定义（2026-09-02 编译提速
// A 方案）：SPV 加载 + 管线创建函数体移出接口 TU；本单元是叶子边。

uint32_t ShaderRegistry::addModelEntry(const std::string& specName, uint32_t vertexSlot,
		uint32_t textureSlot, uint32_t samplerSlot) {
		const auto key = std::make_tuple(specName, vertexSlot, textureSlot);
		// 按 (规格, 顶点 slot, 纹理 slot) 组合去重：同组合返回既有条目（不重复建 shader object）
		if (auto it = modelEntryBySlot.find(key); it != modelEntryBySlot.end())
			return it->second;

		const RGL::PipelineSpec* spec = findSpec(specName.c_str());
		if (!spec)
			throw std::runtime_error(fmt::format("pipeline spec not found: {}", specName));
		const uint32_t descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		const uint32_t samplerHeapOffset =
			samplerSlot * res.getSamplerHeap().getDescriptorSize();
		// Phase 5 批次 2：模型管线恒绑 scene data（binding 3，幂等创建；unlit frag 不声明
		// = 映射空转，同 mesh 阶段对 binding 1/2 的既证行为）
		const uint32_t sceneSlot = res.ensureSceneData();
		RGL::PipelineBindings b{};
		b.descriptorSize = descriptorSize;
		b.slotVertex = vertexSlot;
		b.slotIndex = vertexSlot;
		b.slotTexture = textureSlot;
		b.samplerHeapOffset = samplerHeapOffset;
		b.slotSceneData = sceneSlot;
		b.textured = true;
		b.hasSceneData = true;
		// Phase 5.5：env cubemap 已上传 → 全模型条目烘焙 binding 4/5（env 槽全局单例
		// 恒等，不进组合去重 key；不 import env.slang 的 frag 不声明 = 映射空转，同上既证行为）
		if (const auto* env = res.getEnvTexture()) {
			b.hasEnv = true;
			b.slotEnv = env->heapSlot;
			b.envSamplerHeapOffset =
				res.ensureSkySampler(env->mipLevels) * res.getSamplerHeap().getDescriptorSize();
		}
		auto entry = PipelineEntry{
			fmt::format("{}.v{}.t{}", specName, vertexSlot, textureSlot),
			RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b)};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		modelEntryBySlot[key] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: {} (vertex slot {} / texture slot {}) → 下标 {}",
				specName, vertexSlot, textureSlot, index);
		return index;
	}

	uint32_t ShaderRegistry::addSkyEntry() {
		if (auto idx = pipelineIndex("skybox"))
			return *idx;
		const RGL::PipelineSpec* spec = findSpec("skybox");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: skybox");
		const auto* env = res.getEnvTexture();
		if (!env)
			throw std::runtime_error("skybox entry requires env cubemap uploaded");
		RGL::PipelineBindings b{};
		b.descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		b.slotVertex = 0;
		b.slotIndex = 1;
		b.slotSceneData = res.ensureSceneData();
		b.hasSceneData = true;
		b.hasEnv = true;
		b.slotEnv = env->heapSlot;
		b.envSamplerHeapOffset =
			res.ensureSkySampler(env->mipLevels) * res.getSamplerHeap().getDescriptorSize();
		auto entry = PipelineEntry{"skybox",
			RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b)};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: skybox (env slot {}) → 下标 {}", b.slotEnv, index);
		return index;
	}

	uint32_t ShaderRegistry::addLightEntry(uint32_t albedoSlot, uint32_t normalSlot,
			uint32_t positionSlot, uint32_t metalRoughSlot, uint32_t samplerSlot) {
		if (auto idx = pipelineIndex("light"))
			return *idx;
		const RGL::PipelineSpec* spec = findSpec("light");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: light");
		const uint32_t descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		RGL::PipelineBindings b{};
		b.descriptorSize = descriptorSize;
		b.slotVertex = 0;   // binding 0/1 占位（light mesh 不声明 = 空转，skybox 既证）
		b.slotIndex = 1;
		b.slotSceneData = res.ensureSceneData();  // binding 3：readSceneLight 单灯
		b.hasSceneData = true;
		const uint32_t samplerHeapOffset =
			samplerSlot * res.getSamplerHeap().getDescriptorSize();
		// extra：4 G-buffer RT 采样槽（resource heap）+ 共享 sampler（sampler heap）
		b.extra = {
			{.binding = 4, .sampler = false, .heapSlot = albedoSlot},
			{.binding = 5, .sampler = false, .heapSlot = normalSlot},
			{.binding = 6, .sampler = false, .heapSlot = positionSlot},
			{.binding = 7, .sampler = false, .heapSlot = metalRoughSlot},
			{.binding = 8, .sampler = true, .samplerHeapOffset = samplerHeapOffset},
		};
		auto entry = PipelineEntry{"light",
			RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b)};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: light (gbuffer slots {}/{}/{}/{}) → 下标 {}",
				albedoSlot, normalSlot, positionSlot, metalRoughSlot, index);
		return index;
	}

	void ShaderRegistry::addCubeEntry() {
		const RGL::PipelineSpec* cubeSpec = findSpec("cube");
		if (!cubeSpec)
			throw std::runtime_error("pipeline spec not found: cube");
		const auto& cubeAsset = res.getAssetTable().get(res.cubeAssetId());
		RGL::PipelineBindings b{};
		b.descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		b.slotVertex = cubeAsset.heapSlot;
		b.slotIndex = *cubeAsset.indexHeapSlot;
		auto entry = PipelineEntry{
			"cube",
			RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*cubeSpec), b)};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
	}

	RGL::ShaderEffect ShaderRegistry::loadShaderEffect(const RGL::PipelineSpec& spec) {
		auto meshResult = RGL::loadShaderSpv(spec.meshSpv);
		if (!meshResult) throw std::runtime_error(meshResult.error().describe());
		auto fragResult = RGL::loadShaderSpv(spec.fragSpv);
		if (!fragResult) throw std::runtime_error(fragResult.error().describe());

		RGL::ShaderEffect effect;
		effect.meshStageSpv = std::move(*meshResult);
		effect.fragStageSpv = std::move(*fragResult);
		return effect;
	}

void ShaderRegistry::bindPipelineToAsset(const char* name, uint32_t assetId) {
		auto idx = pipelineIndex(name);
		if (!idx)
			throw std::runtime_error(fmt::format("pipeline entry not found: {}", name));
		res.getAssetTable().setPipelineIndex(assetId, *idx);
	}

}  // namespace RGL
