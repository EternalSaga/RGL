module;
export module shader_registry;

import rlstd;
import rlvk;
import rllogger;

import vk_device;
import vk_resources;
import scene_data;  // SceneDataService（架构债务续账步骤 1：scene data 槽移出内核）
import environment;  // Environment（架构债务续账步骤 3：env 单例/sky sampler 移出内核）
import asset_table;  // AssetTable（F 拆出：不再经内核透传）
import shader;
import scene_config;
import runtime_config;  // RuntimeConfig（pickMeshletMeshVariant 入参：overlay 门 + instrument 三态位）
import material_types_generated;
import vkerror;

namespace RGL {

/**
 * 克隆管线用途（Phase 10 批 10.5 引入 picking、10.7 加 highlight）。
 *
 * 两者机制完全同构：把 model 族条目的 mesh SPV 配上一个**只写固定输出、不做完整
 * 光照**的替换 frag，录制期按 draw 重放一遍。故共用一套 remap 表（cloneRemap），
 * 用途做第一维索引——新增用途（如将来的 outline）只需加一个枚举值 + 一次
 * addCloneEntries 调用，不动既有表与下标语义。
 */
export enum class ClonePurpose : uint8_t {
	picking = 0,    // 批 10.5：写 objectIndex+1 进 r32ui object_id（回读取 id）
	highlight = 1,  // 批 10.7：菲涅尔边缘发光 alpha 混合叠加 scene_hdr（视觉高亮）
	kCount = 2,     // 用途计数（remap 表第一维长度；非有效用途）
};

/**
 * 簇路径叠加条目的 **mesh SPV 变体**（★1-a1ii-α debug 档 + ★0.5-b2 tint 档；
 * 债-129 S2 起由配置键 `meshlet.instrument` 三态直接选档，原 `meshlet.debug`/`meshlet.tint`
 * 两布尔的「同真 ⟹ tint 优先」让位规则随合并**结构性消失**——该非法态写不出来）。
 *
 * 门 = 选 SPV 变体不是运行时 flag（2026-10-02 slang fp 重排教训，同 IZB/PCSS/EVSSM 纪律）：
 *  · `base`        = `meshlet.mesh.spv`（正常叠加：共面 + eLess ⟹ 零写入）
 *  · `debugOffset` = `meshlet.mesh_MESHLET_DEBUG_OFFSET.spv`（clip.z 拉向相机 + 翻法线）
 *  · `tint`        = `meshlet.mesh_MESHLET_TINT.spv`（着色输入写常数、clip 不动；共面双臂
 *                    的阳性对照，`meshlet.coplanar` 的 equal/ne 两臂靠它做非空洞证明）
 */
export enum class MeshletMeshVariant : uint8_t {
	base = 0,        ///< 正常叠加档
	debugOffset = 1, ///< `meshlet.instrument = "offset"`：沿视向偏移 + 翻法线
	tint = 2,        ///< `meshlet.instrument = "tint"`：着色输入写常数、深度不动
};

/**
 * 叠加 mesh SPV 变体选档（★0.5-b2 的三档 base/debug/tint + **债-105** 的归属裁决 +
 * **债-129 S2** 的类型消灭：原「tint 与 debug 同真 = tint 优先」的让位分支已删——
 * 配置层是一枚三值 `MeshletInstrument`，同真形态**不可表示**，无需也让位）。
 *
 * 🔴 变体位**归叠加段所有**：`overlay = false` 时**一律 base**——这是**条目级**开关（变体经
 * `bakeMeshletEntries` 的 `variant` 烘进**条目**），而那批条目同时是 β-1 原位取代的宿主
 * （`makeOpaqueCall` 读 `asset.meshletPipelineIndex`）⟹ 叠加段关着也能改产品路的几何
 * （DEBUG_OFFSET 把 clip.z 拉向相机 = 取代段整体错位；TINT 把着色输入写死 = 产品路变平色块）。
 * 这不是「仪器没宿主」而是「仪器渗进产品」。孤立组合（仪器开了但被强制归 base）的出声
 * 自债-106 起在 **gate_lint**（meshlet.instrument 规则）——原就近 warn
 * `warnMeshletVariantInstruments` 的判据是纯配置组合，同一根因不许多处各喊一条
 * （影子真相源），随迁移删除。
 *
 * 三档返回语义（债-129 S2 合并前后逐字不变）：none→base、offset→debugOffset、tint→tint。
 * 住在 shader_registry（= `MeshletMeshVariant` 与 SPV 路径表的 owner）= 单测可达：
 * 原住 `module main:meshlets` 实现分区，rgl_tests 链不到 main（既有覆盖缺口，本刀收口）。
 * @param cfg 运行配置（读 overlay 门 + instrument 三态位）
 * @return 本跑该烘的 mesh SPV 变体
 */
export inline MeshletMeshVariant pickMeshletMeshVariant(const RuntimeConfig& cfg) {
	if (!cfg.meshletOverlay)
		return MeshletMeshVariant::base;
	if (cfg.instrumentIsTint())
		return MeshletMeshVariant::tint;
	if (cfg.instrumentIsOffset())
		return MeshletMeshVariant::debugOffset;
	return MeshletMeshVariant::base;  // instrument = none = 正常叠加档
}

/**
 * 管线表一条（Phase 4 批次 2：从 vkrenderer.cppm 迁入，Vulkan 层零 entt）：
 * shader 风格名 + 其 RenderPipeline（shader object + 烘焙的 descriptor heap binding mapping）
 * + 显式 push constant 元数据（L5 解决：argsSize/pushDataSet 替代旧 "set 0 被两种 struct
 * 隐式共享"）。argsSize = 该管线 push constant 字节数（Phase 7b+ DrawArgs 仅 objectIndex
 * = 4B，material_types_generated static_assert 与 shader 反射一致）；pushDataSet = 0（显式）。
 */
export struct PipelineEntry {
	std::string name;                             // 条目名（模型 = "{规格}.v{顶点槽}.t{base槽}.n{normal槽}.o{orm槽}"，skybox/cube/light 固定名）
	RGL::RenderPipeline pipeline;                 // shader object + 烘焙 binding mapping（见 PipelineBindings）
	uint32_t argsSize = sizeof(RGL::DrawArgs);    // push constant 字节数（DrawArgs 4B，仅 objectIndex）
	uint32_t pushDataSet = 0;                     // 显式 push constant set 号（vkCmdPushDataEXT 用）
	// Phase 10 批 10.4 热重载重建参数（buildReplacement 消费）：条目建成的两份输入存下
	// 即可原位重建——heap 槽运行期不漂移（binding mapping 恒可复用），SPV 由 hot reload
	// 侧提供新字节。默认值 = 未填（cube 等一次性条目不影响：热重载只处理填了 spec 的条目）。
	RGL::PipelineSpec spec;                       // SPV 规格路径（源 .slang 名 = basename 换扩展）
	RGL::PipelineBindings bindings;               // 烘焙 mapping 参数（heap 槽/形态位/extra）
	bool modelEntry = false;                      // addModelEntry 建的模式位（picking 克隆范围判据 = mesh 接口是 ModelVertexOutput）
};

/**
 * light 管线 spec（普通 / IZB 消费变体；findSpec 默认表 helper，批 11.3-1e 拆出控复杂度）。
 * IZB 变体 = 同一 light.frag.slang 的 -DIZB_CONSUME 编译产物（rgl_shader_defined）；
 * 门关选原版 = 原版 SPV 逐位不变（红线构造性保证）。
 * @param name spec 名
 * @return 命中 light/light_izb = spec 指针；否则 nullptr
 */
inline const PipelineSpec* findLightSpec(const char* name) {
	const bool izb = std::strcmp(name, "light_izb") == 0;
	if (izb) {
		static const PipelineSpec defaultLightIzb{
			"light_izb", "generated_shaders/light.mesh.spv",
			"generated_shaders/light.frag_IZB_CONSUME.spv"};
		return &defaultLightIzb;
	}
	if (std::strcmp(name, "light") != 0)
		return nullptr;
	static const PipelineSpec defaultLight{
		"light", "generated_shaders/light.mesh.spv", "generated_shaders/light.frag.spv"};
	return &defaultLight;
}

/**
 * 管线表（Phase 4：从 Renderer 迁出，零 entt——只认 slot/资产 id，不认场景实体）：
 *   条目全部按需动态加入：模型 = addModelEntry、内建 cube = addCubeEntry（架构债务 1：
 *   slot 经传入的资产记录取，Resources 不认识 cube）、天空盒 = addSkyEntry、
 *   延迟光 = addLightEntry；模型条目按 (规格, 顶点 slot, base 纹理 slot, normal slot,
 *   orm slot) 五元组组合去重——binding mapping 是 shader object 级状态（无法运行时换），
 *   同组合的映射恒等，故共享一条条目而非每 primitive 一条（Phase 4 验收：392 primitive
 *   → 12 条；Phase 8 ③a 扩 normal/orm 槽后条目数仍 ≤ 材质贴图组合数）。
 * 渲染 system 经资产记录的 pipelineIndex（数值）取条目，录制路径零字符串查找（E2.4）。
 * 条目下标写回资产记录经 bindToAsset / bindPipelineToAsset（启动期一次）。
 */

export class ShaderRegistry {
   public:
	/**
	 * 构造（main.cpp 初始化层构造顺序第 5 站：Resources→ShaderRegistry→Renderer）：
	 * specs 来自 configs/scene.lua（name → SPV 路径），按 name 索引；条目全按需建
	 *（模型 = addModelEntry、内建 cube = addCubeEntry、天空盒 = addSkyEntry、
	 * 延迟光 = addLightEntry）——画不画 = entity 有无，无内容专属的构造期恒建条目。
	 * @param device 设备层（RenderPipeline 创建）
	 * @param res 资源层（heap / sampler 槽 / env——binding mapping 烘焙）
	 * @param sceneData scene data 服务（幂等创建 buffer + 各子范围槽，架构债务续账步骤 1）
	 * @param env 环境服务（env 槽/sky sampler binding 4/5 烘焙，架构债务续账步骤 3）
	 * @param specs 管线规格（scene.lua styles 表，name → SPV 路径；缺条目时
	 *   findSpec 回落默认路径）
	 */
	ShaderRegistry(RGL::DeviceContext& device, RGL::Resources& res,
		RGL::SceneDataService& sceneData, RGL::Environment& env, RGL::AssetTable& assets,
		std::span<const RGL::PipelineSpec> specs)
		: device(device),
		  res(res),
		  sceneData(sceneData),
		  env(env),
		  assets(assets) {
		for (const auto& spec : specs)
			styles[spec.name] = spec;
	}

