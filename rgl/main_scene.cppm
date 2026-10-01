module;
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // makeRootTransform（translate/rotate/scale 组装）
#include <fmt/format.h>
#include <filesystem>  // environment.hdr 本地素材存在性探测（GMF 例外清单成员）

// 入口模块 main 的分区实现单元（module main:scene;）——启动流重函数定义（接口瘦身：
// main.cppm 接口只有声明 + 轻量 struct，函数体在本叶子单元）。main() 在姊妹分区
// main:impl（main_impl.cppm），经主接口声明可见本单元定义（同模块，无需互 import）。
module main:scene;

import main;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import vk_device;
import vk_resources;
import scene_data;  // kSceneMatrixCapacity + SceneDataService（架构债务续账步骤 1）
import sampler_library;  // SamplerLibrary（架构债务续账步骤 4a/4b）
import environment;  // Environment（架构债务续账步骤 3）
import render_target_cache;  // RenderTargetCache（架构债务续账步骤 2）
import gltf_loader;
import model_importer;
import texture_pool;
import scene_config;
import scene_context;
import camera_config;  // loadCameraConfig（hot_reload camera.lua 套用，Phase 10 批 10.3）
import camera;  // Camera ctx 类型（hot_reload 调参字段套用，10.3）
import hot_reload;  // HotReloader（配置热重载机制模块，10.3）
import shader_hot_reload;  // ShaderHotReloader（shader 热重载 L1，10.4）
import picking;  // PickingService（wirePicking 桥接，10.5）
import ecs;  // ConfigReloadHook ctx（帧边界挂点类型，10.3）
import shader_registry;
import gpu_cull;
import vk_indirect_commands;
import vk_renderer;
import vk_swapchain;
import render_graph;
import builtin_geometry;
import asset_table;
import diagnostics;
import rllogger;

