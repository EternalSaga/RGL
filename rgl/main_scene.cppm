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
import aa_target;  // aaTargetModel()/kModelKey（细线抗锯齿靶内存合成，批 12.6 T4）
import model_importer;
import texture_pool;
import scene_config;
import scene_context;
import taa_jitter;  // fsrJitterPhaseCount（抖动相位数，纯函数层；批 14.5 第四批）
import shader_registry;
import gpu_cull;
import vk_indirect_commands;
import vk_renderer;
import vk_swapchain;
import render_graph;
import builtin_geometry;
import asset_table;
import meshlet_asset;  // ★0-b2：loadMeshletAsset（产物读取侧，纯 CPU 无设备）
import meshlet_geometry;  // ★1-a0：uploadMeshletGeometry（簇产物 4 段的 GPU 侧持有者，rlgfx）
import diagnostics;
import shadow_config;  // shadowConfig() + shadowFormPlan()（IZB mask 条目门，批 N6b）
import rllogger;

namespace RGL {

namespace {

/**
 * 解包启动期 expected：失败即抛，消息 = 「标签: 底层描述」。
 *
 * 本文件此前有 **10 处**逐字同构的四行段 `if (!x) throw std::runtime_error(fmt::format(
 * "{} …失败: {}", x.error().describe()))`。批 14.4 加第 11 处（velocity RT）时收成一处——
 * 十份副本意味着"改错误消息口径"要改十处，而漏改的那处**只在失败路径上被读到**，
 * 正是最不容易被发现的那条路径（本项目在日志文案上漂过不止一次）。
 * 消息格式与旧代码逐字一致（`"{}: {}"` + 原标签），启动失败输出零变化。
 */
template <class Exp>
auto orThrow(Exp&& result, const char* what) {
	if (!result)
		throw std::runtime_error(fmt::format("{}: {}", what, result.error().describe()));
	return *result;
}

/**
 * ★0-b2 簇产物加载（**只加载、只校验、只打日志**——不上传 GPU、不画任何东西）。
 *
 * 边界（docs/mdi_plan.md §9 ★0-b2 + 主 agent 拍板）：GPU 上传归 ★1-a（那里才需要
 * buffer usage flags 与 shader 契约）。门关 = 调用方根本不进本函数（逐位不变是
 * **构造性成立**，不是测出来的）；门开 = 数据进 CPU 并打印统计，**图像仍逐位不变**
 * （这条比"门关不变"更强：它证明加载路径是惰性的）。
 *
 * 🔴 **不抛异常**：产物是可选加速资产（§7.8 拍板 3「缺失 → 回落现行路径」），坏产物
 * 不能砖掉引擎。失败打 WARNING 带可归因错误串（读取侧每条校验各返回不同错误串，
 * 见 meshlet_asset 文件头校验清单）；产物缺失打 INFO——**别静默**，静默会让"门开了
 * 但没产物"被读成"门开了但没生效"。
 * @param modelFile 模型路径（CWD 相对）；产物路径约定 = 本路径 + `.meshlets.bin`
 * @param store 装配层容器（所有权落点理由 → MeshletAssetStore 注释）
 * @return 加载成功 = 容器内该产物的稳定引用（★1-a0 直接喂上传，省一次查表）；
 *         未加载（缺产物 / 坏产物）= nullptr（本函数已按形态各打过一条 INFO/WARNING）
 */
const MeshletAsset* loadMeshletForModel(const std::string& modelFile, MeshletAssetStore& store) {
	const std::string binPath = modelFile + ".meshlets.bin";
	auto* lg = RLLogger::getInstance();
	std::error_code ec;
	if (!std::filesystem::exists(binPath, ec)) {
		if (lg)
			lg->info("meshlet: 门开但 {} 无簇产物（{} 不存在）：走现行路径", modelFile, binPath);
		return nullptr;
	}
	auto loaded = loadMeshletAsset(binPath);
	if (!loaded) {
		if (lg)
			lg->warn("meshlet: 产物加载失败 {}：{} —— 回落现行路径", binPath, loaded.error());
		return nullptr;
	}
	const auto& h = loaded->header;
	if (lg)
		lg->info("meshlet: {} ← {} 已加载: prims={} clusters={} groups={} "
				 "nodes={} levels={} vertices={} l0Tris={} sourceSha1={}",
			modelFile, binPath, h.primCount, h.clusterCount, h.groupCount, h.nodeCount,
			h.levelCount, h.vertexCount, h.l0TriangleCount, loaded->sourceSha1Hex.substr(0, 8));
	// std::map 节点地址稳定（插入后不搬家）⟹ 返回的指针在容器存活期内一直有效。
	return &store.byFile.emplace(modelFile, std::move(*loaded)).first->second;
}

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

/**
 * 批 14.5 第四批：把「抖动要不要开、序列周期是多少」写进 SceneEnv（抽函数控复杂度）。
 *
 * ⚠ 相位数**必须用与 FsrUpscaler 完全相同的两个尺寸**算：`initFsr` 传的是
 * `swapchain.getRenderExtent()` / `swapchain.getExtent()`（vkrenderer_offgraph.cppm:180），
 * vendor 内部拿同一对尺寸自己算 `JitterSequenceLength()`（`ffx_fsr3upscaler.cpp:1010`）。
 * 两边不一致 ⟹ shader 里的历史寿命衰减按错的周期算，**不报错、不崩，只是糊得没道理**。
 * 这也是把这两行放在装配层、而不是塞进 taaSystem 的理由：尺寸来源只有一处，且 ecs
 * 不许 import FSR 模块（分层红线）。
 */
void wireFsrJitter(entt::registry& registry, Swapchain& swapchain, bool fsrUsable,
	bool fsrJitter) {
	auto& env = registry.ctx().get<SceneEnv>();
	env.fsrJitter = fsrUsable && fsrJitter;
	env.fsrJitterPeriod = env.fsrJitter
		? fsrJitterPhaseCount(swapchain.getRenderExtent().width, swapchain.getExtent().width)
		: 0u;
	if (!env.fsrJitter)
		return;
	if (auto* lg = RLLogger::getInstance())
		lg->info("fsr_jitter: 抖动已接入投影并申报给 FSR（相位 {} = 渲染 {} / 上屏 {}）",
			env.fsrJitterPeriod, swapchain.getRenderExtent().width,
			swapchain.getExtent().width);
}

/** EVSSM 矩图槽（批 11.6d-2）：发号 + 失败抛出收进 helper（assembleBackEnd 复杂度
 *  红线先例：调用点零分支）。幂等（bakePipelineEntries 已发过号则直接复用同号）。 */
uint32_t evssmMomentsSlotOf(Resources& resources) {
	return orThrow(resources.ensureEvssmSampleSlot(), "EVSSM 矩图槽发号失败");
}

/**
 * G-buffer 领槽 + light 条目（Phase 7 D1；批 15.1 前半把领槽条件放宽到
 * `deferredMode || ssrDecorated` 后抽成 helper 控 assembleBackEnd 行数）。
 * 领槽 = ensureDeferredGBufferSlots（幂等，light / ssr 条目复用同一组槽号）；light
 * 条目仅 deferred 建（无 G-buffer 附件的自定义图接不了 light pass，ssr 装饰器的跳过条件 3
 * 也保证 ssrDecorated ⟹ 图里 kGBufferAttachments 契约表全部通道都在 ⟹ 槽可领）。
 */
std::optional<std::array<uint32_t, 5>> gBufferSlotsAndLightEntry(
	Resources& resources, SamplerLibrary& samplers, RenderTargetCache& rtCache,
	Swapchain& swapchain, ShaderRegistry& pipelines, bool deferredMode, bool ssrDecorated) {
	std::optional<std::array<uint32_t, 5>> gbSlots;
	if (deferredMode || ssrDecorated)
		gbSlots = orThrow(ensureDeferredGBufferSlots(rtCache, swapchain),
			"deferred: G-buffer RT 创建失败");
	if (deferredMode) {
		// light 条目下标由 Renderer 构造期按名 pipelineIndex("light") 取，此处仅需触发建条目
		// 批 11.6d-2：EVSSM 矩图槽（binding 15 无条件 bake，同 shadowSlot 纪律）。
		const uint32_t evssmSlot = evssmMomentsSlotOf(resources);
		[[maybe_unused]] auto lightIdx = pipelines.addLightEntry(
			(*gbSlots)[0], (*gbSlots)[1], (*gbSlots)[2], (*gbSlots)[3], (*gbSlots)[4],
			samplers.ensureShared(), deferredShadowSlotOf(rtCache), samplers.ensurePointSampler(),
			evssmSlot);
	}
	return gbSlots;
}

}  // namespace

/**
 * BLEND 图元的**装配期对账**（透明混合管线刀2，docs/透明混合管线_plan.md §1-4）。
 * 场景里没有 BLEND 图元 ⟹ 本函数**一条判据都不跑**（出厂 bistro = 122 OPAQUE + 10 MASK +
 * 0 BLEND，零成本零风险）。有 BLEND 而下面两条任一不过 = **拒启动**，不 warn + 继续：
 *
 * ① **图里没人消费透明段** ⟹ 整类几何静默不落地，而画面照出。这不是假想的：图变体收敛
 *    刀②.2 删 forward 之后就是这个状态，一直持续到本管线落地，而交付场景 0 BLEND 让它在
 *    仓库里静默躺了一天。判据 = 图事实 `graphDrawsTransparentSegment`（按 `pass.drawSource`
 *    而不是 state——要问的是"有没有人画那一段"）。
 * ② **没有 env cubemap** ⟹ 透明 frag 声明的 binding 4/5 拿不到 mapping
 *    （`addModelEntry` 只在 `Environment::get()` 非空时置 `hasEnv`），症状是建 shader 时
 *    VVL 报 `VUID-VkShaderCreateInfoEXT-flags-11292`——**2026-10-18 实测**（临时注释掉
 *    scene.lua 的 environment 段 + transparent.glb）确实起不来，方向是对的（响亮），
 *    但那条消息对不知情人等于天书，所以提前到装配期点名。
 *
 * 🔴 **时序契约 = `uploadEnvironment` 之后、`bakePipelineEntries` 之前**（调用点在本仓
 * main_impl 的 C1 前一行）。这条不是风格：②的崩溃就发生在 C1 烘焙里——本对账第一版放在
 * `assembleBackEnd`（C3），实测**根本没跑到**就先撞 VUID-11292（2026-10-18）。
 * 同理判据源只能是 `UploadedAssets` 里的 `GltfPrimitive.alphaModeBlend`，不能是
 * `AssetTable` 的材质记录——材质要到 C2 `registerScene` 才进表，那时 C1 早烘完了。
 *
 * 为什么住装配层而不是 gate_lint：lint 的契约明写「lint 不阻止启动（诊断门不该让程序起不来）」
 * （`main_impl.cppm::runGateLint` 头注释），而这两条的失败形态正是本仓反复登记的病
 * ——「配了能力却静默拿到出厂图」，与债-129 S3-C 把六个 warn 出口反转成 throw 同一条裁决。
 */
void validateBlendSupport(const UploadedAssets& uploaded, const Environment& env,
	const CompiledRenderGraph& graph) {
	const bool anyBlend = std::ranges::any_of(uploaded.models,
		[](const UploadedModel& um) {
			return std::ranges::any_of(um.model->primitives,
				[](const GltfPrimitive& p) { return p.alphaModeBlend; });
		});
	if (!anyBlend)
		return;
	if (!graphDrawsTransparentSegment(graph.passes))
		throw std::runtime_error(
			"场景含 alphaMode=\"BLEND\" 图元，但渲染图里没有任何 pass 的 draws = \"transparent\" —— "
			"透明图元会被静默丢弃（renderListSystem 照旧排那一段，只是没人画它）。"
			"修法：内建 deferred 图本来就带 transparent 段（删掉 configs/render_graph.lua 的 "
			"attachments/passes 两键即回落），自定义图请补一段 "
			"{ state = \"transparent\", draws = \"transparent\" } 并让它 load scene_hdr。");
	if (env.get() == nullptr)
		throw std::runtime_error(
			"场景含 alphaMode=\"BLEND\" 图元，但 configs/scene.lua 没有 environment 段 —— "
			"透明前向着色（transparent.frag）要采 env cubemap（binding 4/5），没有 env 资产就烘不出 "
			"mapping，Vulkan 会在建 shader 时报 VUID-VkShaderCreateInfoEXT-flags-11292。"
			"修法：给 scene.lua 配 environment 段（hdr 或 dir/faces），或把该资产的材质改成 OPAQUE。");
}

/** A0 内建 cube + A1 解析/顶点上传（全几何槽先于任何纹理槽）+ A2 纹理上传。
 *  ★0-b2：A1 去重循环里挂簇产物加载（门与惰性边界 → 接口注释 + loadMeshletForModel）。
 *  ★1-a0：加载成功后同处挂簇产物 4 段 GPU 上传（边界与回落口径 → meshlet_geometry）。 */
UploadedAssets uploadModelAssets(DeviceContext& device, Resources& resources,
	SamplerLibrary& samplers, AssetTable& assets, const SceneConfig& scene,
	bool meshletAssets, MeshletAssetStore& meshlets, MeshletGeometryStore& meshletGeometry) {
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
		// 批 12.6 T4：aa_target = **内存合成**的 GltfModel（aa_target.cppm），键固定、
		// 与真模型走同一条去重/上传/注册路——不在 UploadedAssets 另立字段（那 = 第二份真相）
		const bool target = ent.type == "aa_target";
		if (ent.type != "model" && !target)
			return;
		const std::string key = target ? std::string(aaTarget::kModelKey) : ent.file;
		if (auto [it, inserted] = out.modelOf.emplace(key, out.models.size()); !inserted) {
			out.models[it->second].instances++;
			return;  // 同文件已上传（instancing 共享资产）
		}
		std::unique_ptr<GltfModel> mesh;
		if (target)
			mesh = std::make_unique<GltfModel>(aaTargetModel());  // 零文件依赖、零新 shader
		else {
			auto modelResult = loadGltfModel(ent.file);
			if (!modelResult)
				throw std::runtime_error(modelResult.error().describe());
			mesh = std::make_unique<GltfModel>(std::move(*modelResult));
		}
		auto assetIdsResult = uploadModelMeshes(device, resources, assets, *mesh);
		if (!assetIdsResult)
			throw std::runtime_error(assetIdsResult.error().describe());
		out.models.push_back(UploadedModel{.file = key, .model = std::move(mesh),
			.assetIds = std::move(*assetIdsResult), .instances = 1});
		// ★0-b2 簇产物加载 + ★1-a0 4 段 GPU 上传（按 file 去重后每文件至多一次）。门关 = 本块
		// 不执行（逐位不变构造性成立）；门开也只进 meshlets / meshletGeometry 两个容器 + 日志，
		// 不碰上面的上传路径、不建条目、不领 heap slot ⟹ 图像仍逐位不变。
		// aa_target 是内存合成模型（key 不是文件路径，无"路径旁的产物"可言）不适用。
		// 上传的失败口径（WARNING + 不落容器）→ meshlet_geometry::uploadMeshletGeometryInto。
		if (!meshletAssets || target)
			return;
		if (const MeshletAsset* loaded = loadMeshletForModel(key, meshlets); loaded != nullptr)
			uploadMeshletGeometryInto(*loaded, key, meshletGeometry, resources.getAllocator(), device.getDevice());
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
			orThrow(loadEnvCubeMapHdr(resources, env, device, envCfg.hdr,
				envCfg.hdrSize, "scene.environment"), "environment");
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

/** 风格 → 管线规格名（GltfPrimitive 适配层；判据本体 = scene_config::resolvePipelineSpecName，
 *  搬出去的理由 = 那条 blend 护栏要能被 `just test` 打到，而 main 的模块分区只有可执行文件
 *  能 import。原 forward 半边（styleMap + 缺 style 抛错）已随刀②.2 删除，
 *  docs/图变体收敛_plan.md §5。 */
std::string resolvePrimitiveSpecName(const SceneConfig& scene, const GltfPrimitive& prim) {
	return resolvePipelineSpecName(scene, prim.materialName, prim.unlit, prim.alphaModeBlend);
}

/** C1 管线条目烘焙（per unique 模型；风格派生 + 条目去重 + DIAG 分布日志）。 */
void bakePipelineEntries(UploadedAssets& uploaded, const SceneConfig& scene,
						 ShaderRegistry& pipelines, Resources& resources, SamplerLibrary& samplers,
						 AssetTable& assetTable, RenderTargetCache& rtCache) {
	const uint32_t samplerSlot = samplers.ensureShared();
	// Phase 11 批 11.2：阴影图槽必须在**模型条目烘焙之前**领掉（堆槽号由 ensure 时分配，
	// 烘焙后再拿到就太晚）。与 env 槽同轴（AGENTS：环境上传 = A2 后、addModelEntry 前）。
	// 无条件建（不看 diagnostics.shadow_pass）——光照管线无条件烘这个槽。
	// assembleHdrAoChain 里的同名 ensure 是幂等的（deferred light 条目用）。
	const auto shadowSlot = orThrow(ensureShadowMapSlot(rtCache, kShadowMapSize),
		"阴影图 RT 创建失败");
	// 批 11.6d-2：EVSSM 矩图槽条目传入前发号（binding 15 **无条件传入**，shadowSlot
	// 同纪律；门关 = -DEVSSM_SOFT 变体不声明该 binding = 批 M 过滤不烘，真描述符由
	// EvssmShadow 构造时覆写同槽）。幂等，失败处理收 evssmMomentsSlotOf。
	const uint32_t evssmSlot = evssmMomentsSlotOf(resources);
	for (auto& um : uploaded.models) {
		auto& model = *um.model;
		const auto& texIds = um.textureIds;
		// 管线条目（Phase 4 批次 2：按 (规格,顶点 slot,base 纹理 slot,normal slot,
		// orm slot,emissive slot) 六元组组合去重——同模型全 primitive 共享一个拼接顶点
		// slot，仅纹理槽差异决定条目拆分；纹理/sampler slot 烘焙进 binding mapping，每帧
		// 零成本；Phase 8 ③a 扩 normal/orm 槽、③ 扩 emissive 槽）。批次 3：风格派生（loader unlit
		// 字段即数据源，与 MaterialAsset 一致性由 registerScene 金丝雀保证）→ 风格键经
		// deferred_styles 表映射到管线规格名（缺表回落 deferred_geometry；forward 的 styles 表
		// 半边已随刀②.2 删除）。
		// Phase 8 批次 F：材质风格 = scene.lua material_styles 连连看（材质名含 pattern →
		// 风格键，有序首个命中；默认规则 {Emissive→light, Foliage→vegetation}）。deferred 经
		// deferredStyleMap 落规格名（植被/光源两路专属视觉对齐；deferred 变体把类别写进
		// G-buffer emissive.a，light.frag 分支复刻已删除的 forward 视觉——历史对偶）。
		// 风格 → 管线规格名 = resolvePrimitiveSpecName 共享函数（★1-a1ii-α：派生逻辑抬出，
		// 叠加条目与本体条目同源；原 lambda 删除——独立计分注释随宿主一并搬至定义处）。
		auto addPrimitiveEntry = [&](const GltfPrimitive& prim, size_t i) {
			const uint32_t id = um.assetIds[i];
			const uint32_t entryIdx = pipelines.addModelEntry(
				resolvePrimitiveSpecName(scene, prim),
				assetTable.get(id).heapSlot,  // = 模型拼接顶点 slot（同模型全 primitive 相同）
				assetTable.getTexture(texIds.base[i]).heapSlot,
				assetTable.getTexture(texIds.normal[i]).heapSlot,
				assetTable.getTexture(texIds.orm[i]).heapSlot,
				assetTable.getTexture(texIds.emissive[i]).heapSlot,
				samplerSlot, shadowSlot, samplers.ensurePointSampler(), evssmSlot);
			pipelines.bindToAsset(id, entryIdx);
			// Phase 11 批 11.2：阴影 depth-only 管线条目。按 vertexSlot 去重（addShadowEntry
			// 内部有表）——bistro 1591 个 primitive 共用约 8 个拼接块槽 → 仅 8 条目。
			// 无条件分支：门控关时条目与 shadowCalls 都为空，GPU 产生一个未被引用的管线
			// 条目（零成本；AGENTS「条目全按需建」纪律）。
			pipelines.bindShadowToAsset(id, pipelines.addShadowEntry(assetTable.get(id).heapSlot));
			// 批 11.3-1d 后半：IZB mask 条目（门关零创建；重放点经 vertexSlot 反查，
			// 不挂资产——同 vertexSlot 去重）。门 = shadow.lua 的 izb.on（N6b）
			if (shadowFormPlan(shadowConfig()).consumeIzbMask)
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

/** 克隆管线条目（门 = diagnostics.picking；契约见接口注释）。 */
void addDiagnosticCloneEntries(ShaderRegistry& pipelines) {
	if (!diagnostics().picking)
		return;
	pipelines.addCloneEntries(ClonePurpose::picking, "generated_shaders/picking.frag.spv");
	pipelines.addCloneEntries(ClonePurpose::highlight, "generated_shaders/highlight.frag.spv");
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
	// 批 12.6 T4：aa_target 与 model 同路（差别只在去重键 = 合成模型的固定键）
	for (const auto& ent : scene.entities) {
		const bool target = ent.type == "aa_target";
		if (ent.type != "model" && !target)
			continue;
		auto& um = assets.models[assets.modelOf.at(
			target ? std::string(aaTarget::kModelKey) : ent.file)];
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
	// 进 scene data @0 头单灯，全数组按 index 落灯数组区）。灯无资产/上传/矩阵槽，逻辑内联最简。
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
 * 为何无条件建（不分图形态）：内建 deferred 图含 ao + tonemap pass（forward 图已随刀②.2
 * 删除）；
 * 自定义图可能两者都不含，条目闲置无害（管线表条目不绑定即零成本）。
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
 * @param taaDecorated 批 12.6：TAA 已装饰落图（启动期 decorateTaaPass 结果）。true =
 *        追加 taa_out/taa_hist RT + taa 条目，且 tonemap 色槽烘 taa_out；false = 逐位不变
 * @param velocityDecorated 批 14.4：速度场已装饰落图。true = 追加 velocity RT + velocity
  * @param fsrEnabled      批 14.5：FSR upscaler 门（装配层已把
  *                        「velocity 真的进了图」并进本参数，见 main_impl）
 *        条目（**不改任何既有 binding**，与 taa 换 tonemap 色槽不同——速度场暂无下游）；
 *        false = 逐位不变
 * @param ssrDecorated    批 15.1 前半：SSR pass 已装饰落图（批 15.3B 起 = trace + apply
 *                        两个 pass）。true = 追加 ssr_trace / ssr_applied RT + 两条目，且
 *                        taa / ao_apply / tonemap 的色输入槽按优先级链
 *                        ao_applied > taa_out > ssr_applied > scene_hdr 跟着换（图上输入名
 *                        的改写由 decorateSsrPass 做，这里换的是**烘进管线条目的堆槽**——
 *                        两条通道漏一条 = 「图说采 A、描述符指向 B」= 采到从未转换布局的
 *                        图，VVL 报布局不符，教训原文见函数体内「色输入优先级」注释）
 * @param gbSlots         ssr 条目要烘的 G-buffer 5 槽（ensureDeferredGBufferSlots 产物；
 *                        ssrDecorated 为真时调用方必须给值——装饰器跳过条件保证彼时图里
 *                        必有 G-buffer 附件）
 */
void assembleHdrAoChain(Swapchain& swapchain, Resources& resources,
	SamplerLibrary& samplers, RenderTargetCache& rtCache, ShaderRegistry& pipelines,
	bool taaDecorated, bool velocityDecorated, bool fsrUsable = false,
	bool aoApplyDecorated = false, bool ssrDecorated = false,
	const std::optional<std::array<uint32_t, 5>>& gbSlots = {}) {
	const auto hdrSlot = orThrow(ensureSceneHdrSlot(rtCache, swapchain), "scene_hdr RT 创建失败");
	const auto aoSlot = orThrow(ensureAoSlot(rtCache, swapchain), "ao RT 创建失败");
	const auto depthSlot = orThrow(resources.ensureDepthSampleSlot(), "深度采样槽分配失败");
	// Phase 11 批 11.2：阴影图槽。**无条件建**（不看 diagnostics.shadow_pass）——光照管线条目
	// 无条件烘焙它（否则开关时描述绑定的图从未被任何 pass 转布局 = 采样就报 VVL）。
	// 门控在渲染侧：关时阴影段零 draw，图内全 1.0 = 无遮挡 → 成片逐位不变。
	orThrow(ensureShadowMapSlot(rtCache, kShadowMapSize), "阴影图 RT 创建失败");
	// ao / tonemap 条目下标由 Renderer 构造期按名 pipelineIndex("ao"/"tonemap") 取
	[[maybe_unused]] auto aoIdx = pipelines.addAoEntry(depthSlot, samplers.ensurePointSampler());
	// 批 12.6 TAA（门默认关 = taaDecorated false = 本段整体跳过 = 槽/条目零变化）：
	// ① 两幅 TAA RT 在 addTaaEntry 之前领槽（同 scene_hdr 契约：槽号烘进 binding，
	//    resize 只重写描述符）；② **tonemap 的色输入槽换成 taa_out**——描述符绑定
	//    是启动期烘死的（binding → 固定 heap 槽），图上的输入名改写只管 barrier，
	//    真正把"tonemap 读哪张图"换掉的是这里换烘焙槽号（两条通道必须同时换，
	//    漏一条 = 读旧图或布局链错）。
	std::optional<std::array<uint32_t, 2>> taaSlots;
	if (taaDecorated) {
		taaSlots = orThrow(ensureTaaSlots(rtCache, swapchain), "TAA RT 创建失败");
	}
	// 批 15.1 前半：ssr 在图里时领 ssr_applied 槽（同 scene_hdr 契约：槽号一经烘进管线
	// binding 即终身有效，resize 只重写描述符内容）。门关 = 不领槽 = 堆槽布局零差异。
	// 批 15.3B：trace RT（半分辨率）同段领槽——两槽都在两个 addSsr*Entry 之前。
	std::optional<uint32_t> ssrSlot, ssrTraceSlot;
	if (ssrDecorated) {
		ssrTraceSlot = orThrow(ensureSsrTraceSlot(rtCache, swapchain), "ssr_trace RT 创建失败");
		ssrSlot = orThrow(ensureSsrAppliedSlot(rtCache, swapchain), "ssr_applied RT 创建失败");
	}
	// 批 14.5 第二批：色输入优先级 **FSR > TAA > scene_hdr**。FSR 在图里时它读
	// taa_out/scene_hdr、写自己那张私有图，这里把它的输出采样槽**烘进 tonemap 条目**——
	// 零拷贝（不新建图附件、不动图词表），先例 = EvssmShadow 的 momentsSampleSlot。
	// ⚠ 判据由调用方算好传进来（`fsrUsable`），本函数**不再自行判断**：槽号一旦烘进管线条目
	//   就必须有人写描述符，两处判据漂移 = 读到未初始化描述符（`Renderer::initFsr` 是另一处）。
	// 批 14.5 第三批：ao_apply 在图里时**图**已把 tonemap 的色输入换成 ao_applied
	//（decorateAoApplyPass 干的），所以烘进条目的堆槽必须跟着换——否则「图说采 A、描述符
	// 指向 B」= 采到没转布局的图（VVL 报布局不符）。
	// 批 15.1 前半：优先级 **ao_applied > taa_out > ssr_applied > scene_hdr**——ssr_applied
	//   档插在 scene_hdr 之上：SSR 与 taa / ao_apply 是**串联**（SSR 先合成、它们后处理），
	//   装饰器插入序保证 tonemap 在链尾看到的永远是链头产物。FSR 开时整条被 FSR 的
	//   **零拷贝输出槽**顶掉（批 14.5 第二批那条路），两者写同一个 binding、互斥。
	// 写法 = 顺序覆盖（低优先级先赋、高优先级后覆盖），与嵌套三目逐位等价；用三目会把
	// 本函数认知复杂度推过 15 的检查红线（每层嵌套 +2）。
	uint32_t graphColorSlot = hdrSlot;
	if (ssrSlot)
		graphColorSlot = *ssrSlot;
	if (taaSlots)
		graphColorSlot = (*taaSlots)[0];
	if (aoApplyDecorated) // ao_applied 槽只在真要建条目时领（门关 = 堆槽零变化）
		graphColorSlot = orThrow(ensureAoAppliedSlot(rtCache, swapchain), "ao_applied RT 创建失败");
	const uint32_t colorSlot = fsrUsable
		? orThrow(resources.ensureFsrSampleSlot(), "FSR 输出采样槽领取失败") : graphColorSlot;
	[[maybe_unused]] auto tonemapIdx = pipelines.addTonemapEntry(colorSlot, aoSlot,
		samplers.ensureShared());
	// 批 15.1 前半：SSR 专用 pass 的条目。批 15.3B 拆成 **trace + apply 两条目**（图上
	// 也是两个 pass）。色输入**恒为 scene_hdr**——decorateSsrPass 插在 velocity/taa/
	// ao_apply 之前，两 pass 读的本帧成片永远是 light 的产物（跳过条件保证它们不可能读到
	// 下游）。G-buffer normal/position/metalrough = 烘 5/6/7（镜像 light.frag 通道号）；
	// 深度复用 ao 那一份采样槽（不另领，先例同 velocity）。
	if (ssrDecorated) {
		// trace：G-buffer 通道号镜像 light.frag：[0]=albedo（binding 9，fresnel F0）/[1]=normal/
		// [2]=position/[3]=metalrough；emissive（[4]）SSR 不消费。sampler = 共享 LINEAR，
		// 只服务命中点那一次 Sample（逐像素自身量全 Load）。env 减法源（11/12）在条目内烘。
		[[maybe_unused]] auto ssrTraceIdx = pipelines.addSsrTraceEntry(hdrSlot, (*gbSlots)[1],
			(*gbSlots)[2], (*gbSlots)[3], depthSlot, (*gbSlots)[0], samplers.ensureShared());
		// apply：读 scene_hdr（直通基准 Load）+ ssr_trace（模糊 taps Sample）+ metalrough
		// （半径自变量）。env **不再需要**——减法留在 trace（裁决 → ssr_trace.frag 文件头）。
		[[maybe_unused]] auto ssrIdx = pipelines.addSsrApplyEntry(
			hdrSlot, *ssrTraceSlot, (*gbSlots)[3], samplers.ensureShared());
	}
	// 批 14.5 第三批：AO 施加专用 pass 的条目。色输入 = SSR 在图时 ssr_applied、否则
	// scene_hdr——本 pass 的跳过条件保证「taa 在图里就不插」，所以它读的是装饰器插入序
	// 上自己上游的那张成片（SSR 先插 ⟹ 上游 = ssr_applied），不可能需要读 taa_out。
	if (aoApplyDecorated) {
		// ⚠ 同下面 taa 那条：本行的 `ssrSlot ? *ssrSlot : hdrSlot` 也零测试覆盖（缺口 → phase15_plan §3.2）。
		[[maybe_unused]] auto aoApplyIdx = pipelines.addAoApplyEntry(
			ssrSlot ? *ssrSlot : hdrSlot, aoSlot, samplers.ensureShared());
	}
	if (taaDecorated) {
		// 色输入同 ao_apply 那条理由：SSR 在图时 taa 的上游 = ssr_applied（图上输入名已由
		// decorateSsrPass 改写，烘进条目的堆槽必须同步）。
		// ⚠ **本行当前零测试覆盖**：把它退回 `hdrSlot`（= 图说读 ssr_applied、描述符指
		//   scene_hdr）时单测 275 例全绿、`--ssr --aa taa --syncval` errors=0、出图正常
		//   ——两张图同在 eShaderReadOnlyOptimal，VVL 无从分辨（缺口论证 → phase15_plan §3.2）。
		//   改它之后要手测：`aa=taa` 档 SSR 开/关差分**必须不同**。
		[[maybe_unused]] auto taaIdx = pipelines.addTaaEntry(
			ssrSlot ? *ssrSlot : hdrSlot, (*taaSlots)[1], depthSlot, samplers.ensureShared(),
			samplers.ensurePointSampler(), aoSlot);
	}
	// 批 14.4 速度场（门默认关 = velocityDecorated false = 本段整体跳过 = 槽/条目零变化）：
	// 领 velocity RT 槽 + 建条目两件事，**不动任何既有 binding**——速度场目前没有消费者
	// （14.5 接 FSR 时才多一个下游），所以不像 taa 那样换 tonemap 的色槽。深度采样槽
	// **复用 ao 那一份**（上面已 ensureDepthSampleSlot），不另领：sampler/描述符堆槽
	// 自己算偏移是本项目的事故先例（vkresources.cppm:350）。
	if (velocityDecorated) {
		orThrow(ensureVelocitySlot(rtCache, swapchain), "velocity RT 创建失败");
		[[maybe_unused]] auto velIdx = pipelines.addVelocityEntry(depthSlot);
	}
}

/** 批 14.5 第二批：FSR 生效判据（装配层一次算完，**不懒**）。四条全真才接：门 + velocity
 *  真的进了图（FSR 的 motionVectors 输入就是那张图）+ 设备有 shaderFloat16/Int16（FSR 的
 *  SPIR-V 声明了这两个 capability，见 vkdevice.cppm 同批注释）+ 三个输入附件齐备。
 *
 *  🔴 **第三条（齐备）的唯一来源 = `render_graph` 的图事实查询面 `graphFsrInputs`**
 *  （债-129 S1a 收口）。收口前这里自己写一份 `(has(taa_out) || has(scene_hdr)) && …`，
 *  **色输入那一档漏了 `ao_applied`**，与 `Renderer::initFsr` 的三分支链条件集不同
 *  （4 条 vs 3 条）——两处判据各写一份策略，就是这次正交化要治的病本体。
 *
 *  🔴 **第一条与第二条不在查询面里**，且不是偷懒：velocity 附件在图里 ≠ 有人在写它
 *  （装饰落图才是），而「装饰落没落」今天只有装饰器知道 → S1b 把它变成图带出的来源记录。
 *  设备能力是设备事实（GateEnv 那一档）。
 *
 *  之所以不能留到运行期再判：装配层要把 FSR 的输出采样槽**烘进 tonemap 管线条目**，烘了却
 *  没人写描述符 = 读到未初始化描述符。`Renderer::initFsr` 是同一判据的另一半（它只查齐备
 *  那一条，不满足即抛）——两半现在共享同一个 `graphFsrInputs`，漂移在**共享的那一半**上
 *  结构性不可能；剩下的两半本来就该只有一处判。
 *
 *  抽成函数而不是内联在 `assembleBackEnd` 里，是因为内联版把该函数的认知复杂度推到 20、
 *  函数体推到 101 行，双双超 style 红线。
 */
bool computeFsrUsable(DeviceContext& device, const CompiledRenderGraph& graph,
	bool fsrEnabled, bool velocityDecorated, bool taaDecorated) {
	const bool inputsReady = graphFsrInputs(graph.attachments).complete();
	const bool usable = fsrEnabled && velocityDecorated && device.shader16Supported() && inputsReady;
	if (fsrEnabled && !usable) {
		if (auto* lg = RLLogger::getInstance())
			lg->warn("aa=\"fsr\" 开门但接不起来（velocityDecorated={} shader16={} 输入齐备={})"
				" ⟹ 不接 FSR，tonemap 色输入保持 {}",
				velocityDecorated, device.shader16Supported(), inputsReady,
				taaDecorated ? "taa_out" : "scene_hdr");
	}
	return usable;
}


/**
 * 批 14.5 第七批：**TAA 与 FSR 时域互斥**——这不是画质偏好，是结构关系。
 *
 * FSR3-upscaler 自己就是带累积的时域升频器 ⟹ 接在 TAA 后面 = 两个时域滤波器串联
 * = 双重模糊，**就算抖动簿记全修对也一样**。让它可配 = 等着被人配出"画面糊了"再当
 * 调参问题去调（本 phase 已在这上面烧掉一整批）。
 *
 * 🔴 **批 14.6 之后本函数的拦截路径已结构上不可达**：`fsrUsable` 蕴含 `aa == fsr`、
 * `taaDecorated` 蕴含 `aa == taa`，而 `aa` 是一个槽位只有一个值的 enum——「两个都开」
 * 这种配置**根本写不出来**（这正是三态取代两个布尔的目的：旧失效形态 = 引擎静默把
 * 配置里的 true 判关，申报值与生效值分家）。留着它是**防御性断言**而不是机制：若哪天
 * 有人把接线改回两个独立来源，本函数会当场 warn 出声，而不是让串联悄悄上线。
 *
 * 判据取 `taaDecorated`（**实际图形态**）而非配置推导值：配置为真但装饰
 * 失败时色输入并不是 taa_out，拦它属于误拦（先例 = AO 施加点、`fsrUsable` 本身）。
 *
 * @param fsrUsable    FSR 真的接进了图（含 velocity 装饰成功）
 * @param fsrTemporal  runtime.lua `fsr` 块的 temporal 调参
 * @param taaDecorated TAA pass 真的在编译图里
 * @return 实际生效的时域开关（被拦时 = false，并出 warn）
 */
bool resolveFsrTemporal(bool fsrUsable, bool fsrTemporal, bool taaDecorated) {
	// **生效锚点**（`tools/smoke.py --fsr-temporal` 的双向核对读这一行）。为什么必须有：本函数
	// 曾在 TAA 在图时把调参**判关**，而配置里写的仍然是 `true` ⟹ 只看配置会以为
	// 开着。与 `fsr_jitter` 那条同族：申报值与生效值必须现场可读。
	// ⚠ 文本格式被 smoke.py 的正则钉住（批 14.6 只改注释不改文案）。
	// 为什么写在**本函数**而不是调用方：`assembleBackEnd` 的认知复杂度已经压线（实测加两行
	// 即超 15 被 source_checks 点名），而"生效状态"本来就是本函数的职责所在。
	const auto logState = [&](bool effective) {
		if (!fsrUsable)
			return;  // FSR 没接进图 ⟹ 本键无意义，不往启动日志里加噪声
		if (auto* lg = RLLogger::getInstance())
			lg->info("fsr temporal: {}（配置键 fsr_temporal = {}，TAA 在图 = {}）",
				effective ? "开 = vendor 累积历史" : "关 = 纯空间 EASU+RCAS",
				fsrTemporal ? "true" : "false", taaDecorated ? "true" : "false");
	};
	if (!fsrUsable || !fsrTemporal || !taaDecorated) {
		logState(fsrTemporal);
		return fsrTemporal;
	}
	if (auto* lg = RLLogger::getInstance())
		lg->warn("fsr_temporal 与 TAA 互斥（两个时域滤波器串联 = 双重模糊）："
				 "已强制时域关，退化为纯空间升频");
	logState(false);
	return false;
}

/** C3 候选表 + GpuCull + deferred + HDR + DGC + Renderer（装配时序见接口注释）。 */
RenderBackEnd assembleBackEnd(DeviceContext& device, Swapchain& swapchain, Resources& resources,
	SceneDataService& sceneData, SamplerLibrary& samplers, Environment& env,
	RenderTargetCache& rtCache, AssetTable& assets,
							  ShaderRegistry& pipelines, CompiledGraphResult&& graphResult,
							  entt::registry& registry,
							  bool fsrEnabled, bool fsrTemporal, bool fsrJitter) {
	const bool deferredMode = graphResult.deferredMode;
	const bool taaDecorated = graphResult.taaDecorated;
	const bool velocityDecorated = graphResult.velocityDecorated;
	const bool aoApplyDecorated = graphResult.aoApplyDecorated;
	const bool ssrDecorated = graphResult.ssrDecorated;
	// 批 13.6：AO 施加点标志 = **实际图形态**（`decorateTaaPass` 结果），**不是**配置推导值
	// （`aaWantsTaa()`）。true ⟹ AO 已在 tonemap **之前**施加过，tonemap 跳过；false ⟹ 没有更早的
	// 施加点（S3-C 起装饰失败即拒启动，false 只剩「aa 不要 taa」一种来源），AO 留在 tonemap 施加。
	// 取错源 = AO 双施或零施。消费者 = scene data @723600.z → fillTaaData / tonemap.frag。
	// 批 14.5 第三批：来源从「只有 taa」变成**两个**——`taaDecorated`（AO 乘在 taa.frag 里，
	// 批 13.6）或 `aoApplyDecorated`（AO 乘在专用 ao_apply pass 里，与"谁做时域"解耦）。
	// 两者**互斥**：decorateAoApplyPass 的第一条跳过条件就是「taa 在图里」。
	registry.ctx().get<SceneEnv>().aoAppliedPreTemporal = taaDecorated || aoApplyDecorated;
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
			// 🔴 **指定初始化**不是风格选择（债-118 实测教训）：位置聚合初始化与字段名**无关**——
			// CullEntry 现在是反射生成物，改 cull_tables.slang 的字段名会编译失败，但**同类型字段换序**
			// （objectSlot/pipelineIndex/groupCount… 全是 uint32_t）在位置初始化下照样编得过、值静默串位。
			// C++20 要求指定初始化按声明序书写 ⟹ 换序也编不过。两个洞一起堵，本处才是"改布局要编译期
			// 知道"的承重墙（同款理由逐字见 gpu_cull_impl.cppm 的 FanoutPush 注释）。
			cullEntries.push_back(CullEntry{.centerX = cull.center.x, .centerY = cull.center.y, .centerZ = cull.center.z,
				.radius = cull.radius, .objectSlot = meshRef.objectSlot, .pipelineIndex = asset.pipelineIndex,
				.groupCount = asset.groupCount, .totalTriangles = asset.triangleCount, .vertexOffset = asset.vertexOffset,
				.baseColor = mat.baseColor, .roughnessMetallic = roughnessMetallic(mat)});  // 字段序单源 = asset_table::roughnessMetallic
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
	// 注释）；先构造（sky/light 条目等杂项条目其后加入，见上 N 快照契约）
	auto cull = std::make_unique<GpuCull>(device, resources, sceneData, cullEntries,
		dgcStaticEnabled ? dgcStaticEntryCount : 0u);

	// 天空盒管线条目（addSkyEntry + SceneEnv 回填）已随 forward 绘制机制删除
	// （2026-10-18 图变体收敛刀②.2，docs/图变体收敛_plan.md §6.2：deferred 的天穹对应物
	//  = env.intensity 总闸）。

	// Phase 7 D1：deferred 资源。装配序——env/models 已分配，此处才分配
	// G-buffer RT 堆槽（运行时段）+ 建 light 管线，再构造
	// Renderer（其构造期校验 drawLight pass 有 light 条目）。图无 deferred 光照 pass 时全跳过
	// （批 15.1 前半：领槽条件放宽到 ssr 也要槽，本体见 gBufferSlotsAndLightEntry）。
	const auto gbSlots = gBufferSlotsAndLightEntry(resources, samplers, rtCache, swapchain,
		pipelines, deferredMode, ssrDecorated);
	// 批 14.5 第二批：FSR 生效判据在装配层一次算完（判据本体见 computeFsrUsable）。
	const bool fsrUsable = computeFsrUsable(device, graphResult.graph, fsrEnabled,
		velocityDecorated, taaDecorated);
	// 批 14.5 第四批：抖动解耦（判据与"为什么在装配层算相位数"见 wireFsrJitter）
	wireFsrJitter(registry, swapchain, fsrUsable, fsrJitter);
	assembleHdrAoChain(swapchain, resources, samplers, rtCache, pipelines, taaDecorated,
		velocityDecorated, fsrUsable, aoApplyDecorated, ssrDecorated, gbSlots);

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

	// 清晰选路日志（启动期一次）：哪个设备走 DGC execute / 静态 execute / 读回重放。
	// 末尾附 `cull_replay`（债-30 方案 C）：本行说的是**能力**选了哪条，而"opaque 段到底
	// 由谁驱动"由该门决定（cpu/gpu_lagged = CPU 剔除驱动，下面三条都不会被走到）。
	if (auto* logger = RLLogger::getInstance()) {
		std::string devName(device.getPhysicalDevice().getProperties().deviceName.data());
		if (dgc && dgc->active())
			logger->info("DGC path: device='{}' graphicsIndirect={} computeIndirect={} static={} -> DGC execute (vkCmdExecuteGeneratedCommandsEXT) [cull_replay={}]",
				devName, dgcSup.graphicsIndirect, dgcSup.computeIndirect, dgcSup.staticGraphicsIndirect, cullReplayName(diagnostics().cullReplay));
		else if (dgcStatic && dgcStatic->active())
			logger->info("DGC path: device='{}' graphicsIndirect={} computeIndirect={} static={} -> DGC static execute ({} entries × vkCmdExecuteGeneratedCommandsEXT) [cull_replay={}]",
				devName, dgcSup.graphicsIndirect, dgcSup.computeIndirect, dgcSup.staticGraphicsIndirect,
				dgcStatic->entryCount(), cullReplayName(diagnostics().cullReplay));
		else
			logger->info("DGC path: device='{}' graphicsIndirect={} computeIndirect={} static={} -> 读回重放 (CPU 回退) [cull_replay={}]",
				devName, dgcSup.graphicsIndirect, dgcSup.computeIndirect, dgcSup.staticGraphicsIndirect, cullReplayName(diagnostics().cullReplay));
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
	// 批 14.5 第七批：TAA 与 FSR 时域互斥（判据与理由见 `resolveFsrTemporal`）。
	const bool temporal = resolveFsrTemporal(fsrUsable, fsrTemporal, taaDecorated);
	return RenderBackEnd{std::move(cull), std::move(dgc), std::move(dgcStatic),
		Renderer(device, swapchain, resources, sceneData, env, rtCache, pipelines,
			std::move(graphResult.graph), std::move(graphResult.desc), cullRaw, dgcRaw, dgcStaticRaw,
			// 批 14.5：FSR 生效判据 = 配置键 **且** velocity 真的进了图（FSR 的 motionVectors
			// 输入就是那张图；装饰失败时本键自动失效。取实际图形态的先例 = AO 施加点）。
			// B2-5：highlight 的装饰申报单（门开 = 登记成可撤回；门关 = 留作「从零插上它」的
			// 配方）。原值只在装饰前问得到，所以只能由编译期这条链带进来。
			fsrUsable, temporal, std::move(graphResult.highlightPlan))};
}

}  // namespace RGL