	/**
	 * 条目名 → 下标（启动期解析用：bindPipelineToAsset / 防御检查；每帧路径经资产
	 * 记录的 pipelineIndex 直接取，零字符串查找）。
	 * @param name 条目名（见 PipelineEntry.name 规则）
	 * @return 条目下标；nullopt = 无此条目
	 */
	[[nodiscard]] std::optional<uint32_t> pipelineIndex(std::string_view name) const {
		auto it = indexByName.find(std::string(name));
		return it == indexByName.end() ? std::nullopt : std::optional<uint32_t>(it->second);
	}
	/**
	 * 下标 → 条目（Renderer draw 循环 / llm_debug 截图路径取整条目：pushDataSet + pipeline）。
	 * @param index 条目下标
	 * @return 条目 const 引用（无界检查，调用方保证下标有效）
	 */
	[[nodiscard]] const PipelineEntry& entry(uint32_t index) const { return entries[index]; }
	/**
	 * 全管线表 const 视图（Phase 7b：DGC execution set 构造期逐条目一次性写 mesh/frag
	 * shader——容量 = 2×entries.size()）。
	 * @return entries 的 const span（下标 = 绘制契约 = 资产 pipelineIndex）
	 */
	[[nodiscard]] std::span<const PipelineEntry> entriesView() const { return entries; }

	/**
	 * 热重载重建 · 第一步（Phase 10 批 10.4；调用方 = shader_hot_reload）：用条目存的
	 * spec/bindings + 新 SPV 造一条**新的** RenderPipeline——在役条目不动，旧 pipeline
	 * 仍活着（execution set 此刻仍可安全指向旧句柄）。签名守卫（同签名判定）在调用方
	 * 完成，本函数不做校验。
	 * @param index 条目下标
	 * @param effect 新编译的 mesh + frag SPV
	 * @throws vk::SystemError 等（vkCreateShadersEXT 失败 = 新 SPV 不可加载，调用方
	 *         catch 后保留旧管线）
	 */
	[[nodiscard]] RGL::RenderPipeline buildReplacement(uint32_t index, const RGL::ShaderEffect& effect) {
		return RGL::RenderPipeline(device.getDevice(), effect, entries[index].bindings,
			entries[index].name);
	}

	/**
	 * 热重载重建 · 提交（第二步，时序契约）：**调用方前置 = waitForIdle + execution set
	 * 槽已指向新句柄**（旧句柄零 in-flight 引用 + 集合内零残留引用），本函数 move 覆盖
	 * 条目 = 旧 VkShaderEXT 在赋值点销毁。pipelineIndex 不变 = 资产表/cull 序列表零改动。
	 * @param index 条目下标
	 * @param fresh buildReplacement 产物
	 */
	void commitReplacement(uint32_t index, RGL::RenderPipeline&& fresh) {
		entries[index].pipeline = std::move(fresh);
	}

	/**
	 * 克隆条目（Phase 10 批 10.5 picking / 10.7 highlight 共用机制）：把当前表
	 * **全部 model 族条目**克隆出指定 frag 的变体——mesh SPV = 原条目 mesh、frag SPV
	 * = 传入的克隆 frag、bindings 原样（批 M：克隆 frag 不声明的 binding 由 RenderPipeline
	 * 按 mesh+frag 并集的 SPV 声明过滤在烘焙期滤掉，不再白烘；克隆 frag 若声明了 bindings
	 * 形态里没有的号，仍是「声明未 mapping」VVL 错误——两侧同向约束不因克隆放松）。非 model 条目（cube/天空盒/光 = mesh 输出接口非
	 * ModelVertexOutput）不克隆——重放路对其跳过。
	 * 装配时序契约 = main 于模型注册完成后调用（门 = diagnostics.picking；先于
	 * addSkyEntry/addLightEntry = 克隆段追加在 model 条目后）；既有条目下标语义零改动。
	 * @param purpose 克隆用途（进 remap 表键，见 cloneEntryFor）
	 * @param fragSpvPath 克隆 frag 的 SPV 相对路径（generated_shaders/ 下）
	 * @throws std::runtime_error 克隆 frag / model mesh SPV 读失败（装配期 fail-fast）
	 */
	void addCloneEntries(ClonePurpose purpose, const std::string& fragSpvPath);

	/**
	 * 克隆条目反查（执行器 picking / highlight 重放路逐 draw 调用）。
	 * @param purpose 克隆用途
	 * @param entry 原条目下标（DrawCall.pipeline）
	 * @return 克隆条目下标；nullopt = 该条目无此用途的克隆（cube/天空盒/克隆条目本身）
	 */
	[[nodiscard]] std::optional<uint32_t> cloneEntryFor(ClonePurpose purpose,
		uint32_t entry) const {
		if (static_cast<size_t>(purpose) >= cloneRemap.size())
			return std::nullopt;
		const auto& remap = cloneRemap[static_cast<size_t>(purpose)];
		if (entry >= remap.size() || remap[entry] == kNoClone)
			return std::nullopt;
		return remap[entry];
	}

