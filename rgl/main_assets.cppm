module;
#include <entt/entt.hpp>
#include <fmt/format.h>
#include <filesystem>  // 模型旁的簇产物 / environment.hdr 存在性探测（GMF 例外清单成员）

// 入口模块 main 的分区实现单元（module main:assets;）——**资产装配**这一族：把 configs/scene.lua
// 描述的东西搬进 GPU，并烘出「画它 / 打光给它」所需的管线条目。
//
// 【为什么从 main:scene 拆出来】main:scene 已顶在「单文件代码行 ≤ 500」上限（实测 500/500）。
// 上限不是要绕的，是要找缝的。缝在这里：main:scene 剩下的函数全在回答"**渲染后端怎么拼起来**"
// （读 CompiledGraphResult 的装饰标志 → 领后处理 RT 槽 → 构造候选表 / GpuCull / DGC / Renderer），
// 而搬过来的这一族回答"**scene.lua 里的东西怎么进 GPU**"（解析模型 → 上传顶点/纹理 → 传 env →
// 按材质与光源烘管线条目）。
//
// 职责边界（与 main:scene 对读）：
//   本单元 = 资产上传（A0 内建 cube / A1 解析+顶点 / A2 纹理 / env cubemap / 簇产物加载与上传）
//          + **逐资产与逐光源**的管线条目烘焙（C1 模型条目 + 阴影/IZB 条目、picking/highlight
//            克隆条目、deferred light 条目）+ 装配期对账（BLEND 图元 vs 图与环境前提）；
//   main:scene = 场景实体注册（C2/C2b）+ **按图形态**装配后处理链条目（assembleHdrAoChain 的
//            ao / tonemap / taa / ssr_trace / ssr_apply / velocity / ao_apply）+ 候选表 /
//            GpuCull / DGC / Renderer（C3）。
//   ⟹ 判据 = 烘出来的条目是"**画东西 / 打光**"的（本单元）还是"**后处理**"的（main:scene）。
//
// ⚠ 一条**如实记下的不完美**：gBufferSlotsAndLightEntry 跟着搬过来了，而它吃两枚图形态标志
//   （deferredMode / ssrDecorated）当**闸门**。上面那条"资产驱动 vs 图形态驱动"的缝对它只成立
//   一半。搬它的理由是另外两条更硬的：① 它烘的是 **light 光照条目**，按上面的判据属本单元；
//   ② 它与 bakePipelineEntries 共用 evssmMomentsSlotOf（幂等发号）——留在 main:scene 会把这对
//   helper 劈成跨分区调用，把"单一来源"换成"多一条接口声明"。两害相权取其轻。
//
// 时序契约（跨调用的先后由 main_impl 的 `main()` 调用点承载，本单元只保证单次调用内部不乱）：
//   uploadModelAssets → uploadEnvironment → validateBlendSupport → bakePipelineEntries
//   → addDiagnosticCloneEntries →（main:meshlets 的叠加装配）→ registerSceneEntities。
//   env 上传必须在 C1 烘焙**之前**（addModelEntry 查 getEnvTexture 烘 binding 4/5）；BLEND 对账
//   必须在 C1 **之前**（env 缺失的真实崩溃点就是 C1 的 vkCreateShadersEXT，实测放 C3 跑不到）。
//
// 可见性同姊妹分区：声明在 main.cppm（export），定义在本单元（不带 export）。启动期 expected
// 解包 helper `orThrow` 现由本单元与 main:scene 共用，已提到 main.cppm 保持单一来源。
module main:assets;

import main;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import vk_device;
import vk_resources;
import vk_swapchain;
import sampler_library;  // SamplerLibrary（架构债务续账步骤 4a/4b）
import environment;  // Environment（架构债务续账步骤 3）
import render_target_cache;  // RenderTargetCache（架构债务续账步骤 2）
import gltf_loader;
import aa_target;  // aaTargetModel()/kModelKey（细线抗锯齿靶内存合成，批 12.6 T4）
import model_importer;
import texture_pool;
import scene_config;
import scene_context;
import shader_registry;
import builtin_geometry;
import asset_table;
import meshlet_asset;  // ★0-b2：loadMeshletAsset（产物读取侧，纯 CPU 无设备）
import meshlet_geometry;  // ★1-a0：uploadMeshletGeometry（簇产物 4 段的 GPU 侧持有者，rlgfx）
import render_graph;
import diagnostics;
import shadow_config;  // shadowConfig() + shadowFormPlan()（IZB mask 条目门，批 N6b）
import rllogger;

namespace RGL {

namespace {

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

/** EVSSM 矩图槽（批 11.6d-2）：发号 + 失败抛出收进 helper（assembleBackEnd 复杂度
 *  红线先例：调用点零分支）。幂等（bakePipelineEntries 已发过号则直接复用同号）。 */
uint32_t evssmMomentsSlotOf(Resources& resources) {
	return orThrow(resources.ensureEvssmSampleSlot(), "EVSSM 矩图槽发号失败");
}

}  // namespace

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
 *  docs/archive/图变体收敛_plan.md §5。 */
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
}  // namespace RGL
