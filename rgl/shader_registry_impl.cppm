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
import shadow_config;  // 批 11.3-1e / N6b：light 管线条目与软影 frag 变体按形态决策表选 spec
import asset_table;

namespace RGL {

// addModelEntry/addSkyEntry/addCubeEntry/loadShaderEffect 定义（2026-09-02 编译提速
// A 方案）：SPV 加载 + 管线创建函数体移出接口 TU；本单元是叶子边。

	uint32_t ShaderRegistry::addModelEntry(const std::string& specName, uint32_t vertexSlot,
			uint32_t textureSlot, uint32_t normalSlot, uint32_t ormSlot, uint32_t emissiveSlot,
			uint32_t samplerSlot, uint32_t shadowMapSlot, uint32_t shadowSamplerSlot,
			uint32_t evssmMapSlot) {
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
		// Phase 5 批次 2：模型管线恒传 scene data（binding 3，幂等创建；批 M 按 SPV 声明过滤：
		// 不声明 binding 3 的 frag/mesh 该条不烘，声明了照烘——mesh 读对象矩阵即声明方）
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
		// Phase 5.5：env cubemap 已上传 → 全模型条目传 binding 4/5（env 槽全局单例
		// 恒等，不进组合去重 key；不 import env.slang 的 frag 不声明 = 批 M 过滤不烘）
		if (const auto* envAsset = env.get()) {
			b.hasEnv = true;
			b.slotEnv = envAsset->heapSlot;
			b.envSamplerHeapOffset =
				env.ensureSkySampler(envAsset->mipLevels) * res.getSamplerHeap().getDescriptorSize();
		}
		// Phase 8 ③a：normal（binding 6）+ ORM（binding 7）贴图槽；Phase 8 ③：emissive
		//（binding 8）贴图槽（resource heap SAMPLED_IMAGE，extra 追加，仿 addLightEntry 的
		// extra 用法；不 import model.slang 或 Slang 剔除未用全局的 frag 不声明 = 批 M 过滤
		// 不烘——deferred_geometry 不声明 binding 8 亦不烘，声明了才要求有 mapping）
		b.extra.push_back({.binding = 6, .sampler = false, .heapSlot = normalSlot});
		b.extra.push_back({.binding = 7, .sampler = false, .heapSlot = ormSlot});
		b.extra.push_back({.binding = 8, .sampler = false, .heapSlot = emissiveSlot});
		// Phase 11 批 11.2：阴影图（binding 12）+ 专用 NEAREST sampler（binding 13）。**无条件传入**
		// ——不能看 shadow.lua 的 mode：光照管线条目一旦少了这个描述槽，开关时
		// 图不存在就会在采样侧报 VVL。unlit / phong 等不 import shadow.slang 的风格不声明
		// 它们 → 批 M 过滤不烘（同 binding 4/5 对不 import env 的风格；「声明了未传」才是
		// VVL 错误，import 了 = 声明了 = 这里必须传）。
		b.extra.push_back({.binding = 12, .sampler = false, .heapSlot = shadowMapSlot});
		b.extra.push_back({.binding = 13, .sampler = true,
			.samplerHeapOffset = static_cast<vk::DeviceSize>(shadowSamplerSlot)
				* res.getSamplerHeap().getDescriptorSize()});
		// 批 11.6d-2：EVSSM 矩图集 15 + 矩 sampler 16（**无条件传入**，同 binding 12 纪律；
		// 门关 = -DEVSSM_SOFT 变体不存在、binding 不声明 = 批 M 过滤不烘，binding 4/5 先例）。
		// binding 16 复用本条目共享 LINEAR sampler（矩图集过滤 = mip 高斯核要 linear；
		// uv 恒 clamp 块内 → wrap 模式无关）。
		b.extra.push_back({.binding = 15, .sampler = false, .heapSlot = evssmMapSlot});
		b.extra.push_back({.binding = 16, .sampler = true,
			.samplerHeapOffset = static_cast<vk::DeviceSize>(samplerSlot)
				* res.getSamplerHeap().getDescriptorSize()});
		// 条目名先落局部（批 M：RenderPipeline 的 warn/throw 消息要带条目名定位）
		const std::string entryName = fmt::format("{}.v{}.t{}.n{}.o{}.e{}", specName, vertexSlot,
			textureSlot, normalSlot, ormSlot, emissiveSlot);
		auto entry = PipelineEntry{
			.name = entryName,
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				entryName),
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
	// 不声明的 descriptor 由批 M 的 SPV 声明过滤（mesh+frag 并集）在烘焙期滤掉，不再白烘。
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
			const std::string cloneName = entries[i].name + "." + suffix;
			auto clone = PipelineEntry{
				.name = cloneName,
				.pipeline = RGL::RenderPipeline(device.getDevice(), effect, entries[i].bindings,
					cloneName),
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

	// 簇化轴 ★1-a1ii-α：簇路径叠加条目（契约见接口注释）。与 addModelEntry 的三处差别：
	// mesh SPV 换 meshlet.mesh（按 variant 选 base/DEBUG_OFFSET/TINT 变体）、去重键去顶点
	// slot 加变体位、追加 4 个固定 binding（17-20）。材质绑定段与 addModelEntry 逐条同值
	// ——**别合并成一份共享代码**：两处一旦漂移（比如模型条目加 binding 而叠加条目忘跟），
	// 图像静默错配材质；此处显式重复 + 注释互指，改一处必看见另一处。
	uint32_t ShaderRegistry::addMeshletEntry(const std::string& specName, uint32_t textureSlot,
			uint32_t normalSlot, uint32_t ormSlot, uint32_t emissiveSlot, uint32_t samplerSlot,
			uint32_t shadowMapSlot, uint32_t shadowSamplerSlot, uint32_t evssmMapSlot,
			std::array<uint32_t, 5> clusterSlots, MeshletMeshVariant variant) {
		const auto key = std::make_tuple(specName, textureSlot, normalSlot, ormSlot, emissiveSlot,
			variant);
		if (auto it = meshletEntryByKey.find(key); it != meshletEntryByKey.end())
			return it->second;

		const RGL::PipelineSpec* base = findSpec(specName.c_str());
		if (!base)
			throw std::runtime_error(fmt::format("pipeline spec not found: {}", specName));
		// mesh 阶段换 SPV（frag 原样 = 同规格模型条目同一个）；spec 缓存按 (规格, variant) 去重。
		// 变体名/后缀表 = 条目名与 spec 名的唯一拼写点（★0.5-b2 起三档，别在别处再拼一遍）。
		static constexpr std::array<std::string_view, 3> kVariantSpv{
			"generated_shaders/meshlet.mesh.spv",
			"generated_shaders/meshlet.mesh_MESHLET_DEBUG_OFFSET.spv",
			"generated_shaders/meshlet.mesh_MESHLET_TINT.spv"};
		static constexpr std::array<std::string_view, 3> kVariantSuffix{"", ".dbg", ".tint"};
		static constexpr std::array<std::string_view, 3> kVariantSpecSuffix{"_meshlet",
			"_meshlet_dbg", "_meshlet_tint"};
		const size_t vi = static_cast<size_t>(variant);
		static std::map<std::pair<std::string, MeshletMeshVariant>, RGL::PipelineSpec> meshletSpecs;
		const auto specIt = meshletSpecs.find({specName, variant});
		const RGL::PipelineSpec* spec = nullptr;
		if (specIt != meshletSpecs.end())
			spec = &specIt->second;
		else {
			RGL::PipelineSpec mvSpec = *base;
			mvSpec.meshSpv = std::string(kVariantSpv[vi]);
			mvSpec.name = base->name + std::string(kVariantSpecSuffix[vi]);
			spec = &meshletSpecs.emplace(std::make_pair(specName, variant), std::move(mvSpec))
			          .first->second;
		}
		const uint32_t descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		const uint32_t samplerHeapOffset =
			samplerSlot * res.getSamplerHeap().getDescriptorSize();
		const uint32_t sceneSlot = sceneData.ensure();
		RGL::PipelineBindings b{};
		b.descriptorSize = descriptorSize;
		// binding 0/1 顶点段：meshlet mesh 不声明 modelVertexBuffer（读簇顶点池 @17）⟹
		// 批 M 过滤把这两条滤掉（不声明 = 不烘，skybox/light 条目同）；slotVertex 传
		// 纹理槽号只为占个合法值，不被任何 stage 消费。
		b.slotVertex = textureSlot;
		b.slotIndex = textureSlot;
		b.slotTexture = textureSlot;
		b.samplerHeapOffset = samplerHeapOffset;
		b.slotSceneData = sceneSlot;
		b.textured = true;
		b.hasSceneData = true;
		if (const auto* envAsset = env.get()) {
			b.hasEnv = true;
			b.slotEnv = envAsset->heapSlot;
			b.envSamplerHeapOffset =
				env.ensureSkySampler(envAsset->mipLevels) * res.getSamplerHeap().getDescriptorSize();
		}
		// 材质绑定段 = addModelEntry 同名注释所述（6/7/8 贴图、12/13 阴影、15/16 EVSSM；
		// 无条件传入 + 批 M 按声明过滤烘焙纪律同源）。
		b.extra.push_back({.binding = 6, .sampler = false, .heapSlot = normalSlot});
		b.extra.push_back({.binding = 7, .sampler = false, .heapSlot = ormSlot});
		b.extra.push_back({.binding = 8, .sampler = false, .heapSlot = emissiveSlot});
		b.extra.push_back({.binding = 12, .sampler = false, .heapSlot = shadowMapSlot});
		b.extra.push_back({.binding = 13, .sampler = true,
			.samplerHeapOffset = static_cast<vk::DeviceSize>(shadowSamplerSlot)
				* res.getSamplerHeap().getDescriptorSize()});
		b.extra.push_back({.binding = 15, .sampler = false, .heapSlot = evssmMapSlot});
		b.extra.push_back({.binding = 16, .sampler = true,
			.samplerHeapOffset = static_cast<vk::DeviceSize>(samplerSlot)
				* res.getSamplerHeap().getDescriptorSize()});
		// 簇 buffer 固定 binding（§10.2）：17=顶点池 18=簇表 19=Tris 20=Remap
		// 21=逐簇可见标志（★1-a2-i，🔴 无条件传入：mesh SPV 无条件声明 binding 21，缺 mapping
		// 就是 VVL 错误；批 M 过滤按声明烘，声明了 ⟹ 照烘，门关时那张表内容 = 全 1，
		// 见 addMeshletEntry 接口注释）。
		// 描述符内容 = 装配层 wireMeshletBindings 写入这些槽（storage buffer 描述符，
		// ensureIzbSlots fallback 同法）；本条目只烘槽号。
		static constexpr std::array<uint32_t, 5> kClusterBindings{17u, 18u, 19u, 20u, 21u};
		for (size_t i = 0; i < kClusterBindings.size(); ++i)
			b.extra.push_back({.binding = kClusterBindings[i], .sampler = false,
				.heapSlot = clusterSlots[i]});
		const std::string entryName = fmt::format("meshlet.{}.t{}.n{}.o{}.e{}{}", specName,
			textureSlot, normalSlot, ormSlot, emissiveSlot, kVariantSuffix[vi]);
		auto entry = PipelineEntry{
			.name = entryName,
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				entryName),
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		meshletEntryByKey[key] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: meshlet.{} (base slot {} / cluster slots {}/{}/{}/{} / "
				"variant {}（0=base 1=debug 2=tint）) → 下标 {}",
				specName, textureSlot, clusterSlots[0], clusterSlots[1], clusterSlots[2],
				clusterSlots[3], vi, index);
		return index;
	}

	void ShaderRegistry::bindMeshletToAsset(uint32_t assetId, uint32_t entryIdx) {
		assets.bindMeshletPipeline(assetId, entryIdx);
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
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				"skybox"),
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
			uint32_t samplerSlot, uint32_t shadowMapSlot, uint32_t shadowSamplerSlot,
			uint32_t evssmMapSlot) {
		if (auto idx = pipelineIndex("light"))
			return *idx;
		// 批 11.3-1e 红线终态：门 = 选 spec 变体（IZB 消费版 frag SPV），不是运行时 flag 分支
		// ——slang 对源码差异的 fp 重排会让「门关 flag=0」也逐位漂移（2026-10-02 实测教训）。
		// 批 N6b：判据来自形态决策表的 `consumeIzbMask`（= configs/shadow.lua 的 `izb`）。
		const bool izbGate = shadowFormPlan(shadowConfig()).consumeIzbMask;
		const RGL::PipelineSpec* spec = findSpec(izbGate ? "light_izb" : "light");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: light");
		const uint32_t descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		RGL::PipelineBindings b{};
		b.descriptorSize = descriptorSize;
		b.slotVertex = 0;   // binding 0/1 占位槽号（light mesh 不声明 = 批 M 过滤不烘，skybox 同）
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
		// Phase 11 批 11.2：阴影图 12 + 专用 sampler 13（与 forward 模型条目**同号** →
		// shadow.slang 才能作为两条光照路共用的契约，否则同一份采样代码要复制两份）。
		b.extra.push_back({.binding = 12, .sampler = false, .heapSlot = shadowMapSlot});
		b.extra.push_back({.binding = 13, .sampler = true,
			.samplerHeapOffset = static_cast<vk::DeviceSize>(shadowSamplerSlot)
				* res.getSamplerHeap().getDescriptorSize()});
		// 批 11.6d-2：EVSSM 矩图集 15 + 矩 sampler 16（无条件传入 + 复用共享 LINEAR，
		// 细则见 addModelEntry 同名注释；不声明 = 批 M 过滤不烘）。
		b.extra.push_back({.binding = 15, .sampler = false, .heapSlot = evssmMapSlot});
		b.extra.push_back({.binding = 16, .sampler = true,
			.samplerHeapOffset = static_cast<vk::DeviceSize>(samplerSlot)
				* res.getSamplerHeap().getDescriptorSize()});
		// 批 11.3-1e：IZB mask 位图 14（仅门开变体声明该 binding = VVL 静态活性须绑；
		// 门关原版 frag 无 binding 14 = 不占槽不绑定）。门关也预写 16B 零 fallback 于
		// 发号处（Resources::ensureIzbSlots），防门开首帧 IzbShadow 构造前的时序坑。
		if (izbGate) {
			auto izb = res.ensureIzbSlots();
			if (!izb)
				throw std::runtime_error("light entry: izb slots: " + izb.error().describe());
			b.extra.push_back({.binding = 14, .sampler = false, .heapSlot = izb->mask});
		}
		auto entry = PipelineEntry{.name = "light",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				"light"),
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
		b.slotIndex = vertexSlot;   // binding 1 = 占位槽号（shadow mesh 不声明 = 批 M 过滤不烘，同 light/ao 条目做法）
		b.slotSceneData = sceneData.ensure();  // binding 3 = scene data（VP 区/对象矩阵/ObjectDraw）
		b.hasSceneData = true;
		// textured=false → 不烘焙 binding 1 的 baseColor 形态（上面 slotIndex 占位即此）；
		// 不烘焙 env（612）、不纹理纹理槽（6-8）——两个 shader 文件均未声明它们。
		const std::string entryName = fmt::format("shadow.v{}", vertexSlot);
		auto entry = PipelineEntry{.name = entryName,
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				entryName),
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

	// 簇化轴 ★18-B1：阴影段簇路径条目（契约见接口注释）。全场景 1 条目 = 簇 buffer 是全局
	// 单例（§10.2）⟹ addShadowEntry 的 vertexSlot 去重键在这里没有对应物；幂等靠条目名
	// （同 addSkyEntry / addLightEntry 的 `pipelineIndex("...")` 写法），装配层逐 prim 调它。
	uint32_t ShaderRegistry::addShadowMeshletEntry(std::array<uint32_t, 4> clusterSlots) {
		if (auto idx = pipelineIndex("shadow_meshlet"))
			return *idx;
		const RGL::PipelineSpec* spec = findSpec("shadow_meshlet");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: shadow_meshlet");
		RGL::PipelineBindings b{};
		b.descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		// binding 0/1 = 合法占位槽号（本管线 mesh/frag 两 stage 都不声明它们：mesh 不 import model、
		// frag 是 depth-only 空壳）⟹ 批 M 过滤不烘（批 M 前是「烘了空转无害」，现在根本不烘），
		// 方向与 addSkyEntry/addLightEntry 的 0/1 占位同（「声明未映射」才是 VVL 错误，
		// 过滤只敢滤没声明的）。取 0/1 两个真实存在的低段槽号。
		b.slotVertex = 0;
		b.slotIndex = 1;
		b.slotSceneData = sceneData.ensure();  // binding 3：簇区间/对象矩阵/ShadowViewProj 三张表
		b.hasSceneData = true;
		// textured = false → 不传 binding 1 的 baseColor 形态；不传 env、不传 6/7/8/12/13/15/16
		// ——两个 shader 文件均未声明它们（批 M 后即使传了也不会烘，但「不传」让槽号来源
		// 不参与账目，同 addShadowEntry 的取舍；🔴 若将来 shader 声明了其中之一，这里必须
		// 补传——「声明了未 mapping」仍是 VVL 错误）。
		// 簇 buffer 固定 binding（§10.2，与 addMeshletEntry 的 kClusterBindings 前 4 项同源）：
		// 17=顶点池 18=簇表 19=Tris 20=Remap。🔴 **不含 21**（逐簇可见标志）——本管线不声明
		// 该 binding，条目侧也不烘（两侧同向，理由 → 接口注释）。
		static constexpr std::array<uint32_t, 4> kClusterBindings{17u, 18u, 19u, 20u};
		for (size_t i = 0; i < kClusterBindings.size(); ++i)
			b.extra.push_back({.binding = kClusterBindings[i], .sampler = false,
				.heapSlot = clusterSlots[i]});
		auto entry = PipelineEntry{.name = "shadow_meshlet",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				"shadow_meshlet"),
			// ShadowMeshletDrawArgs = objectIndex + cascadeIndex + clusterStart（12B，scalar
			// layout；frame_draw_list.cppm 的 static_assert 与本 shader 的 struct 同源）
			.argsSize = 12,
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: shadow_meshlet (cluster slots {}/{}/{}/{}，binding "
				"17-20；无 binding 21) → 下标 {}",
				clusterSlots[0], clusterSlots[1], clusterSlots[2], clusterSlots[3], index);
		return index;
	}

	void ShaderRegistry::bindShadowMeshletToAsset(uint32_t assetId, uint32_t entryIdx) {
		assets.bindShadowMeshletPipeline(assetId, entryIdx);
	}

	// 批 11.3-1d 后半：IZB mask 条目（契约见接口注释；vertexSlot 去重同 shadow）。
	uint32_t ShaderRegistry::addIzbMaskEntry(uint32_t vertexSlot) {
		if (const auto it = izbMaskEntryBySlot.find(vertexSlot); it != izbMaskEntryBySlot.end())
			return it->second;
		const RGL::PipelineSpec* spec = findSpec("izb_mask");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: izb_mask");
		auto slots = res.ensureIzbSlots();
		if (!slots)
			throw std::runtime_error("izb_mask entry: " + slots.error().describe());
		RGL::PipelineBindings b{};
		b.descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		b.slotVertex = vertexSlot;   // binding 0（mesh 消费 modelVertexBuffer）
		b.slotIndex = vertexSlot;    // binding 1 = 占位（两 stage 不声明，同 shadow 做法）
		b.slotSceneData = sceneData.ensure();  // binding 3（readObjectDraw/Matrix/ShadowViewProj）
		b.hasSceneData = true;
		// 5/6/7 = IZB head/nodes/mask（frag 声明 = 必须传；mesh 不声明也无妨，过滤取并集）
		b.extra.push_back({.binding = 5, .heapSlot = slots->head});
		b.extra.push_back({.binding = 6, .heapSlot = slots->nodes});
		b.extra.push_back({.binding = 7, .heapSlot = slots->mask});
		const std::string entryName = fmt::format("izb_mask.v{}", vertexSlot);
		auto entry = PipelineEntry{.name = entryName,
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				entryName),
			.argsSize = 12,  // ShadowDrawArgs 8B + gridDim 4B（重放点拼接）
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		izbMaskEntryBySlot[vertexSlot] = index;
		entries.push_back(std::move(entry));
		return index;
	}

	std::optional<uint32_t> ShaderRegistry::izbMaskEntryFor(uint32_t vertexSlot) const {
		const auto it = izbMaskEntryBySlot.find(vertexSlot);
		return it != izbMaskEntryBySlot.end() ? std::optional{it->second} : std::nullopt;
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
		// 同 model 条目恒传入——cube mesh 声明了它 ⟹ 批 M 过滤照烘；cube 的 mvp 由
		// render_list 循环填进 mvp 表，objectIndex 有效）
		b.slotSceneData = sceneData.ensure();
		b.hasSceneData = true;
		auto entry = PipelineEntry{
			.name = "cube",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*cubeSpec), b,
				"cube"),
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

/**
 * 批 11.5/11.6：软阴影变体 spec 查表/建表（批 N6b：后缀直接来自形态决策表的 `spvSuffix`，
 * 改造前那条「shadowEvssm 优先、其次 shadowPcss」的让位链**从此结构性消失**——枚举下
 * "两门同开"这个组合根本表达不出来，也就没有谁给谁让位）。两档都不开时原样返回
 * = 原版 SPV 红线构造性保证，同 addLightEntry 的 1e 模式——运行时 flag 分支会被 slang
 * fp 重排坑穿，2026-10-02 实测教训）。
 * 阴影消费 frag 恰 4 文件（rgl_shader_defined 声明面同源）；"light.frag" 前缀同时覆盖
 * light.frag 与 light.frag_IZB_CONSUME.spv，后者插后缀 = ..._IZB_CONSUME_EVSSM_SOFT.spv
 * = rgl_shader_defined2 命名。
 */
const RGL::PipelineSpec* ShaderRegistry::softShadowVariantSpec(const RGL::PipelineSpec* base) const {
	if (!base)
		return base;
	const auto plan = shadowFormPlan(shadowConfig());
	if (plan.spvSuffix.empty())
		return base;
	const std::string_view path{base->fragSpv};
	const auto slash = path.find_last_of("/\\");
	const std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
	static constexpr std::array<std::string_view, 4> kShadowConsumers{
		"model_pbr_ibl.frag", "model_vegetation.frag", "model_light.frag", "light.frag"};
	bool consumer = false;
	for (const auto prefix : kShadowConsumers)
		consumer = consumer || file.starts_with(prefix);
	if (!consumer)
		return base;
	static std::map<std::string, RGL::PipelineSpec> variantSpecs;  // key = 原版 fragSpv（启动期单线程）
	if (auto it = variantSpecs.find(base->fragSpv); it != variantSpecs.end())
		return &it->second;
	RGL::PipelineSpec variant = *base;
	const auto dot = variant.fragSpv.rfind(".spv");
	variant.fragSpv.insert(dot == std::string::npos ? variant.fragSpv.size() : dot, plan.spvSuffix);
	// 条目名后缀 = spv 后缀的小写短名（表里没有的后缀不会走到这里，故无需兜底分支）
	variant.name = base->name
		+ (plan.spvSuffix == "_EVSSM_SOFT" ? "_evssm" : "_pcss");
	return &variantSpecs.emplace(base->fragSpv, std::move(variant)).first->second;
}

}  // namespace RGL
