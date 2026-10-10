module;
/**
 * shader_registry:fullscreen —— 全屏后处理 pass 条目组（tonemap / ao / velocity /
 * ao_apply / taa / ssr）实现单元（批 15.1 前半自 :impl 拆出，先例 = vk_renderer:offgraph）。
 *
 * 拆的 cohesion 判据：这六个 add*Entry 是**同一形状**的三件套——全屏 drawLight pass、
 * 单条目去重（pipelineIndex(name) 早退）、extra = 烘死堆槽、无 mesh 顶点。与 :impl 里
 * 「按资产/克隆批量建条目」的 addModelEntry / addCloneEntries / addShadowEntry 是两个
 * 变化面（后者按 slot/元组去重、可多条目）。
 */
module shader_registry:fullscreen;

import shader_registry;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vk_device;
import vk_resources;
import shader;

namespace RGL {

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

	/** addVelocityEntry 实现（契约见接口注释；批 14.4，仿 addAoEntry，少一个 sampler 绑定）。 */
	uint32_t ShaderRegistry::addVelocityEntry(uint32_t depthSlot) {
		if (auto idx = pipelineIndex("velocity"))
			return *idx;
		const RGL::PipelineSpec* spec = findSpec("velocity");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: velocity");
		const uint32_t descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		RGL::PipelineBindings b{};
		b.descriptorSize = descriptorSize;
		b.slotVertex = 0;   // binding 0/1 占位（velocity mesh = light.mesh 全屏大三角）
		b.slotIndex = 1;
		b.slotSceneData = sceneData.ensure();  // binding 3：@64 + TAA 尾区
		b.hasSceneData = true;
		b.extra = {
			// 深度图（eDepth aspect）；本 shader 用 Load 直取 ⟹ 无 sampler 绑定
			{.binding = 4, .sampler = false, .heapSlot = depthSlot},
		};
		auto entry = PipelineEntry{.name = "velocity",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b),
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: velocity (depth slot {}) → 下标 {}", depthSlot, index);
		return index;
	}

	/** addTaaEntry 实现（契约见接口注释；批 12.6，仿 addTonemapEntry 三步曲）。 */
	uint32_t ShaderRegistry::addAoApplyEntry(uint32_t colorSlot, uint32_t aoSlot,
		uint32_t samplerSlot) {
		if (auto idx = pipelineIndex("ao_apply"))
			return *idx;
		const RGL::PipelineSpec* spec = findSpec("ao_apply");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: ao_apply");
		const uint32_t descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		RGL::PipelineBindings b{};
		b.descriptorSize = descriptorSize;
		b.slotVertex = 0;   // binding 0/1 占位（mesh = light.mesh 全屏大三角，不声明顶点）
		b.slotIndex = 1;
		b.slotSceneData = sceneData.ensure();  // binding 3：@60 AO 强度 + @722208 c_t
		b.hasSceneData = true;
		const uint32_t samplerHeapOffset =
			samplerSlot * res.getSamplerHeap().getDescriptorSize();
		b.extra = {
			{.binding = 4, .sampler = false, .heapSlot = colorSlot},
			{.binding = 5, .sampler = true, .samplerHeapOffset = samplerHeapOffset},
			{.binding = 6, .sampler = false, .heapSlot = aoSlot},
		};
		auto entry = PipelineEntry{.name = "ao_apply",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b),
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		return index;
	}

uint32_t ShaderRegistry::addTaaEntry(uint32_t colorSlot, uint32_t historySlot,
			uint32_t depthSlot, uint32_t samplerSlot, uint32_t pointSlot, uint32_t aoSlot) {
		if (auto idx = pipelineIndex("taa"))
			return *idx;
		const RGL::PipelineSpec* spec = findSpec("taa");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: taa");
		const uint32_t descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		RGL::PipelineBindings b{};
		b.descriptorSize = descriptorSize;
		b.slotVertex = 0;   // binding 0/1 占位（taa mesh = light.mesh.spv 全屏大三角）
		b.slotIndex = 1;
		b.slotSceneData = sceneData.ensure();  // binding 3：@722080 起 TAA 尾区 + @64 重建
		b.hasSceneData = true;
		const uint32_t samplerHeapOffset =
			samplerSlot * res.getSamplerHeap().getDescriptorSize();
		const uint32_t pointHeapOffset =
			pointSlot * res.getSamplerHeap().getDescriptorSize();
		b.extra = {
			{.binding = 4, .sampler = false, .heapSlot = colorSlot},
			{.binding = 5, .sampler = false, .heapSlot = historySlot},
			{.binding = 6, .sampler = false, .heapSlot = depthSlot},
			{.binding = 7, .sampler = true, .samplerHeapOffset = samplerHeapOffset},
			{.binding = 8, .sampler = true, .samplerHeapOffset = pointHeapOffset},
			// 批 13.6：AO 施加点搬进本 pass（时域滤波之前）——与 tonemap 条目复用同一个 ao 槽
			{.binding = 9, .sampler = false, .heapSlot = aoSlot},
		};
		auto entry = PipelineEntry{.name = "taa",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b),
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: taa (color slot {}, history slot {}) → 下标 {}",
				colorSlot, historySlot, index);
		return index;
	}

	/** addSsrEntry 实现（契约见接口注释；批 15.1 前半，仿 addVelocityEntry：全 Load 无 sampler）。 */
	uint32_t ShaderRegistry::addSsrEntry(uint32_t colorSlot, uint32_t normalSlot,
			uint32_t positionSlot, uint32_t metalRoughSlot, uint32_t depthSlot) {
		if (auto idx = pipelineIndex("ssr"))
			return *idx;
		const RGL::PipelineSpec* spec = findSpec("ssr");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: ssr");
		const uint32_t descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		RGL::PipelineBindings b{};
		b.descriptorSize = descriptorSize;
		b.slotVertex = 0;   // binding 0/1 占位（mesh = light.mesh 全屏大三角，不声明顶点）
		b.slotIndex = 1;
		b.slotSceneData = sceneData.ensure();  // binding 3：@722272 ssrViewProj（消费者 = 15.1 ray march）
		b.hasSceneData = true;
		// 全 `Load` 直取 ⟹ 无 sampler 绑定（同 addVelocityEntry：不占 sampler 堆槽 =
		// 不参与"自己算偏移"的事故面）。G-buffer 通道号镜像 light.frag（5=normal/6=position/
		// 7=metalrough），15.1 接线时 shader 侧 binding 面零改动。
		b.extra = {
			{.binding = 4, .sampler = false, .heapSlot = colorSlot},
			{.binding = 5, .sampler = false, .heapSlot = normalSlot},
			{.binding = 6, .sampler = false, .heapSlot = positionSlot},
			{.binding = 7, .sampler = false, .heapSlot = metalRoughSlot},
			{.binding = 8, .sampler = false, .heapSlot = depthSlot},
		};
		auto entry = PipelineEntry{.name = "ssr",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b),
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: ssr (color slot {}, gbuffer {}/{}/{}, depth slot {}) → 下标 {}",
				colorSlot, normalSlot, positionSlot, metalRoughSlot, depthSlot, index);
		return index;
	}

}  // namespace RGL