	/**
	 * 模型管线条目（资产上传期）：用管线规格创建 RenderPipeline，binding mapping 显式烘焙
	 * 顶点 slot + 纹理槽（binding 0 = 顶点 buffer 槽、binding 1 = resource heap 的
	 * baseColor SAMPLED_IMAGE 描述符槽、binding 2 = sampler heap 槽——OpTypeSampler 映射，
	 * 共享一份 sampler 描述符，偏移 = samplerSlot * samplerHeap.getDescriptorSize()；
	 * scene data 各子范围载体（fillSceneRegionSlots）、4/5 = env 恒传入（批 M 按 SPV 声明
	 * 过滤烘焙）；binding 6/7 =
	 * normal/orm 贴图槽
	 *（Phase 8 ③a）、binding 8 = emissive 贴图槽（Phase ③，extra 追加，见 impl））。
	 * 首参 specName = 管线规格名（Phase 4 批次 3：风格→规格名映射在 main.cpp 经 scene.lua
	 * styles 表完成，本函数只认规格名）；按 (规格, 顶点 slot, base 纹理 slot, normal slot,
	 * orm slot, emissive slot) 六元组组合去重——同组合返回既有条目下标（不重复建 shader
	 * object）；新组合建条目并记入 indexByName / 去重表。返回条目下标，调用方经 bindToAsset
	 * 写回资产记录（renderListSystem 经资产表解析管线，无字符串约定）。失败（规格缺失 /
	 * SPV 加载失败等）抛出（同构造期路径）。
	 * @param specName 管线规格名（scene.lua styles / findSpec 回落词表）
	 * @param vertexSlot 顶点 buffer heap 槽（binding 0；模型顶点/索引共享同一 buffer）
	 * @param textureSlot baseColor 纹理 SAMPLED_IMAGE heap 槽（binding 1；白纹理 = 纹理首槽）
	 * @param normalSlot normal 贴图 SAMPLED_IMAGE heap 槽（binding 6；无贴图 = 中性法线纹理槽）
	 * @param ormSlot ORM 贴图 SAMPLED_IMAGE heap 槽（binding 7；无贴图 = 白纹理槽）
	 * @param emissiveSlot emissive 贴图 SAMPLED_IMAGE heap 槽（binding 8；无贴图 = 白纹理槽）
	 * @param samplerSlot 共享 sampler 的 sampler heap 槽（binding 2，ensureSharedSampler 产物）
	 * @param shadowMapSlot 阴影图 SAMPLED_IMAGE 槽（**binding 12**，Phase 11 批 11.2；无条件
	 *        传入，批 M 按 SPV 声明过滤烘焙——不 import shadow.slang 的风格不声明 = 不烘，
	 *        import 了 = 声明了 = 必有此条）
	 * @param shadowSamplerSlot 阴影专用 NEAREST sampler 槽（**binding 13**；不能用共享的 LINEAR，
	 *        深度格式无 filterable 能力）
	 * @param evssmMapSlot EVSSM 矩图 SAMPLED_IMAGE 槽（**binding 15**，批 11.6d-2；无条件
	 *        传入（同 binding 12 纪律），门关 = -DEVSSM_SOFT 变体不存在、binding 不声明 =
	 *        批 M 过滤不烘；binding 16 = 矩 sampler **复用本条目共享 LINEAR sampler**（矩图集
	 *        linear 过滤 = mip 高斯核；uv 恒 clamp 在块内 → wrap 模式无关））
	 * @return 条目下标（新组合 = entries 追加）
	 */
	[[nodiscard]] uint32_t addModelEntry(const std::string& specName, uint32_t vertexSlot,
		uint32_t textureSlot, uint32_t normalSlot, uint32_t ormSlot, uint32_t emissiveSlot,
		uint32_t samplerSlot, uint32_t shadowMapSlot, uint32_t shadowSamplerSlot,
		uint32_t evssmMapSlot);

	/**
	 * 簇化轴 ★1-a1ii-α：簇路径**影子叠加**管线条目（docs/mdi_plan.md §10.2）。
	 *
	 * 与 addModelEntry 的差别只有两处：
	 *  · **mesh SPV 换成 meshlet.mesh**（variant 选正常/DEBUG_OFFSET/TINT 变体——门 = 选
	 *    SPV 变体不是运行时 flag，2026-10-02 slang fp 重排教训）；
	 *    frag 与同规格模型条目**同一个 SPV**（输出接口同为 ModelVertexOutput）。
	 *  · 去重键 = (规格, base/normal/orm/emissive 纹理槽, variant)，**不含顶点槽**——
	 *    簇路径全局只有一份顶点池（§10.2「条目退化成按纹理集键控」）。
	 * 材质绑定与模型条目逐条同值（binding 1/2/6/7/8/12/13/15/16 + 3 + 4/5），追加簇 buffer 的
	 * **固定 binding**（17=顶点池 / 18=簇表 / 19=Tris / 20=Remap / 21=逐簇可见标志，
	 * §10.2「全局单例进固定 binding，不进 per-entry 描述符」；描述符内容由装配层
	 * wireMeshletBindings 写入这些 Runtime 段槽）。
	 * 🔴 binding 21（★1-a2-i）**无条件传入**：mesh SPV 只有一份、shader 侧无条件声明该 binding，
	 * 而 descriptor heap 下「声明了 binding 却无 mapping」是 VVL 错误。批 M 的按声明过滤
	 * **不豁免它**——真声明了 ⟹ 照烘（依据从「多烘一条空转无害」变成「声明了就必须烘」，
	 * 未声明的条目侧传了才被滤）。门关时那张表的内容是全 1（= 全可见），不是「没有这张表」。
	 * modelEntry 位 = false：picking/highlight 克隆与 DGC 候选都不吃本条目（叠加不参与
	 * 候选序对账）。
	 * @param specName 管线规格名（与同 primitive 的模型条目同名 = 同一 frag）
	 * @param textureSlot/normalSlot/ormSlot/emissiveSlot 材质纹理 heap 槽（同模型条目）
	 * @param samplerSlot 共享 sampler 槽（binding 2）
	 * @param shadowMapSlot/shadowSamplerSlot 阴影图 + NEAREST sampler（binding 12/13）
	 * @param evssmMapSlot EVSSM 矩图槽（binding 15）
	 * @param clusterSlots 5 个簇 buffer 描述符槽（vertexPool/clusters/tris/remap/clusterVisible
	 *        序 = binding 17-21 升序，🔴 binding 号 ≠ 槽号：这里传的是**槽号**）
	 * @param variant mesh SPV 变体（base/debugOffset/tint，见 MeshletMeshVariant）
	 * @return 条目下标（同键重复调用返回既有下标）
	 */
	[[nodiscard]] uint32_t addMeshletEntry(const std::string& specName, uint32_t textureSlot,
		uint32_t normalSlot, uint32_t ormSlot, uint32_t emissiveSlot, uint32_t samplerSlot,
		uint32_t shadowMapSlot, uint32_t shadowSamplerSlot, uint32_t evssmMapSlot,
		std::array<uint32_t, 5> clusterSlots, MeshletMeshVariant variant);

	/** 簇路径叠加条目写回资产记录（★1-a1ii-α，与 bindShadowToAsset 同节奏）。
	 * @param assetId mesh 资产记录 id
	 * @param entryIdx addMeshletEntry 返回的条目下标
	 */
	void bindMeshletToAsset(uint32_t assetId, uint32_t entryIdx);

