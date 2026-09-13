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

namespace RGL {

// addModelEntry/addSkyEntry/addCubeEntry/loadShaderEffect 定义（2026-09-02 编译提速
// A 方案）：SPV 加载 + 管线创建函数体移出接口 TU；本单元是叶子边。

	uint32_t ShaderRegistry::addModelEntry(const std::string& specName, uint32_t vertexSlot,
			uint32_t textureSlot, uint32_t normalSlot, uint32_t ormSlot, uint32_t emissiveSlot,
			uint32_t samplerSlot) {
		const auto key = std::make_tuple(specName, vertexSlot, textureSlot, normalSlot, ormSlot, emissiveSlot);
		// 按 (规格, 顶点 slot, base 纹理 slot, normal slot, orm slot, emissive slot) 六元组组合
		// 去重：同组合返回既有条目（不重复建 shader object）
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
		// Phase 8 ③a：normal（binding 6）+ ORM（binding 7）贴图槽；Phase 8 ③：emissive
		//（binding 8）贴图槽（resource heap SAMPLED_IMAGE，extra 追加，仿 addLightEntry 的
		// extra 用法；不 import model.slang 或 Slang 剔除未用全局的 frag 不声明 = 映射空转，
		// 同上既证行为——deferred_geometry 不声明 binding 8 亦无害空转）
		b.extra.push_back({.binding = 6, .sampler = false, .heapSlot = normalSlot});
		b.extra.push_back({.binding = 7, .sampler = false, .heapSlot = ormSlot});
		b.extra.push_back({.binding = 8, .sampler = false, .heapSlot = emissiveSlot});
		auto entry = PipelineEntry{
			fmt::format("{}.v{}.t{}.n{}.o{}.e{}", specName, vertexSlot, textureSlot, normalSlot,
				ormSlot, emissiveSlot),
			RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b)};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		modelEntryBySlot[key] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: {} (vertex slot {} / base slot {} / normal slot {} / "
				"orm slot {} / emissive slot {}) → 下标 {}",
				specName, vertexSlot, textureSlot, normalSlot, ormSlot, emissiveSlot, index);
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
		// 批次 2 refine：env cubemap（9）+ sky sampler（10）——本 pass 的 4/5 已被 G-buffer
		// 占用，env 走 9/10（light.frag envMapL/envSamplerL）。env 已上传 = 正常绑定；未上传
		// = 占位槽 0（shader 端 mipRange=0 不采样，黑天/常 ambient 回落，见 light.frag）。
		if (const auto* env = res.getEnvTexture()) {
			b.extra.push_back({.binding = 9, .sampler = false, .heapSlot = env->heapSlot});
			b.extra.push_back({.binding = 10, .sampler = true,
				.samplerHeapOffset = res.ensureSkySampler(env->mipLevels) *
					res.getSamplerHeap().getDescriptorSize()});
		} else {
			b.extra.push_back({.binding = 9, .sampler = false, .heapSlot = 0});
			b.extra.push_back({.binding = 10, .sampler = true, .samplerHeapOffset = 0});
		}
		auto entry = PipelineEntry{"light",
			RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b)};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: light (gbuffer slots {}/{}/{}/{}, env {}) → 下标 {}",
				albedoSlot, normalSlot, positionSlot, metalRoughSlot,
				res.getEnvTexture() ? "on" : "off", index);
		return index;
	}

	// Phase 8 批次 ①：tonemap pass 条目（仿 addLightEntry 单条目形态）。import scene
	// 仅读 @56 曝光（configs/scene.lua exposure），hasSceneData = true；extra 仅
	// scene_hdr 采样槽 + 共享 sampler。执行器 drawLight 分支按 state 名 "tonemap" 取本条目。
	uint32_t ShaderRegistry::addTonemapEntry(uint32_t sceneHdrSlot, uint32_t samplerSlot) {
		if (auto idx = pipelineIndex("tonemap"))
			return *idx;
		const RGL::PipelineSpec* spec = findSpec("tonemap");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: tonemap");
		const uint32_t descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		RGL::PipelineBindings b{};
		b.descriptorSize = descriptorSize;
		b.slotVertex = 0;   // binding 0/1 占位（tonemap mesh = light.mesh 全屏大三角，不声明顶点）
		b.slotIndex = 1;
		b.slotSceneData = res.ensureSceneData();  // binding 3：@56 曝光
		b.hasSceneData = true;
		const uint32_t samplerHeapOffset =
			samplerSlot * res.getSamplerHeap().getDescriptorSize();
		b.extra = {
			{.binding = 4, .sampler = false, .heapSlot = sceneHdrSlot},
			{.binding = 5, .sampler = true, .samplerHeapOffset = samplerHeapOffset},
		};
		auto entry = PipelineEntry{"tonemap",
			RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b)};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: tonemap (scene_hdr slot {}) → 下标 {}", sceneHdrSlot,
				index);
		return index;
	}

	uint32_t ShaderRegistry::addCubeEntry(uint32_t meshAssetId) {
		if (auto idx = pipelineIndex("cube"))
			return *idx;
		const RGL::PipelineSpec* cubeSpec = findSpec("cube");
		if (!cubeSpec)
			throw std::runtime_error("pipeline spec not found: cube");
		const auto& cubeAsset = res.getAssetTable().get(meshAssetId);
		if (!cubeAsset.indexHeapSlot)
			throw std::runtime_error("addCubeEntry: asset is not an indexed (builtin) mesh");
		RGL::PipelineBindings b{};
		b.descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		b.slotVertex = cubeAsset.heapSlot;
		b.slotIndex = *cubeAsset.indexHeapSlot;
		// Phase 7b+（NV DGC 规避）：cube mesh 也走 readObjectMvp（binding 3 scene data，
		// 同 model 条目恒烘焙；cube 的 mvp 由 render_list 循环填进 mvp 表，objectIndex 有效）
		b.slotSceneData = res.ensureSceneData();
		b.hasSceneData = true;
		auto entry = PipelineEntry{
			"cube",
			RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*cubeSpec), b)};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: cube (vertex slot {} / index slot {}) → 下标 {}",
				b.slotVertex, b.slotIndex, index);
		return index;
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
