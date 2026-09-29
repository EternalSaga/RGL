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
		const uint32_t sceneSlot = sceneData.ensure();
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
		if (const auto* envAsset = env.get()) {
			b.hasEnv = true;
			b.slotEnv = envAsset->heapSlot;
			b.envSamplerHeapOffset =
				env.ensureSkySampler(envAsset->mipLevels) * res.getSamplerHeap().getDescriptorSize();
		}
		// Phase 8 ③a：normal（binding 6）+ ORM（binding 7）贴图槽；Phase 8 ③：emissive
		//（binding 8）贴图槽（resource heap SAMPLED_IMAGE，extra 追加，仿 addLightEntry 的
		// extra 用法；不 import model.slang 或 Slang 剔除未用全局的 frag 不声明 = 映射空转，
		// 同上既证行为——deferred_geometry 不声明 binding 8 亦无害空转）
		b.extra.push_back({.binding = 6, .sampler = false, .heapSlot = normalSlot});
		b.extra.push_back({.binding = 7, .sampler = false, .heapSlot = ormSlot});
		b.extra.push_back({.binding = 8, .sampler = false, .heapSlot = emissiveSlot});
		auto entry = PipelineEntry{
			.name = fmt::format("{}.v{}.t{}.n{}.o{}.e{}", specName, vertexSlot, textureSlot, normalSlot,
				ormSlot, emissiveSlot),
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b),
			.spec = *spec, .bindings = b, .modelEntry = true};
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

	// 克隆条目定义（Phase 10 批 10.5 picking / 10.7 highlight，契约见接口注释）：model 族
	// 条目逐个 (mesh = 原 mesh SPV + frag = 传入的克隆 frag)，bindings 原样——克隆 frag
	// 不声明的 descriptor 映射空转无害（skybox 条目"映射未消费"既证方向）。
	void ShaderRegistry::addCloneEntries(ClonePurpose purpose, const std::string& fragSpvPath) {
		// 用途后缀 = 克隆条目名与 spec 名的区分标记（picking / highlight）
		const char* const suffix = purpose == ClonePurpose::picking ? "picking" : "highlight";
		auto fragResult = RGL::loadShaderSpv(fragSpvPath);
		if (!fragResult)
			throw std::runtime_error(fmt::format("clone frag SPV load failed {}: {}",
				fragSpvPath, fragResult.error().describe()));
		// 同 mesh SPV 只读一次（model 条目共享 model.mesh.spv：几十条目 → 1 次读盘）
		std::map<std::string, std::vector<uint32_t>> meshCache;
		// remap 第一维按用途索引（resize 覆盖「本用途首次装配」与「更高用途先装配」两种序）
		const size_t purposeIdx = static_cast<size_t>(purpose);
		if (cloneRemap.size() <= purposeIdx)
			cloneRemap.resize(purposeIdx + 1);
		std::vector<uint32_t>& remap = cloneRemap[purposeIdx];
		remap.assign(entries.size(), kNoClone);
		uint32_t clones = 0;
		for (uint32_t i = 0; i < entries.size(); ++i) {
			if (!entries[i].modelEntry)
				continue;  // cube/天空盒/光 = mesh 输出接口非 ModelVertexOutput，克隆 frag 不兼容
			auto it = meshCache.find(entries[i].spec.meshSpv);
			if (it == meshCache.end()) {
				auto meshResult = RGL::loadShaderSpv(entries[i].spec.meshSpv);
				if (!meshResult)
					throw std::runtime_error(fmt::format("clone mesh SPV load failed: {}",
						entries[i].spec.meshSpv));
				it = meshCache.emplace(entries[i].spec.meshSpv, std::move(*meshResult)).first;
			}
			RGL::ShaderEffect effect{.meshStageSpv = it->second, .fragStageSpv = *fragResult};
			auto clone = PipelineEntry{
				.name = entries[i].name + "." + suffix,
				.pipeline = RGL::RenderPipeline(device.getDevice(), effect, entries[i].bindings),
				// spec = 克隆真实来源对（热重载基线读回 = 同一对 SPV）；name 不参与查找
				.spec = RGL::PipelineSpec{.name = entries[i].spec.name + "_" + suffix,
					.meshSpv = entries[i].spec.meshSpv, .fragSpv = fragSpvPath},
				.bindings = entries[i].bindings};
			remap[i] = static_cast<uint32_t>(entries.size());
			entries.push_back(std::move(clone));
			++clones;
		}
		if (auto* logger = RLLogger::getInstance())
			logger->info("clone[{}]: {} 条管线克隆（model 族条目 × {}）", suffix, clones,
				fragSpvPath);
	}

	uint32_t ShaderRegistry::addSkyEntry() {
		if (auto idx = pipelineIndex("skybox"))
			return *idx;
		const RGL::PipelineSpec* spec = findSpec("skybox");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: skybox");
		const auto* envAsset = env.get();
		if (!envAsset)
			throw std::runtime_error("skybox entry requires env cubemap uploaded");
		RGL::PipelineBindings b{};
		b.descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		b.slotVertex = 0;
		b.slotIndex = 1;
		b.slotSceneData = sceneData.ensure();
		b.hasSceneData = true;
		b.hasEnv = true;
		b.slotEnv = envAsset->heapSlot;
		b.envSamplerHeapOffset =
			env.ensureSkySampler(envAsset->mipLevels) * res.getSamplerHeap().getDescriptorSize();
		auto entry = PipelineEntry{.name = "skybox",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b),
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: skybox (env slot {}) → 下标 {}", b.slotEnv, index);
		return index;
	}

	uint32_t ShaderRegistry::addLightEntry(uint32_t albedoSlot, uint32_t normalSlot,
			uint32_t positionSlot, uint32_t metalRoughSlot, uint32_t emissiveSlot,
			uint32_t samplerSlot) {
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
		b.slotSceneData = sceneData.ensure();  // binding 3：readSceneLight 单灯
		b.hasSceneData = true;
		const uint32_t samplerHeapOffset =
			samplerSlot * res.getSamplerHeap().getDescriptorSize();
		// extra：4 G-buffer RT 采样槽（resource heap，4-7 = kGBufferAttachments 前 4 序）
		// + 共享 sampler（sampler heap，8）+ emissive 第 5 RT（Phase 8 批次 B，排尾 11——
		// 4-10 已被旧布局占用）
		b.extra = {
			{.binding = 4, .sampler = false, .heapSlot = albedoSlot},
			{.binding = 5, .sampler = false, .heapSlot = normalSlot},
			{.binding = 6, .sampler = false, .heapSlot = positionSlot},
			{.binding = 7, .sampler = false, .heapSlot = metalRoughSlot},
			{.binding = 8, .sampler = true, .samplerHeapOffset = samplerHeapOffset},
		};
		// 批次 2 refine：env cubemap（9）+ sky sampler（10）——本 pass 低段 binding 被
		// G-buffer 连号占用，env 排尾（light.frag envMapL/envSamplerL）。env 已上传 = 正常绑定；
		// 未上传 = 占位槽 0（shader 端 mipRange=0 不采样，黑天/常 ambient 回落，见 light.frag）。
		if (const auto* envAsset = env.get()) {
			b.extra.push_back({.binding = 9, .sampler = false, .heapSlot = envAsset->heapSlot});
			b.extra.push_back({.binding = 10, .sampler = true,
				.samplerHeapOffset = env.ensureSkySampler(envAsset->mipLevels) *
					res.getSamplerHeap().getDescriptorSize()});
		} else {
			b.extra.push_back({.binding = 9, .sampler = false, .heapSlot = 0});
			b.extra.push_back({.binding = 10, .sampler = true, .samplerHeapOffset = 0});
		}
		b.extra.push_back({.binding = 11, .sampler = false, .heapSlot = emissiveSlot});
		auto entry = PipelineEntry{.name = "light",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b),
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: light (gbuffer slots {}/{}/{}/{}/{}, env {}) → 下标 {}",
				albedoSlot, normalSlot, positionSlot, metalRoughSlot, emissiveSlot,
				env.get() ? "on" : "off", index);
		return index;
	}

	// Phase 8 批次 ①：tonemap pass 条目（仿 addLightEntry 单条目形态）。import scene
	// 仅读 @56 曝光（configs/scene.lua exposure），hasSceneData = true；extra 仅
	// scene_hdr 采样槽 + 共享 sampler。执行器 drawLight 分支按 state 名 "tonemap" 取本条目。
	uint32_t ShaderRegistry::addTonemapEntry(uint32_t sceneHdrSlot, uint32_t aoSlot,
		uint32_t samplerSlot) {
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
		b.slotSceneData = sceneData.ensure();  // binding 3：@56 曝光 + @60 AO 强度
		b.hasSceneData = true;
		const uint32_t samplerHeapOffset =
			samplerSlot * res.getSamplerHeap().getDescriptorSize();
		b.extra = {
			{.binding = 4, .sampler = false, .heapSlot = sceneHdrSlot},
			{.binding = 5, .sampler = true, .samplerHeapOffset = samplerHeapOffset},
			// Phase 11 批 11.4：ao 遮蔽量（施加强度 0 时逐位旁路，绑定恒在）
			{.binding = 6, .sampler = false, .heapSlot = aoSlot},
		};
		auto entry = PipelineEntry{.name = "tonemap",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b),
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug(
				"pipeline entry: tonemap (scene_hdr slot {}, ao slot {}) → 下标 {}",
				sceneHdrSlot, aoSlot, index);
		return index;
	}

	uint32_t ShaderRegistry::addAoEntry(uint32_t depthSlot, uint32_t samplerSlot) {
		if (auto idx = pipelineIndex("ao"))
			return *idx;
		const RGL::PipelineSpec* spec = findSpec("ao");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: ao");
		const uint32_t descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		RGL::PipelineBindings b{};
		b.descriptorSize = descriptorSize;
		b.slotVertex = 0;   // binding 0/1 占位（ao mesh = light.mesh 全屏大三角，不声明顶点）
		b.slotIndex = 1;
		b.slotSceneData = sceneData.ensure();  // binding 3：@12 eyePos + @64 invViewProj
		b.hasSceneData = true;
		const uint32_t samplerHeapOffset =
			samplerSlot * res.getSamplerHeap().getDescriptorSize();
		b.extra = {
			// 深度图（eDepth aspect，usage 含 eSampled——见 Resources::createDepthImage）
			{.binding = 4, .sampler = false, .heapSlot = depthSlot},
			{.binding = 5, .sampler = true, .samplerHeapOffset = samplerHeapOffset},
		};
		auto entry = PipelineEntry{.name = "ao",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b),
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: ao (depth slot {}) → 下标 {}", depthSlot, index);
		return index;
	}

	/** addShadowEntry 实现（契约见接口注释）。去重键 = vertexSlot。 */
	uint32_t ShaderRegistry::addShadowEntry(uint32_t vertexSlot) {
		if (const auto it = shadowEntryBySlot.find(vertexSlot); it != shadowEntryBySlot.end())
			return it->second;
		const RGL::PipelineSpec* spec = findSpec("shadow");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: shadow");
		RGL::PipelineBindings b{};
		b.descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		b.slotVertex = vertexSlot;  // binding 0 = 顶点 buffer 槽（modelVertexBuffer 真正消费）
		b.slotIndex = vertexSlot;   // binding 1 = 占位（shadow mesh 不声明 = 空转，同 light/ao 条目做法）
		b.slotSceneData = sceneData.ensure();  // binding 3 = scene data（VP 区/对象矩阵/ObjectDraw）
		b.hasSceneData = true;
		// textured=false → 不烘焙 binding 1 的 baseColor 形态（上面 slotIndex 占位即此）；
		// 不烘焙 env（612）、不纹理纹理槽（6-8）——两个 shader 文件均未声明它们。
		auto entry = PipelineEntry{.name = fmt::format("shadow.v{}", vertexSlot),
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b),
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		shadowEntryBySlot[vertexSlot] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: shadow.v{} (vertex slot {}) → 下标 {}", vertexSlot,
				vertexSlot, index);
		return index;
	}

	void ShaderRegistry::bindShadowToAsset(uint32_t assetId, uint32_t entryIdx) {
		assets.bindShadowPipeline(assetId, entryIdx);
	}

	uint32_t ShaderRegistry::addCubeEntry(uint32_t meshAssetId) {
		if (auto idx = pipelineIndex("cube"))
			return *idx;
		const RGL::PipelineSpec* cubeSpec = findSpec("cube");
		if (!cubeSpec)
			throw std::runtime_error("pipeline spec not found: cube");
		const auto& cubeAsset = assets.get(meshAssetId);
		if (!cubeAsset.indexHeapSlot)
			throw std::runtime_error("addCubeEntry: asset is not an indexed (builtin) mesh");
		RGL::PipelineBindings b{};
		b.descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		b.slotVertex = cubeAsset.heapSlot;
		b.slotIndex = *cubeAsset.indexHeapSlot;
		// Phase 7b+（NV DGC 规避）：cube mesh 也走 readObjectMvp（binding 3 scene data，
		// 同 model 条目恒烘焙；cube 的 mvp 由 render_list 循环填进 mvp 表，objectIndex 有效）
		b.slotSceneData = sceneData.ensure();
		b.hasSceneData = true;
		auto entry = PipelineEntry{
			.name = "cube",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*cubeSpec), b),
			.spec = *cubeSpec, .bindings = b};
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
		assets.setPipelineIndex(assetId, *idx);
	}

}  // namespace RGL