	/**
	 * 天空盒管线条目（Phase 5.5 批次 4）：单条目（env 槽全局单例，无组合去重需求）。
	 * mapping = binding 0/1 占位槽号（skybox shader 不声明顶点/索引 = 批 M 过滤不烘，
	 * 槽号只是 PipelineBindings 形态的合法占位）+ scene data 子范围载体（skyClipToWorld 读
	 * invViewProj 走头区 UBO）+ binding 4/5 env（skybox.frag import env 声明——"声明未映射"才是错误，
	 * 声明了 ⟹ 过滤后照烘）。
	 * 前提 = env 已上传（main 调用点保证）；幂等（既有 "skybox" 条目直接返回下标）。
	 * @return 条目下标；规格缺失 / env 未上传抛 runtime_error
	 */
	[[nodiscard]] uint32_t addSkyEntry();

	/**
	 * Phase 7 D1：deferred light pass 管线条目（单条目，仿 addSkyEntry）。spec = "light"
	 *（全屏 mesh + 采样 frag）。binding：0/1 占位槽号（light mesh 不声明 = 批 M 过滤不烘）
	 * + 3 sceneData
	 *（readSceneLight 单灯）+ extra：4-8 → 5 个 G-buffer RT 的 SAMPLED_IMAGE 堆槽
	 *、8 → 共享 sampler、9/10 → env、**11 → emissive 第 5 通道**（Phase 8 剩余批次 B；
     * emissive 排尾——4-10 已被旧布局占用，勿按 kGBufferAttachments 序推断 binding）。
	 * 前提 = 5 G-buffer RT 已 ensureRtAttachment（其堆槽由调用点取入，跨 resize 复用不变）。幂等。
	 * @param albedoSlot G-buffer albedo RT 的 SAMPLED_IMAGE heap 槽（binding 4）
	 * @param normalSlot G-buffer normal RT 槽（binding 5）
	 * @param positionSlot G-buffer position RT 槽（binding 6）
	 * @param metalRoughSlot G-buffer metalRough RT 槽（binding 7）
	 * @param emissiveSlot G-buffer emissive RT 槽（binding 11，RGBA16F）
	 * @param samplerSlot 共享 sampler 的 sampler heap 槽（binding 8，sampler 映射）
	 * @param shadowMapSlot 阴影图 SAMPLED_IMAGE 槽（**binding 12**，与 forward 模型条目同号）
	 * @param shadowSamplerSlot 阴影专用 NEAREST sampler 槽（**binding 13**）
	 * @param evssmMapSlot EVSSM 矩图 SAMPLED_IMAGE 槽（**binding 15**，批 11.6d-2；
	 *        无条件传入（批 M 按声明过滤烘焙）+ binding 16 复用本条目 LINEAR sampler，
	 *        细则见 addModelEntry）
	 * @return 条目下标；规格缺失抛 runtime_error
	 */
	[[nodiscard]] uint32_t addLightEntry(uint32_t albedoSlot, uint32_t normalSlot,
		uint32_t positionSlot, uint32_t metalRoughSlot, uint32_t emissiveSlot,
		uint32_t samplerSlot, uint32_t shadowMapSlot, uint32_t shadowSamplerSlot,
		uint32_t evssmMapSlot);

	/**
	 * Phase 8 批次 ①：tonemap pass 管线条目（单条目，仿 addLightEntry）。spec = "tonemap"
	 *（mesh 复用 light.mesh.spv 全屏大三角 + ACES frag）。binding：0/1 占位 + 3 sceneData
	 *（@56 曝光 / Phase 11 批 11.4 起 @60 AO 强度）+ extra：4 → scene_hdr RT 的
	 * SAMPLED_IMAGE 堆槽、5 → 共享 sampler、**6 → ao RT 的 SAMPLED_IMAGE 堆槽**
	 * （r8_unorm 遮蔽量；施加强度 0 时逐位旁路，故该绑定恒在不影响基线）。
	 * 前提 = scene_hdr / ao 两个 RT 已 ensure + 领槽。幂等。
	 * @param sceneHdrSlot scene_hdr RT 的 SAMPLED_IMAGE heap 槽（binding 4）
	 * @param aoSlot ao RT（r8_unorm）的 SAMPLED_IMAGE heap 槽（binding 6）
	 * @param samplerSlot 共享 sampler 的 sampler heap 槽（binding 5）
	 * @return 条目下标；规格缺失抛 runtime_error
	 */
	[[nodiscard]] uint32_t addTonemapEntry(uint32_t sceneHdrSlot, uint32_t aoSlot,
		uint32_t samplerSlot);

	/**
	 * Phase 11 批 11.4：SSAO pass 管线条目（单条目，仿 addTonemapEntry）。spec = "ao"
	 *（mesh 复用 light.mesh.spv 全屏大三角 + depth-only SSAO frag）。binding：0/1 占位
	 * + 3 sceneData（@12 eyePos / @64 invViewProj 做深度↔世界坐标重建）+ extra：
	 * 4 → 深度图的 SAMPLED_IMAGE 堆槽（eDepth aspect）、5 → 共享 sampler（须 point
	 * 采样，由 AO 条目自行声明，与模型条目的 linear 共享槽不冲突——同一 sampler 描述符
	 * 对全屏 1:1 采样等价）。前提 = 深度图 usage 含 eSampled 且已领采样槽
	 * （Resources::ensureDepthSampleSlot）。幂等。
	 * @param depthSlot 深度图 SAMPLED_IMAGE heap 槽（binding 4）
	 * @param samplerSlot 共享 sampler 的 sampler heap 槽（binding 5）
	 * @return 条目下标；规格缺失抛 runtime_error
	 */
	[[nodiscard]] uint32_t addAoEntry(uint32_t depthSlot, uint32_t samplerSlot);

	/**
	 * Phase 14 批 14.4：逐像素速度场 pass 管线条目（单条目，仿 addAoEntry；门默认关
	 * **不被调用** = 条目表逐位不变）。spec = "velocity"（mesh 复用 light.mesh.spv 全屏
	 * 大三角 + 速度场 frag）。binding：0/1 占位 + 3 sceneData（@64 invViewProj +
	 * TAA 尾区 @723456 起两帧矩阵与内容位移）+ extra：4 → 深度图 SAMPLED_IMAGE 堆槽。
	 *
	 * **无 sampler 绑定**（与 ao 的唯一差别）：本 shader 用 `depthTex.Load(像素)` 直取，
	 * 不过滤 ⟹ 不需要也不该占 sampler 堆槽（`vkresources.cppm:350` 记的 2026-10-04
	 * sampler 槽相撞事故就是"自己算偏移"造成的；不占 = 不参与该风险）。
	 * 前提 = 深度图 usage 含 eSampled 且已领采样槽（Resources::ensureDepthSampleSlot，
	 * 与 ao 复用同一槽）。幂等。
	 * @param depthSlot 深度图 SAMPLED_IMAGE heap 槽（binding 4）
	 * @return 条目下标；规格缺失抛 runtime_error
	 */
	[[nodiscard]] uint32_t addVelocityEntry(uint32_t depthSlot);

