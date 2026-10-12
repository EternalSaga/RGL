module;
/**
 * shader_registry:fullscreen —— 全屏后处理 pass 条目组（tonemap / ao / velocity /
 * ao_apply / taa / ssr_trace / ssr）实现单元（批 15.1 前半自 :impl 拆出，先例 = vk_renderer:offgraph；
 * 批 15.3-B：ssr 拆成 trace + apply 两条目）。
 *
 * 拆的 cohesion 判据：这些 add*Entry 是**同一形状**的三件套——全屏 drawLight pass、
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
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				"tonemap"),
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
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				"ao"),
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
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				"velocity"),
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
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				"ao_apply"),
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
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				"taa"),
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: taa (color slot {}, history slot {}) → 下标 {}",
				colorSlot, historySlot, index);
		return index;
	}

	/** addSsrTraceEntry 实现（契约见接口注释；批 15.1 后半补齐 binding 9/10：albedo + 命中取色 sampler；
	 *  批 15.2 子步 C 补齐 11/12：env cubemap + sky sampler，命中处减 IBL specular 的镜像重算源；
	 *  批 15.3-B 自 addSsrEntry 改名——march 体不变，模糊+合成拆给 addSsrApplyEntry）。 */
	uint32_t ShaderRegistry::addSsrTraceEntry(uint32_t colorSlot, uint32_t normalSlot,
			uint32_t positionSlot, uint32_t metalRoughSlot, uint32_t depthSlot,
			uint32_t albedoSlot, uint32_t samplerSlot) {
		if (auto idx = pipelineIndex("ssr_trace"))
			return *idx;
		const RGL::PipelineSpec* spec = findSpec("ssr_trace");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: ssr_trace");
		const uint32_t descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		RGL::PipelineBindings b{};
		b.descriptorSize = descriptorSize;
		b.slotVertex = 0;   // binding 0/1 占位（mesh = light.mesh 全屏大三角，不声明顶点）
		b.slotIndex = 1;
		b.slotSceneData = sceneData.ensure();  // binding 3：@722272 ssrViewProj（ray march 投回屏幕）
		b.hasSceneData = true;
		// 自身像素量全走 `Load`（不需要 sampler），binding 10 只服务命中点那**一次** `Sample`
		// ——命中 uv 是亚像素的，必须过滤。G-buffer 通道号镜像 light.frag（5=normal/6=position/
		// 7=metalrough/9=albedo）。
		// ⚠ sampler=true 的条目必须烘 **samplerHeapOffset（字节偏移）**而不是 heapSlot——
		// 消费端 shader.cppm 对 sampler 条目读 `e.samplerHeapOffset`（shader.cppm:488）。
		// 15.1 原样误写 `.heapSlot = samplerSlot`（偏移退化为 0）：本机 RADV reservedSlots=0
		// 且共享 sampler 恰在槽 0 ⟹ 碰巧正确、逐位判据全绿；NVIDIA reservedSlots=3024 ⟹
		// 命中取色 sampler 会绑进驱动保留区。形态照 addTaaEntry 同名偏移（:166）。
		const uint32_t samplerHeapOffset =
			samplerSlot * res.getSamplerHeap().getDescriptorSize();
		b.extra = {
			{.binding = 4, .sampler = false, .heapSlot = colorSlot},
			{.binding = 5, .sampler = false, .heapSlot = normalSlot},
			{.binding = 6, .sampler = false, .heapSlot = positionSlot},
			{.binding = 7, .sampler = false, .heapSlot = metalRoughSlot},
			{.binding = 8, .sampler = false, .heapSlot = depthSlot},
			{.binding = 9, .sampler = false, .heapSlot = albedoSlot},
			{.binding = 10, .sampler = true, .samplerHeapOffset = samplerHeapOffset},
		};
		// 批 15.2 子步 C：env cubemap（11）+ sky sampler（12）——env **不是图附件**，走
		// PipelineBindings.extra（light pass 9/10 同法），渲染图零改动。绑定形态照
		// addLightEntry（shader_registry_impl.cppm:210-218）：env 已上传 = 正常绑定（heapSlot
		// 烘资源堆槽、sampler 烘 sky sampler 字节偏移，maxLod = mipLevels-1 是 GGX mip 链
		// 唯一入口）；未上传 = 占位 0/0（shader 端 mipRange=0 恒不采样，见 ssr_trace.frag 减法块）。
		if (const auto* envAsset = env.get()) {
			b.extra.push_back({.binding = 11, .sampler = false, .heapSlot = envAsset->heapSlot});
			b.extra.push_back({.binding = 12, .sampler = true,
				.samplerHeapOffset = env.ensureSkySampler(envAsset->mipLevels) *
					res.getSamplerHeap().getDescriptorSize()});
		} else {
			b.extra.push_back({.binding = 11, .sampler = false, .heapSlot = 0});
			b.extra.push_back({.binding = 12, .sampler = true, .samplerHeapOffset = 0});
		}
		auto entry = PipelineEntry{.name = "ssr_trace",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				"ssr_trace"),
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: ssr_trace (color slot {}, gbuffer {}/{}/{}, depth slot {}) → 下标 {}",
				colorSlot, normalSlot, positionSlot, metalRoughSlot, depthSlot, index);
		return index;
	}

	/** addSsrApplyEntry 实现（批 15.3-B，契约见接口注释：模糊+合成，输入 = scene_hdr +
	 *  ssr_trace + metalrough，无 env——减法留在 trace 里做完，apply 只认预乘契约）。 */
	uint32_t ShaderRegistry::addSsrApplyEntry(uint32_t colorSlot, uint32_t traceSlot,
			uint32_t metalRoughSlot, uint32_t samplerSlot) {
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
		b.slotSceneData = sceneData.ensure();  // binding 3：本 pass 不读，但管线布局要求占位
		b.hasSceneData = true;
		// ⚠ sampler=true 条目烘 **字节偏移**（红线同 addSsrTraceEntry 上方注释）。
		const uint32_t samplerHeapOffset =
			samplerSlot * res.getSamplerHeap().getDescriptorSize();
		b.extra = {
			{.binding = 4, .sampler = false, .heapSlot = colorSlot},
			{.binding = 5, .sampler = false, .heapSlot = traceSlot},
			{.binding = 6, .sampler = false, .heapSlot = metalRoughSlot},
			{.binding = 7, .sampler = true, .samplerHeapOffset = samplerHeapOffset},
		};
		auto entry = PipelineEntry{.name = "ssr",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				"ssr"),
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: ssr (color slot {}, trace slot {}, metalrough slot {}) → 下标 {}",
				colorSlot, traceSlot, metalRoughSlot, index);
		return index;
	}

}  // namespace RGL
