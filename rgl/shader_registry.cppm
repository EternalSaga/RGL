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
	 * @param sceneData scene data 服务（binding 3 槽 = ensure 幂等返回，架构债务续账步骤 1）
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
		return RGL::RenderPipeline(device.getDevice(), effect, entries[index].bindings);
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
	 * = 传入的克隆 frag、bindings 原样（克隆 frag 不声明的 descriptor 映射空转无害，
	 * skybox 条目同先例）。非 model 条目（cube/天空盒/光 = mesh 输出接口非
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
	 * binding 3 = scene data、4/5 = env 恒烘焙；binding 6/7 = normal/orm 贴图槽
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
	 * @return 条目下标（新组合 = entries 追加）
	 */
	[[nodiscard]] uint32_t addModelEntry(const std::string& specName, uint32_t vertexSlot,
		uint32_t textureSlot, uint32_t normalSlot, uint32_t ormSlot, uint32_t emissiveSlot,
		uint32_t samplerSlot);

	/**
	 * 天空盒管线条目（Phase 5.5 批次 4）：单条目（env 槽全局单例，无组合去重需求）。
	 * mapping = binding 0/1 占位（skybox shader 不声明顶点/索引 = 空转，Phase 3/5 既证
	 * "映射未消费无害"方向）+ binding 3 scene data（skyClipToWorld 读 invViewProj）+
	 * binding 4/5 env（skybox.frag import env 声明——"声明未映射"才是错误，本条目恒烘焙）。
	 * 前提 = env 已上传（main 调用点保证）；幂等（既有 "skybox" 条目直接返回下标）。
	 * @return 条目下标；规格缺失 / env 未上传抛 runtime_error
	 */
	[[nodiscard]] uint32_t addSkyEntry();

	/**
	 * Phase 7 D1：deferred light pass 管线条目（单条目，仿 addSkyEntry）。spec = "light"
	 *（全屏 mesh + 采样 frag）。binding：0/1 占位（light mesh 不声明 = 空转）+ 3 sceneData
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
	 * @return 条目下标；规格缺失抛 runtime_error
	 */
	[[nodiscard]] uint32_t addLightEntry(uint32_t albedoSlot, uint32_t normalSlot,
		uint32_t positionSlot, uint32_t metalRoughSlot, uint32_t emissiveSlot,
		uint32_t samplerSlot);

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
	 * 把一条 pipeline 规格对应的 mesh/fragment SPV 载入 ShaderEffect（SPV 路径来自 SceneConfig）。
	 * @param spec 管线规格（meshSpv / fragSpv 相对路径）
	 * @return 填好两阶段 SPV 的 ShaderEffect；任一路径加载失败抛 runtime_error（Error::describe）
	 */
	static RGL::ShaderEffect loadShaderEffect(const RGL::PipelineSpec& spec);

	/**
	 * 按名查找 pipeline 规格（scene.lua 缺条目时回落默认 SPV 路径，同原 Renderer 行为）。
	 * 回落词表：cube / model / model_pbr / model_phong / model_pbr_ibl / skybox /
	 * deferred_geometry / light（各指向 generated_shaders/ 下对应 mesh/frag SPV）。
	 * @param name 规格名
	 * @return 规格 const 指针（styles 表命中或默认回落）；未知名 = nullptr
	 */
	const RGL::PipelineSpec* findSpec(const char* name) const {
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
		if (std::strcmp(name, "light") == 0) {
			static const RGL::PipelineSpec defaultLight{
				"light", "generated_shaders/light.mesh.spv", "generated_shaders/light.frag.spv"};
			return &defaultLight;
		}
		if (std::strcmp(name, "tonemap") == 0) {
			static const RGL::PipelineSpec defaultTonemap{
				"tonemap", "generated_shaders/light.mesh.spv",
				"generated_shaders/tonemap.frag.spv"};  // mesh 复用全屏大三角（Phase 8 ①）
			return &defaultTonemap;
		}
		if (std::strcmp(name, "ao") == 0) {
			static const RGL::PipelineSpec defaultAo{
				"ao", "generated_shaders/light.mesh.spv",
				"generated_shaders/ao.frag.spv"};  // mesh 复用全屏大三角（同 tonemap/light）
			return &defaultAo;
		}
		return nullptr;
	}

	RGL::DeviceContext& device;  // 设备层引用（RenderPipeline 创建）
	RGL::Resources& res;         // 资源层引用（heap / sampler 槽 / env 烘焙）
	RGL::SceneDataService& sceneData;  // scene data 服务引用（binding 3 槽烘焙；架构债务续账步骤 1）
	RGL::Environment& env;  // 环境服务引用（env 槽 + sky sampler binding 4/5 烘焙；架构债务续账步骤 3）
	RGL::AssetTable& assets;  // 资产表引用（bindPipelineToAsset 回写；F 拆出）
	std::map<std::string, RGL::PipelineSpec> styles;          // 风格名 → SPV 规格（scene.lua）
	std::map<std::string, uint32_t> indexByName;              // 条目名 → 下标（启动期 name 解析）
	std::map<std::tuple<std::string, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, uint32_t> modelEntryBySlot;  // 模型条目去重表：(规格名, 顶点 slot, base 纹理 slot, normal slot, orm slot, emissive slot) → 下标
	std::vector<PipelineEntry> entries;                       // 管线表（下标 = 绘制契约）
	// 克隆反查表（批 10.5 picking / 10.7 highlight，addCloneEntries 填）：第一维 =
	// ClonePurpose，第二维 = 原条目下标 → 克隆条目下标。kNoClone = 该条目无此用途克隆；
	// 调用时刻之后新增条目不在表内（天然 nullopt）
	static constexpr uint32_t kNoClone = 0xFFFFFFFFu;
	std::vector<std::vector<uint32_t>> cloneRemap;
};

}  // namespace RGL