	/**
	 * 批 12.6 TAA pass 管线条目（单条目，仿 addTonemapEntry；门默认关不被调用 =
	 * 条目表逐位不变）。spec = "taa"（mesh 复用 light.mesh.spv 全屏大三角 + 时域滤波
	 * frag）。binding：0/1 占位 + 3 sceneData（@723456 prevViewProj / @723520 motion /
	 * @723584 params / @64 invViewProj 深度重建）+ extra：4 → 本帧 scene_hdr RT 的
	 * SAMPLED_IMAGE 堆槽、5 → taa_hist RT 的 SAMPLED_IMAGE 堆槽、6 → 深度图堆槽
	 * （重投影的世界坐标来源）、7 → 共享 sampler（历史亚像素查找取 LINEAR；本帧色
	 * 在像素中心采样与点采等价）、8 → **point sampler**（D32Sfloat 深度不可过滤，
	 * 同 addAoEntry 的理由）、9 → **ao RT 的 SAMPLED_IMAGE 堆槽**（批 13.6：AO 施加点搬进
	 * taa pass = 时域滤波**之前**，与 Filament 同构；tonemap 据 scene data @723600.z 跳过）。
	 * 前提 = 两个 TAA RT 已 ensure + 领槽（ensureTaaSlots）+ 深度采样槽已领 + ao RT 已领槽。
	 * 幂等。
	 * @param colorSlot 本帧场景色（scene_hdr）SAMPLED_IMAGE heap 槽（binding 4）
	 * @param historySlot taa_hist RT 的 SAMPLED_IMAGE heap 槽（binding 5）
	 * @param depthSlot 深度图 SAMPLED_IMAGE heap 槽（binding 6，同 ao 条目复用同槽）
	 * @param samplerSlot 共享 sampler 的 sampler heap 槽（binding 7）
	 * @param pointSlot point sampler 的 sampler heap 槽（binding 8，ensurePointSampler）
	 * @param aoSlot ao RT 的 SAMPLED_IMAGE heap 槽（binding 9，同 tonemap 条目复用同槽）
	 * @return 条目下标；规格缺失抛 runtime_error
	 */
	[[nodiscard]] uint32_t addTaaEntry(uint32_t colorSlot, uint32_t historySlot,
		uint32_t depthSlot, uint32_t samplerSlot, uint32_t pointSlot, uint32_t aoSlot);
	/**
	 * 批 14.5 第三批：AO 施加专用 pass 管线条目（单条目，仿 addTonemapEntry——本 pass 的
	 * 绑定面就是 tonemap 的前半段，去掉 ACES/曝光）。spec = "ao_apply"，mesh 复用
	 * light.mesh.spv 全屏大三角。binding 0/1 顶点/索引占位、3 → scene data（@60 AO 强度
	 * + @723584 TAA 参数取 c_t 做 baseUv）+ extra：4 → 本帧场景色 RT 的 SAMPLED_IMAGE 堆槽、
	 * 5 → 共享 sampler（**必须 linear**：AO 走 baseUv 分数坐标，点采会随相位跳纹素，
	 * 见 shaders/ao_apply.frag.slang 纪律②）、6 → ao RT 的 SAMPLED_IMAGE 堆槽。
	 * 前提 = ao_applied RT 已 ensure + 领槽、ao RT 已领槽。幂等。
	 * @param colorSlot 本帧场景色（scene_hdr 或 taa_out）SAMPLED_IMAGE heap 槽（binding 4）
	 * @param aoSlot ao RT 的 SAMPLED_IMAGE heap 槽（binding 6，同 tonemap/taa 条目复用同槽）
	 * @param samplerSlot 共享 sampler 的 sampler heap 槽（binding 5）
	 * @return 条目下标；规格缺失抛 runtime_error
	 */
	[[nodiscard]] uint32_t addAoApplyEntry(uint32_t colorSlot, uint32_t aoSlot,
		uint32_t samplerSlot);

	/**
	 * 批 15.3-B：SSR **march** pass 管线条目（半分辨率，写 ssr_trace RT；门默认关不被调用 =
	 * 条目表逐位不变）。spec = "ssr_trace"（mesh 复用 light.mesh.spv 全屏大三角 + ssr_trace.frag）。
	 * binding：0/1 占位 + 3 sceneData（binding 31 的 `SsrParamsBlock.viewProj` = 本帧**抖动后** proj·view，ray march
	 * 把命中点投回屏幕 uv 用）+ extra：4 → 本帧场景色（**恒 scene_hdr**，理由见 addSsrTraceEntry
	 * 定义处注释）、5/6/7 → G-buffer normal/position/metalrough 采样槽（通道号镜像 light.frag）、
	 * 8 → 深度图采样槽（与 ao/velocity/taa 复用同一份 ensureDepthSampleSlot 的槽）、
	 * 9 → G-buffer albedo 采样槽（Schlick fresnel 的 F0 = lerp(0.04, albedo, metal)——金属
	 *   表面的 F0 就是 albedo，不绑它等于把所有表面当介质，靶的金属地面反射会整个不出来）、
	 * 10 → 共享 LINEAR/ClampToEdge sampler（**只有**命中点取反射色用它；直通路径与逐像素自身
	 *   量全走 `Load`，判据根据见 ssr_trace.frag.slang 绑定面那条 ⚠）、
	 * 11/12 → env cubemap 采样槽 + sky sampler（批 15.2 子步 C：命中处减 IBL specular 的镜像
	 *   重算源；env 不是图附件，走 extra = light pass 9/10 同法，图零改动；env 未上传 = 占位
	 *   0/0，shader 端 mipRange=0 恒不采样）。
	 * 前提 = ssr_trace RT 已 ensure + 领槽、G-buffer 5 槽已领、深度采样槽已领。幂等。
	 * @param colorSlot scene_hdr 的 SAMPLED_IMAGE heap 槽（binding 4）
	 * @param normalSlot G-buffer normal RT 槽（binding 5）
	 * @param positionSlot G-buffer position RT 槽（binding 6）
	 * @param metalRoughSlot G-buffer metalrough RT 槽（binding 7）
	 * @param depthSlot 深度图 SAMPLED_IMAGE heap 槽（binding 8，同 ao 条目复用同槽）
	 * @param albedoSlot G-buffer albedo RT 槽（binding 9）
	 * @param samplerSlot 共享 sampler 的 sampler heap 槽（binding 10）
	 * @return 条目下标；规格缺失抛 runtime_error
	 */
	[[nodiscard]] uint32_t addSsrTraceEntry(uint32_t colorSlot, uint32_t normalSlot,
		uint32_t positionSlot, uint32_t metalRoughSlot, uint32_t depthSlot,
		uint32_t albedoSlot, uint32_t samplerSlot);

	/**
	 * 批 15.3-B：SSR **模糊+合成** pass 管线条目（全分辨率，写 ssr_applied）。
	 * spec = "ssr"（mesh 复用 light.mesh.spv 全屏大三角 + ssr_apply.frag）。binding：0/1 占位
	 * + 3 sceneData（本 pass 不读 ssrViewProj——模糊/加法全在屏幕空间，抖动只影响采样位置）
	 * + extra：4 → 本帧场景色（恒 scene_hdr，与 trace 同槽）、5 → ssr_trace RT 采样槽
	 * （预乘契约：rgb = weight·(F·reflected − iblSpec)，a = weight）、6 → G-buffer metalrough
	 * 采样槽（模糊核宽度 = roughness 的函数）、7 → 共享 LINEAR/ClampToEdge sampler（金角
	 * 螺旋 5 taps 采样半分辨率 trace RT 用；sampler 条目烘字节偏移，红线同 trace）。
	 * 前提 = ssr_applied/ssr_trace RT 均已 ensure + 领槽。幂等。
	 * @param colorSlot scene_hdr 的 SAMPLED_IMAGE heap 槽（binding 4）
	 * @param traceSlot ssr_trace RT 的 SAMPLED_IMAGE heap 槽（binding 5）
	 * @param metalRoughSlot G-buffer metalrough RT 槽（binding 6）
	 * @param samplerSlot 共享 sampler 的 sampler heap 槽（binding 7）
	 * @return 条目下标；规格缺失抛 runtime_error
	 */
	[[nodiscard]] uint32_t addSsrApplyEntry(uint32_t colorSlot, uint32_t traceSlot,
		uint32_t metalRoughSlot, uint32_t samplerSlot);