namespace RGL {

namespace {

/** deferred light 条目的阴影描述槽（Phase 11 批 11.2）。为何抽出成函数而不内联：它为
 *  assembleBackEnd 多了一个返回值分支（认知复杂度 +1，已触 15 阈值）。
 * 与 bakePipelineEntries 里的同名 ensure 幂等（按名缓存，同尺寸/格式 → 同槽，不重建图）。
 * @param rtCache RT 缓存
 * @return 阴影图 SAMPLED_IMAGE 堆槽（light 条目 binding 12）
 * @throws std::runtime_error RT 创建 / 槽分配失败
 */
uint32_t deferredShadowSlotOf(RenderTargetCache& rtCache) {
	auto slot = ensureShadowMapSlot(rtCache, kShadowMapSize);
	if (!slot)
		throw std::runtime_error(fmt::format("deferred: 阴影图 RT 创建失败: {}",
			slot.error().describe()));
	return *slot;
}

}  // namespace

/** A0 内建 cube + A1 解析/顶点上传（全几何槽先于任何纹理槽）+ A2 纹理上传。 */
UploadedAssets uploadModelAssets(DeviceContext& device, Resources& resources,
	SamplerLibrary& samplers, AssetTable& assets, const SceneConfig& scene) {
	UploadedAssets out;
	// A0：内建几何（架构债务 1 方案 B）——entities 里有 cube 就上传内建 cube 资产，
	// 与普通模型的顶点槽同属"几何段"（先于任何纹理槽）；先于模型循环上传 = 与旧
	// "Resources 构造期占 slot 0/1"一致的槽序（截图回归基线可比）
	for (const auto& ent : scene.entities) {
		if (ent.type != "cube" || out.cubeAsset)
			continue;
		auto cubeResult = uploadBuiltinCube(device, resources, assets);
		if (!cubeResult)
			throw std::runtime_error(cubeResult.error().describe());
		out.cubeAsset = *cubeResult;
	}
	// A1：解析 + 顶点上传（模型的顶点块走 geometry 段，N = 全模型 chunk + 内建几何槽；
	// 几何段内部连续性由 AssetTable::expectedGeometrySlot 校验）。
	// Phase 6 instancing：entities 按 **file 去重**——同文件多 entity = 一次解析
	// 一次上传（顶点/纹理/管线条目全部共享），每 entity 各注册一组 primitive
	// entity（独立 Transform + 独立实例矩阵槽 objectSlot）。资产与实例分离（ECS 规范 5）
	//（per-file 上传 lambda 独立计分）
	auto uploadModelFile = [&](const SceneEntityDesc& ent) {
		if (ent.type != "model")
			return;
		if (auto [it, inserted] = out.modelOf.emplace(ent.file, out.models.size()); !inserted) {
			out.models[it->second].instances++;
			return;  // 同文件已上传（instancing 共享资产）
		}
		auto modelResult = loadGltfModel(ent.file);
		if (!modelResult)
			throw std::runtime_error(modelResult.error().describe());
		auto mesh = std::make_unique<GltfModel>(std::move(*modelResult));
		auto assetIdsResult = uploadModelMeshes(device, resources, assets, *mesh);
		if (!assetIdsResult)
			throw std::runtime_error(assetIdsResult.error().describe());
		out.models.push_back(UploadedModel{.file = ent.file, .model = std::move(mesh),
			.assetIds = std::move(*assetIdsResult), .instances = 1});
	};
	for (const auto& ent : scene.entities)
		uploadModelFile(ent);
	// A2：纹理上传（Phase 3）：先取白纹理——布局契约"纹理首槽 = 白 = 纹理段首槽"，
	// ensureWhiteTexture 带契约断言；产物在资产表
	for (auto& um : out.models) {
		auto textureIdsResult = uploadModelTextures(resources, samplers, assets, device, *um.model);
		if (!textureIdsResult)
			throw std::runtime_error(textureIdsResult.error().describe());
		um.textureIds = std::move(*textureIdsResult);
	}
	return out;
}

/** env cubemap 上传 + SceneEnv 回填（hdr 优先路径缺失回落 faces PNG）。 */
void uploadEnvironment(Resources& resources, Environment& env, DeviceContext& device,
					   const SceneConfig& scene, entt::registry& registry) {
	auto& envCfg = scene.environment;
	if (!envCfg.hdr.empty()) {
		std::error_code ec;
		if (std::filesystem::exists(envCfg.hdr, ec) && !ec) {
			auto hdrResult = loadEnvCubeMapHdr(resources, env, device, envCfg.hdr,
				envCfg.hdrSize, "scene.environment");
			if (!hdrResult)
				throw std::runtime_error(
					fmt::format("environment: {}", hdrResult.error().describe()));
			const auto& envAsset = *env.get();
			auto& envCtx = registry.ctx().get<SceneEnv>();
			envCtx.intensity = envCfg.intensity;
			envCtx.mipRange = static_cast<float>(envAsset.mipLevels - 1);
			return;
		}
		if (auto* logger = RLLogger::getInstance())
			logger->warn("environment.hdr '{}' 不存在（本地素材），回落 faces PNG 路径",
				envCfg.hdr);
	}
	std::array<std::string, 6> paths;
	for (uint32_t f = 0; f < 6; ++f)
		paths[f] = fmt::format("{}/{}", envCfg.dir, envCfg.faces[f]);
	auto envResult = loadEnvCubeMapFiles(resources, env, device, paths, "scene.environment");
	if (!envResult)
		throw std::runtime_error(fmt::format("environment: {}", envResult.error().describe()));
	const auto& envAsset = *env.get();
	auto& envCtx = registry.ctx().get<SceneEnv>();
	envCtx.intensity = envCfg.intensity;
	envCtx.mipRange = static_cast<float>(envAsset.mipLevels - 1);
}

/** C1 管线条目烘焙（per unique 模型；风格派生 + 条目去重 + DIAG 分布日志）。 */
void bakePipelineEntries(UploadedAssets& uploaded, const SceneConfig& scene, bool deferredMode,
						 ShaderRegistry& pipelines, Resources&, SamplerLibrary& samplers,
						 AssetTable& assetTable, RenderTargetCache& rtCache) {
	const uint32_t samplerSlot = samplers.ensureShared();
	// Phase 11 批 11.2：阴影图槽必须在**模型条目烘焙之前**领掉（堆槽号由 ensure 时分配，
	// 烘焙后再拿到就太晚）。与 env 槽同轴（AGENTS：环境上传 = A2 后、addModelEntry 前）。
	// 无条件建（不看 diagnostics.shadow_pass）——光照管线无条件烘这个槽。
	// assembleHdrAoChain 里的同名 ensure 是幂等的（deferred light 条目用）。
	auto shadowSlot = ensureShadowMapSlot(rtCache, kShadowMapSize);
	if (!shadowSlot)
		throw std::runtime_error(
			fmt::format("阴影图 RT 创建失败: {}", shadowSlot.error().describe()));
	for (auto& um : uploaded.models) {
		auto& model = *um.model;
		const auto& texIds = um.textureIds;
		// 管线条目（Phase 4 批次 2：按 (规格,顶点 slot,base 纹理 slot,normal slot,
		// orm slot,emissive slot) 六元组组合去重——同模型全 primitive 共享一个拼接顶点
		// slot，仅纹理槽差异决定条目拆分；纹理/sampler slot 烘焙进 binding mapping，每帧
		// 零成本；Phase 8 ③a 扩 normal/orm 槽、③ 扩 emissive 槽）。批次 3：风格派生（loader unlit
		// 字段即数据源，与 MaterialAsset 一致性由 registerScene 金丝雀保证）→ scene.lua
		// styles 表映射到管线规格名（缺失 = 启动报错，同现有 config 错误路径）。
		// Phase 8 批次 F：材质风格 = scene.lua material_styles 连连看（材质名含 pattern →
		// 风格键，有序首个命中；默认规则 {Emissive→light, Foliage→vegetation}）。forward 经
		// styleMap、deferred 经 deferredStyleMap 落各自规格名（植被/光源两路专属视觉对齐；
		// deferred 变体把类别写进 G-buffer emissive.a，light.frag 分支复刻 forward 视觉）。
		// （lambda 独立计分：宿主认知复杂度不叠加 lambda 体——stylecheck 规则）
		// 风格 → 管线规格名（lambda 独立计分）
		auto resolveSpecName = [&](const GltfPrimitive& prim) -> std::string {
			std::string style = prim.unlit ? "unlit" : "pbr";
			if (!prim.unlit) {
				for (const auto& [pattern, styleKey] : scene.materialRules)
					if (prim.materialName.find(pattern) != std::string::npos) {
						style = styleKey;
						break;
					}
			}
			if (deferredMode) {
				const auto defIt = scene.deferredStyleMap.find(style);
				return defIt != scene.deferredStyleMap.end() ? defIt->second
															  : std::string("deferred_geometry");
			}
			const auto styleIt = scene.styleMap.find(style);
			if (styleIt == scene.styleMap.end())
				throw std::runtime_error(fmt::format("style not in scene.lua styles: {}", style));
			return styleIt->second;
		};
		auto addPrimitiveEntry = [&](const GltfPrimitive& prim, size_t i) {
			const uint32_t id = um.assetIds[i];
			const uint32_t entryIdx = pipelines.addModelEntry(
				resolveSpecName(prim),
				assetTable.get(id).heapSlot,  // = 模型拼接顶点 slot（同模型全 primitive 相同）
				assetTable.getTexture(texIds.base[i]).heapSlot,
				assetTable.getTexture(texIds.normal[i]).heapSlot,
				assetTable.getTexture(texIds.orm[i]).heapSlot,
				assetTable.getTexture(texIds.emissive[i]).heapSlot,
				samplerSlot, *shadowSlot, samplers.ensurePointSampler());
			pipelines.bindToAsset(id, entryIdx);
			// Phase 11 批 11.2：阴影 depth-only 管线条目。按 vertexSlot 去重（addShadowEntry
			// 内部有表）——bistro 1591 个 primitive 共用约 8 个拼接块槽 → 仅 8 条目。
			// 无条件分支：门控关时条目与 shadowCalls 都为空，GPU 产生一个未被引用的管线
			// 条目（零成本；AGENTS「条目全按需建」纪律）。
			pipelines.bindShadowToAsset(id, pipelines.addShadowEntry(assetTable.get(id).heapSlot));
			// 批 11.3-1d 后半：IZB mask 条目（门关零创建；重放点经 vertexSlot 反查，
			// 不挂资产——同 vertexSlot 去重）
			if (diagnostics().izbShadow)
				static_cast<void>(pipelines.addIzbMaskEntry(assetTable.get(id).heapSlot));
		};
		// DIAG（configs/diagnostics.lua style_dist；Phase 4 批次 3 风格分布核查）：
		// per-primitive 风格（unlit/pbr）+ 规格名分布。（lambda 独立计分）
		auto logStyleDiag = [&] {
			if (!diagnostics().styleDist)
				return;
			std::map<std::string, uint32_t> styleCount;
			for (size_t i = 0; i < um.assetIds.size(); ++i)
				++styleCount[model.primitives[i].unlit ? "unlit" : "pbr"];
			if (auto logger = RLLogger::getInstance()) {
				std::string t;
				for (const auto& [style, cnt] : styleCount)
					t += fmt::format("{}={} ", style, cnt);
				logger->info("DIAG style dist ({} x{}): {} primitives → {} styles: {}",
					um.file, um.instances, um.assetIds.size(), styleCount.size(), t);
			}
		};
		// DIAG（configs/diagnostics.lua tex_dist；Phase 4 批次 2 纹理分布核查）：
		// per-primitive 纹理 slot 分布（验证新旧管线用同一批纹理）。（lambda 独立计分）
		auto logTexDiag = [&] {
			if (!diagnostics().texDist)
				return;
			std::map<uint32_t, uint32_t> texSlotCount;
			for (size_t i = 0; i < um.assetIds.size(); ++i)
				++texSlotCount[assetTable.getTexture(texIds.base[i]).heapSlot];
			if (auto logger = RLLogger::getInstance()) {
				std::string t;
				for (const auto& [slot, cnt] : texSlotCount)
					t += fmt::format("slot{}={} ", slot, cnt);
				logger->info("DIAG tex dist ({} x{}): {} primitives → {} distinct tex slots: {}",
					um.file, um.instances, um.assetIds.size(), texSlotCount.size(), t);
			}
		};
		for (size_t i = 0; i < um.assetIds.size(); ++i)
			addPrimitiveEntry(model.primitives[i], i);
		logStyleDiag();
		logTexDiag();
	}
}

/** C2/C2b：模型/cube/灯 entity 注册 + 实例矩阵槽容量契约。 */
void registerSceneEntities(entt::registry& registry, const UploadedAssets& assets,
						   const SceneConfig& scene, Resources& resources, AssetTable& table,
						   ShaderRegistry& pipelines) {
	/**
	 * 实体 rootTransform 组装（scene.lua entities 的 transform 字段 → 4x4 世界矩阵）：
	 * 先 translate(position)，再按 X→Y→Z 顺序依次 glm::rotate 后乘（rotation 为度，
	 * 手动转弧度——GLM_FORCE_RADIANS 下 radians() 恒等），最后 scale 后乘——
	 * 结果 = T·Rx·Ry·Rz·S（旋转应用序 X→Y→Z，同 SceneEntityDesc 契约）。
	 * （lambda 独立计分；模型 entity 与 cube entity 共用）
	 */
	auto makeRootTransform = [](const SceneEntityDesc& ent) {
		glm::mat4 t = glm::translate(glm::mat4(1.0f), ent.position);
		t = glm::rotate(t, glm::radians(ent.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
		t = glm::rotate(t, glm::radians(ent.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
		t = glm::rotate(t, glm::radians(ent.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
		return glm::scale(t, glm::vec3(ent.scale));
	};
	// C2：场景实体注册（per **entity**——同文件各实例独立 rootTransform/矩阵槽；
	// 材质经 addMaterial find-or-add 去重，重复注册返回既有 id）
	for (const auto& ent : scene.entities) {
		if (ent.type != "model")
			continue;
		auto& um = assets.models[assets.modelOf.at(ent.file)];
		registerScene(registry, *um.model, std::span<const uint32_t>(um.assetIds),
			std::span<const uint32_t>(um.textureIds.base), resources, table,
			makeRootTransform(ent));
	}
	// scene data 容量契约（Phase 5 批次 2，Phase 6 instancing 改**实例矩阵槽**计数
	//——资产共享后 meshSize < 槽数才是常态）：对象矩阵表按实例槽索引，超容量 =
	// 矩阵表越界写（staging 上传处有防御报错，此处启动期先拦截）
	if (const uint32_t slotCount = registry.ctx().get<NextObjectSlot>().next;
		slotCount > kSceneMatrixCapacity)
		throw std::runtime_error(fmt::format(
			"scene instances ({}) exceed object matrix capacity ({})",
			slotCount, kSceneMatrixCapacity));
	if (auto logger = RLLogger::getInstance())
		logger->info("instancing: {} unique models / {} primitive instances (matrix slots)",
			assets.models.size(), registry.ctx().get<NextObjectSlot>().next);

	// 内建 cube entity 注册（架构债务 1 方案 B：cube = 内建几何资产，与模型同路）：
	// 注册 entity（画不画 = 实体有无）+ 管线条目（按资产 slot 烘焙，cube 无纹理/无 scene
	// data）+ 下标写回（启动期一次，幂等）。cube 着色 = 每顶点色，MaterialRef 指向默认
	// 材质（baseColor 不被 cube 管线消费，见 registerBuiltinCube 注释）
	for (const auto& ent : scene.entities) {
		if (ent.type != "cube")
			continue;
		const uint32_t cubeMat = table.addMaterial(
			MaterialAsset{glm::vec4{1.0f}, kInvalidTextureId, false});
		registerBuiltinCube(registry, *assets.cubeAsset, cubeMat, makeRootTransform(ent));
		const uint32_t cubeEntry = pipelines.addCubeEntry(*assets.cubeAsset);
		pipelines.bindPipelineToAsset("cube", *assets.cubeAsset);
		if (auto logger = RLLogger::getInstance())
			logger->info("cube entity: 资产 {} / 材质 {} / 管线下标 {}",
				*assets.cubeAsset, cubeMat, table.get(*assets.cubeAsset).pipelineIndex);
		(void)cubeEntry;
	}

	// C2b：灯 entity 注册（Phase 7 灯 entity 化，ECS 规范 3：画不画 = 实体有无）。
	// 数据源 = scene.lights（scene_config 保证非空：lights 段缺 → 回落 [light]）。
	// 每灯一个 entity + Light 组件；index = lights 列表位（正序契约：index 0 = 主光
	// forward @0 头，全数组按 index 落槽 deferred）。灯无资产/上传/矩阵槽，逻辑内联最简。
	for (uint32_t i = 0; i < scene.lights.size(); ++i) {
		auto e = registry.create();
		auto& L = registry.emplace<Light>(e);
		L = scene.lights[i];
		L.index = i;
	}
}

/**
 * HDR 累加链 + SSAO 装配（Phase 8 批次 ① / Phase 11 批 11.4；assembleBackEnd 私有调用）。
 *
 * 装四样东西，顺序即依赖：
 *   1. scene_hdr RT（rgba16_sfloat）+ 其 SAMPLED_IMAGE 堆槽；
 *   2. ao RT（r8_unorm）+ 其堆槽；
 *   3. 深度图 SAMPLED_IMAGE 堆槽（ensureDepthSampleSlot；深度图 usage 自本批起含 eSampled）；
 *   4. ao 条目（binding 4 = 深度槽 / 5 = **AO 专用 point sampler**）+ tonemap 条目
 *      （4 = scene_hdr / 5 = 共享 sampler / **6 = ao 槽**）。
 *      ao 不能用共享 sampler：共享那份是 LINEAR + mipmap LINEAR，而 D32Sfloat 深度图
 *      不可过滤（invalid usage），且深度插值无物理意义——见 SamplerLibrary::ensurePointSampler。
 *
 * 为何无条件建（不分 forward/deferred）：forward/deferred 两图都含 ao + tonemap pass；
 * offscreen DIAG 变体图两者都不含，条目闲置无害（管线表条目不绑定即零成本）。
 *
 * 槽号契约：RT 槽与深度槽都在两个 add*Entry **之前**领——槽号一经 bake 进管线 binding
 * 即终身有效；后续 resize 换图只重写描述符内容（RenderTargetCache::ensure / 
 * Resources::recreateDepthImage），绑定不失效。
 *
 * @param swapchain 交换链层（RT extent）
 * @param resources 资源层（深度采样槽）
 * @param samplers 共享 sampler（ao/tonemap 的 binding 5）
 * @param rtCache RT 缓存（ensure/heapSlot）
 * @param pipelines 管线表（建 ao/tonemap 条目）
 */
void assembleHdrAoChain(Swapchain& swapchain, Resources& resources,
	SamplerLibrary& samplers, RenderTargetCache& rtCache, ShaderRegistry& pipelines) {
	auto hdrSlot = ensureSceneHdrSlot(rtCache, swapchain);
	if (!hdrSlot)
		throw std::runtime_error(
			fmt::format("scene_hdr RT 创建失败: {}", hdrSlot.error().describe()));
	auto aoSlot = ensureAoSlot(rtCache, swapchain);
	if (!aoSlot)
		throw std::runtime_error(
			fmt::format("ao RT 创建失败: {}", aoSlot.error().describe()));
	auto depthSlot = resources.ensureDepthSampleSlot();
	if (!depthSlot)
		throw std::runtime_error(
			fmt::format("深度采样槽分配失败: {}", depthSlot.error().describe()));
	// Phase 11 批 11.2：阴影图槽。**无条件建**（不看 diagnostics.shadow_pass）——光照管线条目
	// 无条件烘焙它（否则开关时描述绑定的图从未被任何 pass 转布局 = 采样就报 VVL）。
	// 门控在渲染侧：关时阴影段零 draw，图内全 1.0 = 无遮挡 → 成片逐位不变。
	auto shadowSlot = ensureShadowMapSlot(rtCache, kShadowMapSize);
	if (!shadowSlot)
		throw std::runtime_error(
			fmt::format("阴影图 RT 创建失败: {}", shadowSlot.error().describe()));
	// ao / tonemap 条目下标由 Renderer 构造期按名 pipelineIndex("ao"/"tonemap") 取
	[[maybe_unused]] auto aoIdx = pipelines.addAoEntry(*depthSlot, samplers.ensurePointSampler());
	[[maybe_unused]] auto tonemapIdx = pipelines.addTonemapEntry(
		*hdrSlot, *aoSlot, samplers.ensureShared());
}

/** C3 候选表 + GpuCull + skybox + deferred + HDR + DGC + Renderer（装配时序见接口注释）。 */
RenderBackEnd assembleBackEnd(DeviceContext& device, Swapchain& swapchain, Resources& resources,
	SceneDataService& sceneData, SamplerLibrary& samplers, Environment& env,
	RenderTargetCache& rtCache, AssetTable& assets,
							  ShaderRegistry& pipelines, CompiledRenderGraph&& graph,
							  entt::registry& registry, const SceneConfig& scene, bool deferredMode) {
	// C3：GPU culling 候选表（Phase 7b 批次 5）：candidate = 不透明 primitive。
	// 守卫与迭代序和 render_list CPU 路径**完全一致**（同 view、同守卫）——GPU 输出
	// 固定槽非紧凑，重放序与 CPU opaque 段序逐位一致（截图 BIT-IDENTICAL 前提）。
	// 透明段不入候选（CPU 排序路径不受 GPU culling 影响）。
	std::vector<CullEntry> cullEntries;
	registry.view<MeshRef, MaterialRef, CullSphere>().each(
		[&](const MeshRef& meshRef, const MaterialRef& matRef, const CullSphere& cull) {
			const auto& asset = assets.get(meshRef.asset);
			if (asset.pipelineIndex == kInvalidPipelineIndex)
				return;
			if (asset.groupCount == 0)
				return;
			const auto& mat = assets.getMaterial(matRef.material);
			if (mat.alphaModeBlend)
				return;
			cullEntries.push_back(CullEntry{
				cull.center, cull.radius, meshRef.objectSlot, asset.pipelineIndex,
				asset.groupCount, asset.triangleCount, asset.vertexOffset,
				mat.baseColor, glm::vec2(mat.roughness, mat.metallic)});
		});
	// 静态 shader DGC 路选路（spike，docs/archive/radv_static_dgc_spike.md）：IES 路缺席
	// （非 graphicsIndirect）但全集含 MESH 且 diagnostics.dgcStatic 开 → per-entry 流
	// 随 GpuCull 分配（fanout kernel 输出）。N = 此刻管线表条目数——sky/light 条目在
	// 其后加入、非 candidate，DgcStaticCommands 必须用同一数保证流布局一致。
	const auto& dgcSup = device.getDgcSupport();
	const uint32_t dgcStaticEntryCount = static_cast<uint32_t>(pipelines.entriesView().size());
	const bool dgcStaticEnabled = !dgcSup.graphicsIndirect && dgcSup.staticGraphicsIndirect
		&& diagnostics().dgcStatic && !cullEntries.empty();
	// GpuCull 堆分配（Renderer 持其指针，聚合返回时局部析构会悬空——见 RenderBackEnd
	// 注释）；先构造（skybox 条目其后加入，见上 N 快照契约）
	auto cull = std::make_unique<GpuCull>(device, resources, sceneData, cullEntries,
		dgcStaticEnabled ? dgcStaticEntryCount : 0u);

	// Phase 5.5 批次 4：天空盒管线条目（env 已上传才建——addSkyEntry 契约）+
	// SceneEnv 回填（renderListSystem 经 ctx 生成 skybox draw，画不画 = 配置有无）
	if (env.get()) {
		auto& envCtx = registry.ctx().get<SceneEnv>();
		envCtx.skyboxPipeline = pipelines.addSkyEntry();
		envCtx.skybox = scene.environment.skybox;
	}

	// Phase 7 D1：deferred 资源。装配序——env/models 已分配，此处才分配
	// G-buffer RT 堆槽（运行时段）+ 建 light 管线，再构造
	// Renderer（其构造期校验 drawLight pass 有 light 条目）。forward 全跳过。
	if (deferredMode) {
		auto gbSlots = ensureDeferredGBufferSlots(rtCache, swapchain);
		if (!gbSlots)
			throw std::runtime_error(
				fmt::format("deferred: G-buffer RT 创建失败: {}", gbSlots.error().describe()));
		// light 条目下标由 Renderer 构造期按名 pipelineIndex("light") 取，此处仅需触发建条目
		[[maybe_unused]] auto lightIdx = pipelines.addLightEntry(
			(*gbSlots)[0], (*gbSlots)[1], (*gbSlots)[2], (*gbSlots)[3], (*gbSlots)[4],
			samplers.ensureShared(), deferredShadowSlotOf(rtCache), samplers.ensurePointSampler());
	}
	assembleHdrAoChain(swapchain, resources, samplers, rtCache, pipelines);

	// Phase 7b：DGC 命令生成（NVIDIA 路）：仅 graphicsIndirect 能力时构造（RADV =
	// compute only → null → Renderer 读回重放路）。管线表定后建（execution set
	// 容量 = 2×entries）；构造异常（容量超设备上限等）回落重放路
	//（lambda 独立计分）
	auto makeDgc = [&]() -> std::unique_ptr<DgcCommands> {
		if (!device.getDgcSupport().graphicsIndirect || !cull->active())
			return nullptr;
		try {
			return std::make_unique<DgcCommands>(device, pipelines,
				cull->candidateCount(), 2);  // framesInFlight=2（preprocess 槽 ×2，11142）
		} catch (const std::exception& e) {
			if (auto* logger = RLLogger::getInstance())
				logger->warn("dgc: 构造失败回落读回重放: {}", e.what());
		}
		return nullptr;
	};
	// 静态 shader DGC 路（RADV spike）：IES 未建且门控开时构造；异常回落读回重放
	// （流 buffer 已随 GpuCull 分配但无人消费——仅多占显存，像素路径无影响）
	auto makeDgcStatic = [&]() -> std::unique_ptr<DgcStaticCommands> {
		if (!dgcStaticEnabled || !cull->active())
			return nullptr;
		try {
			return std::make_unique<DgcStaticCommands>(device, pipelines,
				cull->candidateCount(), dgcStaticEntryCount, 2);
		} catch (const std::exception& e) {
			if (auto* logger = RLLogger::getInstance())
				logger->warn("dgc static: 构造失败回落读回重放: {}", e.what());
		}
		return nullptr;
	};
	std::unique_ptr<DgcCommands> dgc = makeDgc();
	std::unique_ptr<DgcStaticCommands> dgcStatic = (dgc ? nullptr : makeDgcStatic());

	// 清晰选路日志（启动期一次）：哪个设备走 DGC execute / 静态 execute / 读回重放
	if (auto* logger = RLLogger::getInstance()) {
		std::string devName(device.getPhysicalDevice().getProperties().deviceName.data());
		if (dgc && dgc->active())
			logger->info("DGC path: device='{}' graphicsIndirect={} computeIndirect={} static={} -> DGC execute (vkCmdExecuteGeneratedCommandsEXT)",
				devName, dgcSup.graphicsIndirect, dgcSup.computeIndirect, dgcSup.staticGraphicsIndirect);
		else if (dgcStatic && dgcStatic->active())
			logger->info("DGC path: device='{}' graphicsIndirect={} computeIndirect={} static={} -> DGC static execute ({} entries × vkCmdExecuteGeneratedCommandsEXT)",
				devName, dgcSup.graphicsIndirect, dgcSup.computeIndirect, dgcSup.staticGraphicsIndirect,
				dgcStatic->entryCount());
		else
			logger->info("DGC path: device='{}' graphicsIndirect={} computeIndirect={} static={} -> 读回重放 (CPU 回退)",
				devName, dgcSup.graphicsIndirect, dgcSup.computeIndirect, dgcSup.staticGraphicsIndirect);
	}
	// Renderer 以 prvalue **直接初始化**进聚合并 move-init 的 RenderBackEnd 槽位——
	// 绝不 move Renderer：其成员 FrameCapturer::graph 是指向本对象 renderGraph 成员的
	// 引用，隐式 move 的成员拷贝保留旧绑定（= 本函数栈上旧对象），assembleBackEnd
	// 返回即悬空，dump_gbuf 路径 record()→slotOfName 解引用悬垂 vector → SIGSEGV
	// （2026-09-15 勘误）。raw 指针在 unique_ptr 移走前捕获（聚合元素按声明序求值：
	// cull/dgc/dgcStatic 先 move、Renderer 后构造，对象本体不换地址）。
	// struct 内析构逆序 renderer→dgcStatic→dgc→cull 不变，与拆分前 main 局部变量序一致。
	auto* cullRaw = cull.get();
	auto* dgcRaw = dgc.get();
	auto* dgcStaticRaw = dgcStatic.get();
	return RenderBackEnd{std::move(cull), std::move(dgc), std::move(dgcStatic),
		Renderer(device, swapchain, resources, sceneData, env, rtCache, pipelines,
			std::move(graph), cullRaw, dgcRaw, dgcStaticRaw)};
}

/**
 * wireHotReload 定义（契约见 main.cppm 接口注释）。三条监视链 = 帧边界
 * HotReloader::poll 触发；回调只碰 ctx 值字段（不触碰 GPU 资源/资产表/registry 结构），
 * 解析失败 warn + 保留旧值。reloader 所有权在 hook 闭包内（ctx 活多久它活多久）。
 */
void wireHotReload(entt::registry& registry) {
	if (!diagnostics().configReload)
		return;  // 总开关默认关 = 零行为变化（红线）
	// 监视源：RGL_CONFIG_DIR 覆盖时优先（测试用一次性配置目录），否则回退编译期源树
	// configs/（D7——build 副本会被 just build 覆盖复原，监听它无意义）。
	// env 已设 = 该目录由调用方负责存在性与生命周期。
	std::filesystem::path sourceDir;
	if (const char* dir = std::getenv("RGL_CONFIG_DIR"); dir && *dir) {
		sourceDir = dir;
	} else {
#ifdef RGL_SOURCE_CONFIG_DIR
		sourceDir = RGL_SOURCE_CONFIG_DIR;
#else
		if (auto* logger = RLLogger::getInstance())
			logger->warn("hot_reload: 编译期未注入 RGL_SOURCE_CONFIG_DIR，热重载关闭");
		return;
#endif
	}
	std::error_code ec;
	if (!std::filesystem::is_directory(sourceDir, ec)) {
		if (auto* logger = RLLogger::getInstance())
			logger->warn("hot_reload: 源 configs 目录不存在（热重载关闭）: {}", sourceDir.string());
		return;
	}

	auto reloader = std::make_shared<HotReloader>();
	reloader->watch(sourceDir / "scene.lua", [&registry](const std::string& path) {
		auto cfg = loadSceneConfig(path);
		auto* logger = RLLogger::getInstance();
		if (!cfg) {
			if (logger)
				logger->warn("hot_reload: scene.lua 解析失败（保留旧值）: {}", cfg.error().message);
			return;
		}
		registry.ctx().get<SceneSpin>().speed = cfg->spin ? 1.0f : 0.0f;
		auto& env = registry.ctx().get<SceneEnv>();
		env.exposure = cfg->exposure;
		env.aoStrength = cfg->aoStrength;  // Phase 11 批 11.4：SSAO 强度（热重载同路）
		env.intensity = cfg->environment.intensity;  // env 未上传时 mipRange=0 = shader 端无贡献，安全
		if (logger)
			logger->info("hot_reload: scene.lua 已套用 spin={} exposure={} env_intensity={}（entities/灯不热载）",
				cfg->spin, cfg->exposure, cfg->environment.intensity);
	});
	reloader->watch(sourceDir / "camera.lua", [&registry](const std::string& path) {
		auto cfg = loadCameraConfig(path);
		auto* logger = RLLogger::getInstance();
		if (!cfg) {
			if (logger)
				logger->warn("hot_reload: camera.lua 解析失败（保留旧值）: {}", cfg.error().message);
			return;
		}
		const Camera& src = cfg->camera;
		auto& cam = registry.ctx().get<Camera>();
		cam.fov = src.fov;
		cam.nearPlane = src.nearPlane;
		cam.farPlane = src.farPlane;
		cam.fpsSpeed = src.fpsSpeed;
		cam.fpsSensitivity = src.fpsSensitivity;
		cam.trackballSensitivity = src.trackballSensitivity;
		cam.zoomSpeed = src.zoomSpeed;
		cam.keys = src.keys;  // 键位表（CameraState 无键相关缓存，换表安全）
		if (logger)
			logger->info("hot_reload: camera.lua 已套用 fov={}（姿态/mode 不热套，重启生效）",
				glm::degrees(cam.fov));
	});
	reloader->watch(sourceDir / "log_level.json", [](const std::string& path) {
		RLLogger::reloadLevels(path);
	});

	registry.ctx().emplace<ConfigReloadHook>(ConfigReloadHook{
		.poll = [r = std::move(reloader)] { r->poll(); }});
	if (auto* logger = RLLogger::getInstance())
		logger->info("hot_reload: 已启动，监视源树 {}（scene.lua / camera.lua / log_level.json）",
			sourceDir.string());
}

/**
 * wireShaderHotReload 定义（契约见 main.cppm 接口注释）。复用 ConfigReloadHook
 * 帧边界挂点：config_reload 已挂 = 组合追加（配置链先跑），否则独占 emplace；
 * reloader 所有权在闭包内（ctx 活多久它活多久，同 10.3 模式）。init 失败
 * （编译期三件套缺失 / 基线 SPV 读不到）= 降级关，ShaderHotReloader 内部已日志。
 */
void wireShaderHotReload(entt::registry& registry, ShaderRegistry& pipelines,
	RenderBackEnd& backEnd) {
	if (!diagnostics().shaderReload)
		return;  // 总开关默认关 = 零行为变化（红线）
	auto reloader = std::make_shared<ShaderHotReloader>(pipelines, backEnd.renderer,
		backEnd.dgc.get(), backEnd.dgcStatic.get());
	if (!reloader->init())
		return;
	if (auto* hook = registry.ctx().find<ConfigReloadHook>(); hook && hook->poll) {
		auto prev = std::move(hook->poll);
		hook->poll = [prev = std::move(prev), r = std::move(reloader)] {
			prev();
			r->poll();
		};
	} else {
		registry.ctx().emplace<ConfigReloadHook>(ConfigReloadHook{
			.poll = [r = std::move(reloader)] { r->poll(); }});
	}
}

/**
 * wirePicking 定义（契约见 main.cppm 接口注释）：门关早退；门开 = 构造服务 +
 * 挂 Renderer + 双向函数桥（ecs 零依赖 picking 模块，同 ConfigReloadHook 闭包
 * 模式）。requestPick 只在左键按下产请求（右键 = 相机拖拽）；takeResult = 服务侧
 * 一次性消费。
 */
std::unique_ptr<PickingService> wirePicking(entt::registry& registry, Resources& resources,
	RenderTargetCache& rts, Renderer& renderer) {
	if (!diagnostics().picking)
		return nullptr;
	auto service = std::make_unique<PickingService>(resources, rts,
		renderer.getRenderGraphRef());
	renderer.setPickingService(service.get());
	PickingHook hook;
	hook.requestPick = [svc = service.get()](float x, float y) {
		svc->request(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
	};
	hook.takeResult = [svc = service.get()] { return svc->take(); };
	registry.ctx().emplace<PickingHook>(std::move(hook));
	return service;
}

}  // namespace RGL