	/**
	 * Phase 11 批 11.2：阴影 depth-only 管线条目。spec = "shadow"（shadow.mesh.spv + 空壳
	 * shadow.frag.spv）。
	 *
	 * **为何按 vertexSlot 去重而不按六元组**：模型条目要区分 baseColor/normal/orm/
	 * emissive 四个纹理槽，而阴影 mesh 只读 modelVertexBuffer（binding 0）与 scene data
	 * 子范围载体（对象 MVP / 阴影 VP / ObjectDraw）
	 * ——纹理槽外的一切都不消费，因此唯一变量就是 vertexSlot。
	 * 每个 16MB 拼接块一个槽 → bistro 约 8 个条目（而不是 1591 个）。
	 *
	 * **为何不经模型条目的组合去重表**：阴影管线不读纹理逻辑（只读位置），
	 * 不存在“同纹理槽但纹理槽不同”的需求；新建独立去重表会让“两套不同的去重键”
	 * 同时存在（模型空闲槽为什么不能用阴影空闲槽——两者绑的不是同一个纹理 buffer）。
	 *
	 * binding 映射：0 = 顶点 buffer 槽；1 = 索引槽（占位槽号，shadow mesh 不声明 = 批 M
	 * 过滤不烘）；3 = scene data（VP 区 + 对象矩阵表 + ObjectDraw 表）。**不纹理 / 不 sampler /
	 * 不 env**——shadow mesh/frag 两个文件都不声明它们（批 M 前是「烘了空转」，现在传了也
	 * 不烘；若将来 shader 声明了其中之一，条目侧必须补传——「声明了未 mapping」仍是 VVL 错误）。
	 * @param vertexSlot 顶点 buffer heap 槽（binding 0）
	 * @return 条目下标（同槽重复调用返回既有下标）
	 */
	[[nodiscard]] uint32_t addShadowEntry(uint32_t vertexSlot);

	/**
	 * 批 11.3-1d 后半：IZB mask pass 管线条目（spec = "izb_mask"：光空间 mesh +
	 * 走链 frag）。形态 = addShadowEntry 镜像（vertexSlot 去重 + scene data），
	 * 追加 extra bindings：5/6/7 → IZB head/nodes/mask 堆槽（Resources::
	 * ensureIzbSlots 稳定槽号；描述符内容由 IzbShadow 构造时写）。
	 * argsSize = 12（ShadowDrawArgs 8B + gridDim 4B，重放点拼接）。
	 * @param vertexSlot 顶点 buffer heap 槽（binding 0）
	 * @return 管线条目下标
	 */
	[[nodiscard]] uint32_t addIzbMaskEntry(uint32_t vertexSlot);

	/**
	 * mask 条目查询（重放点：shadow draw 的条目 → vertexSlot → mask 条目）。
	 * @param vertexSlot 顶点槽（entry.bindings.slotVertex）
	 * @return 条目下标；nullopt = 该顶点槽未建 mask 条目（门关或资产缺）
	 */
	[[nodiscard]] std::optional<uint32_t> izbMaskEntryFor(uint32_t vertexSlot) const;

	/** 给模型资产回填阴影管线条目（启动期一次，与 bindToAsset 同节奏）。
	 * @param assetId 资产记录 id
	 * @param entryIdx addShadowEntry 返回的条目下标
	 */
	void bindShadowToAsset(uint32_t assetId, uint32_t entryIdx);

	/**
	 * 簇化轴 ★18-B1：阴影段**簇路径** depth-only 管线条目（spec = "shadow_meshlet"：
	 * shadow_meshlet.mesh.spv + **复用** shadow.frag.spv）。docs/mdi_plan.md §18.5 前置清单。
	 *
	 * ## 为什么全场景 **1 条目**而 addShadowEntry 是 ~8 条目
	 *
	 * `addShadowEntry` 的唯一去重变量是 `vertexSlot`（阴影 mesh 只读 modelVertexBuffer +
	 * sceneData，纹理槽外一切不消费 ⟹ 每个 16MB 拼接块一个条目，bistro ≈8）。**这个去重键在
	 * 簇路径不存在了**：簇路径不读 per-资产拼接顶点 buffer，读的是全局单例的簇四段 buffer
	 * （§10.2「全局单例进固定 binding，不进 per-entry 描述符」）⟹ 条目退化成常量一份
	 * （同 addSkyEntry / addLightEntry 的"全局单例 ⟹ 单条目"形态）。
	 *
	 * ## binding 映射
	 *
	 *  · 0/1 = 合法占位槽号：本管线的 mesh/frag **两个 stage 都不声明 binding 0/1**（mesh 不
	 *    import model，frag 是空壳）⟹ 批 M 的按声明过滤在烘焙期把这两条**滤掉**（批 M 前是
	 *    「烘了空转无害」，现在根本不烘）。🔴 方向别记反：「声明了未 mapping」才是 VVL 错误
	 *    ——过滤只敢滤「没声明的」，声明了的（如 3/17-20）照烘（addSkyEntry/addLightEntry 的
	 *    0/1 占位既证方向）。
	 *  · 3 = scene data（`hasSceneData = true`）：readObjectClusterRange / readObjectMatrix /
	 *    readShadowViewProj 三张表都在这一份 buffer 里。
	 *  · 17/18/19/20 = 簇四段 buffer（顶点池/簇表/Tris/Remap），内容 = 装配层
	 *    wireMeshletBindings 写入的 Runtime 段槽（与主 pass 的 meshlet 条目**同一批槽号**）。
	 *  · 🔴 **不传 binding 21**（`clusterVisible`）：本管线不声明它（阴影段本批不做逐簇剔除）
	 *    ⟹ 批 M 后即使条目侧传了也不会烘（没声明）；而「声明了未 mapping」仍是 VVL 错误
	 *    ⟹ 将来 shader 若声明它又漏传就是 VVL 错误——两侧同向（都不带 21）是当前形态的约束。
	 *  · 不传 env、不传 6/7/8/12/13/15/16：两个 shader 文件都不声明它们（`textured = false`；
	 *    批 M 后传了也不烘，但「不传」让槽号来源不参与账目）。
	 *
	 * 幂等：同名条目（"shadow_meshlet"）命中直接返回既有下标（同 addSkyEntry 的写法）——
	 * 装配层逐 primitive 调用它，去重住在本函数。
	 * @param clusterSlots 4 个簇 buffer 描述符**堆槽号**（vertexPool/clusters/tris/remap 序 =
	 *        binding 17-20 升序；🔴 传的是槽号不是 binding 号，同 addMeshletEntry）
	 * @return 条目下标（重复调用返回同一个下标）；规格缺失抛 runtime_error
	 */
	[[nodiscard]] uint32_t addShadowMeshletEntry(std::array<uint32_t, 4> clusterSlots);

	/** 簇路径阴影条目写回资产记录（★18-B1，与 bindShadowToAsset 同节奏）。
	 * @param assetId mesh 资产记录 id
	 * @param entryIdx addShadowMeshletEntry 返回的条目下标
	 */
	void bindShadowMeshletToAsset(uint32_t assetId, uint32_t entryIdx);

	/**
	 * ★M-1b（docs/mdi_plan.md §19.2 D2 / §19.5）：阴影段**真 MDI** 管线条目
	 * （spec = "shadow_meshlet_mdi"：shadow_meshlet.mesh_MDI_SHADOW.spv + **复用**
	 * shadow.frag.spv）。与 addShadowMeshletEntry 的全部差别 = mesh SPV 换变体 +
	 * 多烘一枚 binding 34（行表），12B push / argsSize / scene data 面**一字节不动**
	 * （§19.5 表末「push 尺寸是条目属性」那行：word0 语义切行基址，尺寸不变）。
	 *
	 * 🔴 **声明与 mapping 同向**（§19.5）：MDI 变体 SPV 声明 binding 34 ⟹ 本条目**必须**烘
	 * 22（缺 mapping = VVL 错误）；旧 "shadow_meshlet" 条目不声明也不烘 22（两侧同向）。
	 * 🔴 **纯追加**：本条目只能排在 addShadowMeshletEntry（474）之后（§19.7 前置 1：插到
	 * 之前 = 旧资产字段指向的条目变掉，「门关用的还是今天那份 SPV」静默破）。调用侧的时序
	 * 契约见 main.cppm `wireMeshletPostRegistration`（烘焙在行表载体**建成之后**——argSlot
	 * 此刻才是真值，别拿启动期算不出来的东西凑槽号）。
	 * 幂等：同名条目（"shadow_meshlet_mdi"）命中直接返回既有下标（旧条目同法）。
	 * @param clusterSlots 4 个簇 buffer 堆槽号（binding 17-20，与旧条目同一批，口径同上）
	 * @param argsSlot 行表 heap 槽号（binding 34 = MeshletShadowArgsService::argSlot()；定案号 22
	 * 被 sceneHeader 占用，见 shadow_meshlet.mesh.slang 的 ⚠ 注）
	 * @return 条目下标（重复调用返回同一个下标）；规格缺失抛 runtime_error
	 */
	[[nodiscard]] uint32_t addShadowMeshletMdiEntry(std::array<uint32_t, 4> clusterSlots,
		uint32_t argsSlot);

	/** 真 MDI 阴影条目写回资产记录（★M-1b，与 bindShadowMeshletToAsset 同节奏）。
	 * @param assetId mesh 资产记录 id
	 * @param entryIdx addShadowMeshletMdiEntry 返回的条目下标
	 */
	void bindShadowMeshletMdiToAsset(uint32_t assetId, uint32_t entryIdx);

	/**
	 * 内建几何管线条目（架构债务 1 方案 B）：spec = "cube"，binding mapping = 传入资产记录的
	 * 顶点/索引 heap 槽（无纹理/无 scene data = cube 形态，binding 0 = 顶点槽、
	 * binding 1 落索引槽——非 textured 形态）。
	 * 由 main 在内建几何上传后按 entity 存在与否调用（幂等：既有 "cube" 条目直接返回下标）。
	 * @param meshAssetId 内建 mesh 资产 id（uploadBuiltinCube 产物，须为带索引资产）
	 * @return 条目下标；规格缺失 / 资产非带索引抛 runtime_error
	 */
	[[nodiscard]] uint32_t addCubeEntry(uint32_t meshAssetId);

	/**
	 * 条目下标写回资产记录（启动期一次，调用点 = main 在 addModelEntry / addCubeEntry 后）。
	 * @param assetId mesh 资产 id
	 * @param entryIndex 管线条目下标
	 */
	void bindToAsset(uint32_t assetId, uint32_t entryIndex) {
		assets.setPipelineIndex(assetId, entryIndex);
	}
	/**
	 * 条目名 → 下标写回资产记录（启动期一次；条目缺失 = 配置/构建不一致，直接 throw）。
	 * @param name 条目名（见 PipelineEntry.name 规则）
	 * @param assetId mesh 资产 id
	 */
	void bindPipelineToAsset(const char* name, uint32_t assetId);

   private:
	/**
	 * 把 scene data 各**子范围**载体槽号一次填进 PipelineBindings（UBO 子范围 = 迁移批
	 * 1/2/3/S-1/S-2/S-3/S-4/S-5，逐对象表 SSBO 子范围 = 批 T1）。🔴 binding 号与字段的一一对应
	 * 以**本函数体为唯一真相源**，本文不抄表（抄一次漂一次，S-1 起每批都在加行）。	 *
	 * 15 处条目装配点（:impl 8 + :fullscreen 7）同一段赋值，抽成一处：这些 binding **恒带上、
	 * 不在 C++ 侧登记谁用**，SPV 没声明的那条由批 M 的声明过滤自动裁掉（§9.3/§9.7）——
	 * 「谁用哪张表」的唯一判据是 SPV 声明，C++ 侧再抄一份就是双源。
	 * 🔴 批 N+1 起 scene data **没有全区 binding 3 了**（`ByteAddressBuffer sceneData` 声明已删，
	 * 护栏 = check_shader_deps.py R6）⟹ 本函数就是 scene data 下发面的**全部**。
	 * @param b 待填的 mapping 烘焙参数（只写本函数体内出现的那些字段，其余不动）
	 */
	void fillSceneRegionSlots(RGL::PipelineBindings& b);

	/**
	 * 把一条 pipeline 规格对应的 mesh/fragment SPV 载入 ShaderEffect（SPV 路径来自 SceneConfig）。
	 * @param spec 管线规格（meshSpv / fragSpv 相对路径）
	 * @return 填好两阶段 SPV 的 ShaderEffect；任一路径加载失败抛 runtime_error（Error::describe）
	 */
	static RGL::ShaderEffect loadShaderEffect(const RGL::PipelineSpec& spec);

	/**
	 * 按名查找 pipeline 规格 + 软阴影变体替换（批 11.5/11.6，唯一入口 = 本函数，
	 * findSpecBase 为回落体）：门 shadowEvssm 优先、其次 shadowPcss，且 fragSpv 属阴影
	 * 采样 frag（前缀 ∈ model_pbr_ibl / model_vegetation / model_light / light.frag*——
	 * 最后一个也覆盖 IZB 变体，拼接结果 light.frag_IZB_CONSUME_EVSSM_SOFT.spv 与
	 * rgl_shader_defined2 命名同源）时换对应 _EVSSM_SOFT / _PCSS_SOFT 变体路径。
	 * 两门全关原样透传指针 = 原版 SPV（红线构造性保证，同 1e IZB 门 = 选 spec 变体）。
	 * @param name 规格名
	 * @return 规格 const 指针（styles 表命中或默认回落，可能已换变体）；未知名 = nullptr
	 */
	const RGL::PipelineSpec* findSpec(const char* name) const {
		return softShadowVariantSpec(findSpecBase(name));
	}

	/**
	 * 软阴影变体 spec 查表/建表（批 11.5/11.6；实现在 impl unit——需要 diagnostics()）。
	 * @param base findSpecBase 结果（可空）
	 * @return 两门全关/非阴影消费 frag → base；否则 fragSpv 插 _EVSSM_SOFT（门优先）
	 *         或 _PCSS_SOFT 的缓存 spec
	 */
	const RGL::PipelineSpec* softShadowVariantSpec(const RGL::PipelineSpec* base) const;

	/** 回落查表体（批 11.5 起为 findSpec 的内部底座：scene.lua styles 命中或默认回落词表
	 *  cube / model / model_pbr / model_phong / model_pbr_ibl / skybox / deferred_geometry×3 /
	 *  light×2 / 尾部 kTailSpecs（tonemap/shadow/shadow_meshlet/izb_mask/ao/taa/velocity/
	 *  ao_apply/ssr_trace/ssr）；不含 PCSS 变体替换）。 */
	const RGL::PipelineSpec* findSpecBase(const char* name) const {
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
		if (std::strcmp(name, "model_phong") == 0) {
			static const RGL::PipelineSpec defaultModelPhong{
				"model_phong", "generated_shaders/model.mesh.spv",
				"generated_shaders/model_phong.frag.spv"};
			return &defaultModelPhong;
		}
		if (std::strcmp(name, "model_pbr_ibl") == 0) {
			static const RGL::PipelineSpec defaultModelPbrIbl{
				"model_pbr_ibl", "generated_shaders/model.mesh.spv",
				"generated_shaders/model_pbr_ibl.frag.spv"};
			return &defaultModelPbrIbl;
		}
		if (std::strcmp(name, "skybox") == 0) {
			static const RGL::PipelineSpec defaultSkybox{
				"skybox", "generated_shaders/skybox.mesh.spv",
				"generated_shaders/skybox.frag.spv"};
			return &defaultSkybox;
		}
		if (std::strcmp(name, "deferred_geometry") == 0) {
			static const RGL::PipelineSpec defaultDeferredGeom{
				"deferred_geometry", "generated_shaders/model.mesh.spv",
				"generated_shaders/deferred_geometry.frag.spv"};
			return &defaultDeferredGeom;
		}
		if (std::strcmp(name, "deferred_geometry_vegetation") == 0) {
			static const RGL::PipelineSpec defaultDeferredGeomVeg{
				"deferred_geometry_vegetation", "generated_shaders/model.mesh.spv",
				"generated_shaders/deferred_geometry_vegetation.frag.spv"};
			return &defaultDeferredGeomVeg;
		}
		if (std::strcmp(name, "deferred_geometry_light") == 0) {
			static const RGL::PipelineSpec defaultDeferredGeomLight{
				"deferred_geometry_light", "generated_shaders/model.mesh.spv",
				"generated_shaders/deferred_geometry_light.frag.spv"};
			return &defaultDeferredGeomLight;
		}
		if (const RGL::PipelineSpec* light = RGL::findLightSpec(name))
			return light;
		// 尾部词表（tonemap/shadow/izb_mask/ao/taa）：批 12.6 起表驱动查表——旧 if 链
		// 每加一词控制复杂度 +1（taa 加进来正撞 16>15 红线），表长不受此税。
		// ★M-1b 追加 shadow_meshlet_mdi：表长 10 → **11**（改表忘改长度 = 编译期直接红，
		// brace-init 元素数与 std::array<N> 不符，编译器替我们盯着）。
		static const std::array<RGL::PipelineSpec, 11> kTailSpecs{{
			{"tonemap", "generated_shaders/light.mesh.spv",
				"generated_shaders/tonemap.frag.spv"},  // mesh 复用全屏大三角（Phase 8 ①）
			{"shadow", "generated_shaders/shadow.mesh.spv", "generated_shaders/shadow.frag.spv"},
			// 簇化轴 ★18-B1：阴影段簇路径（§18.5）。mesh 换 shadow_meshlet.mesh（读簇 buffer
			// @17-20 + 12B push），frag **复用** shadow.frag.spv（输出接口同为 ShadowVertexOutput
			// = 只一个 SV_Position，两 mesh 的 frag 侧完全等价）。
			{"shadow_meshlet", "generated_shaders/shadow_meshlet.mesh.spv",
				"generated_shaders/shadow.frag.spv"},
			// ★M-1b（docs/mdi_plan.md §19.2 D2）：真 MDI 变体 = frag 复用 shadow.frag.spv、
			// mesh 换 shadow_meshlet.mesh_MDI_SHADOW.spv（push word0 = rowBase + binding 34
			// 读 24B 行表）。🔴 **表尾纯追加**（同上方 shadow_meshlet 当年的加法）：旧
			// "shadow_meshlet" 规格一字未动 = 门关/出厂选的还是今天那份 SPV、那条条目。
			// ★M-3T-a（docs/mdi_plan.md §19.16）：第四元素 = taskSpv。本条目的 mesh 变体**不再
			// 声明** `SV_DrawIndex`（§19.9 实测它在 mesh 入口毒化 gfx1151 后续 mesh draw），行号与
			// 派发序号由这枚 task 经 payload 转交 ⟹ 挂 task 是这条条目的**正确性前提**，不是可选项。
			{"shadow_meshlet_mdi", "generated_shaders/shadow_meshlet.mesh_MDI_SHADOW.spv",
				"generated_shaders/shadow.frag.spv",
				"generated_shaders/shadow_meshlet.task.spv"},
			{"izb_mask", "generated_shaders/izb_mask.mesh.spv",
				"generated_shaders/izb_mask.frag.spv"},
			{"ao", "generated_shaders/light.mesh.spv",
				"generated_shaders/ao.frag.spv"},  // mesh 复用全屏大三角（同 tonemap/light）
			{"taa", "generated_shaders/light.mesh.spv",
				"generated_shaders/taa.frag.spv"},  // mesh 复用全屏大三角（同 tonemap/ao）
			{"velocity", "generated_shaders/light.mesh.spv",
				"generated_shaders/velocity.frag.spv"},  // 批 14.4：速度场（同 ao/taa 复用大三角）
			{"ao_apply", "generated_shaders/light.mesh.spv",
				"generated_shaders/ao_apply.frag.spv"},  // 批 14.5：AO 施加（同复用大三角）
			{"ssr_trace", "generated_shaders/light.mesh.spv",
				"generated_shaders/ssr_trace.frag.spv"},  // 批 15.3-B：SSR march（同复用大三角）
			{"ssr", "generated_shaders/light.mesh.spv",
				"generated_shaders/ssr_apply.frag.spv"},  // 批 15.3-B：SSR 模糊+合成（同复用大三角）
		}};
		for (const auto& s : kTailSpecs)
			if (std::strcmp(s.name.c_str(), name) == 0)
				return &s;
		return nullptr;
	}

	RGL::DeviceContext& device;  // 设备层引用（RenderPipeline 创建）
	RGL::Resources& res;         // 资源层引用（heap / sampler 槽 / env 烘焙）
	RGL::SceneDataService& sceneData;  // scene data 服务引用（各子范围槽烘焙；架构债务续账步骤 1）
	RGL::Environment& env;  // 环境服务引用（env 槽 + sky sampler binding 4/5 烘焙；架构债务续账步骤 3）
	RGL::AssetTable& assets;  // 资产表引用（bindPipelineToAsset 回写；F 拆出）
	std::map<std::string, RGL::PipelineSpec> styles;          // 风格名 → SPV 规格（scene.lua）
	std::map<std::string, uint32_t> indexByName;              // 条目名 → 下标（启动期 name 解析）
	std::map<std::tuple<std::string, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, uint32_t> modelEntryBySlot;  // 模型条目去重表：(规格名, 顶点 slot, base 纹理 slot, normal slot, orm slot, emissive slot) → 下标
	// 簇路径叠加条目去重表（★1-a1ii-α）：(规格名, base/normal/orm/emissive 纹理 slot,
	// variant) → 下标。**无顶点 slot**（全局一份顶点池，§10.2）；variant 进键 =
	// 三个 SPV 变体（base/debug/tint）结构上不可能共用条目。
	std::map<std::tuple<std::string, uint32_t, uint32_t, uint32_t, uint32_t, MeshletMeshVariant>,
		uint32_t>
		meshletEntryByKey;
	// 阴影条目去重表：唯一变量 = vertexSlot（见 addShadowEntry 注释）
	std::map<uint32_t, uint32_t> shadowEntryBySlot;
	// 批 11.3-1d：IZB mask 条目表（vertexSlot → 条目下标；izbMaskEntryFor 消费）
	std::map<uint32_t, uint32_t> izbMaskEntryBySlot;
	std::vector<PipelineEntry> entries;                       // 管线表（下标 = 绘制契约）
	// 克隆反查表（批 10.5 picking / 10.7 highlight，addCloneEntries 填）：第一维 =
	// ClonePurpose，第二维 = 原条目下标 → 克隆条目下标。kNoClone = 该条目无此用途克隆；
	// 调用时刻之后新增条目不在表内（天然 nullopt）
	static constexpr uint32_t kNoClone = 0xFFFFFFFFu;
	std::vector<std::vector<uint32_t>> cloneRemap;
};

}  // namespace RGL
