module;
export module shader_registry;

import rlstd;
import rlvk;
import rllogger;

import vk_device;
import vk_resources;
import shader;
import scene_config;
import material_types_generated;
import vkerror;

namespace RGL {

/**
 * 管线表一条（Phase 4 批次 2：从 vkrenderer.cppm 迁入，Vulkan 层零 entt）：
 * shader 风格名 + 其 RenderPipeline（shader object + 烘焙的 descriptor heap binding mapping）
 * + 显式 push constant 元数据（L5 解决：argsSize/pushDataSet 替代旧 "set 0 被两种 struct
 * 隐式共享"）。argsSize = 该管线 push constant 字节数（统一 DrawArgs 超集 = 100B scalar 布局，
 * material_types_generated static_assert 与 shader 反射一致）；pushDataSet = 0（显式）。
 */
export struct PipelineEntry {
	std::string name;                             // 条目名（模型 = "{规格}.v{顶点槽}.t{纹理槽}"，skybox/cube/light 固定名）
	RGL::RenderPipeline pipeline;                 // shader object + 烘焙 binding mapping（见 PipelineBindings）
	uint32_t argsSize = sizeof(RGL::DrawArgs);    // push constant 字节数（DrawArgs 100B scalar 布局）
	uint32_t pushDataSet = 0;                     // 显式 push constant set 号（vkCmdPushDataEXT 用）
};

/**
 * 管线表（Phase 4：从 Renderer 迁出，零 entt——只认 slot/资产 id，不认场景实体）：
 *   条目全部按需动态加入：模型 = addModelEntry、内建 cube = addCubeEntry（架构债务 1：
 *   slot 经传入的资产记录取，Resources 不认识 cube）、天空盒 = addSkyEntry、
 *   延迟光 = addLightEntry；模型条目按 (规格, 顶点 slot, 纹理 slot) 组合去重
 *   ——binding mapping 是 shader object 级状态（无法运行时换），同组合的映射恒等，
 *   故共享一条条目而非每 primitive 一条（Phase 4 验收：392 primitive → 12 条）。
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
	 * @param res 资源层（heap / sampler 槽 / scene data / env——binding mapping 烘焙）
	 * @param specs 管线规格（scene.lua styles 表，name → SPV 路径；缺条目时
	 *   findSpec 回落默认路径）
	 */
	ShaderRegistry(RGL::DeviceContext& device, RGL::Resources& res,
		std::span<const RGL::PipelineSpec> specs)
		: device(device),
		  res(res) {
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
	 * 模型管线条目（资产上传期）：用管线规格创建 RenderPipeline，binding mapping 显式烘焙
	 * 顶点 slot + 纹理 slot（binding 0 = 顶点 buffer 槽、binding 1 = resource heap 的
	 * SAMPLED_IMAGE 描述符槽、binding 2 = sampler heap 槽——OpTypeSampler 映射，
	 * 共享一份 sampler 描述符，偏移 = samplerSlot * samplerHeap.getDescriptorSize()；
	 * binding 3 = scene data、4/5 = env 亦恒烘焙，见 impl）。
	 * 首参 specName = 管线规格名（Phase 4 批次 3：风格→规格名映射在 main.cpp 经 scene.lua
	 * styles 表完成，本函数只认规格名）；按 (规格, 顶点 slot, 纹理 slot) 组合去重——
	 * 同组合返回既有条目下标（不重复建 shader object）；新组合建条目并记入 indexByName /
	 * 去重表。返回条目下标，调用方经 bindToAsset 写回资产记录（renderListSystem 经资产表
	 * 解析管线，无字符串约定）。失败（规格缺失 / SPV 加载失败等）抛出（同构造期路径）。
	 * @param specName 管线规格名（scene.lua styles / findSpec 回落词表）
	 * @param vertexSlot 顶点 buffer heap 槽（binding 0；模型顶点/索引共享同一 buffer）
	 * @param textureSlot 纹理 SAMPLED_IMAGE heap 槽（binding 1；白纹理 = 纹理首槽）
	 * @param samplerSlot 共享 sampler 的 sampler heap 槽（binding 2，ensureSharedSampler 产物）
	 * @return 条目下标（新组合 = entries 追加）
	 */
	[[nodiscard]] uint32_t addModelEntry(const std::string& specName, uint32_t vertexSlot,
		uint32_t textureSlot, uint32_t samplerSlot);

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
	 *（readSceneLight 单灯）+ extra：4-7 → 4 个 G-buffer RT 的 SAMPLED_IMAGE 堆槽、8 → 共享 sampler。
	 * 前提 = 4 G-buffer RT 已 ensureRtAttachment（其堆槽由调用点取入，跨 resize 复用不变）。幂等。
	 * @param albedoSlot G-buffer albedo RT 的 SAMPLED_IMAGE heap 槽（binding 4）
	 * @param normalSlot G-buffer normal RT 槽（binding 5）
	 * @param positionSlot G-buffer position RT 槽（binding 6）
	 * @param metalRoughSlot G-buffer metalRough RT 槽（binding 7）
	 * @param samplerSlot 共享 sampler 的 sampler heap 槽（binding 8，sampler 映射）
	 * @return 条目下标；规格缺失抛 runtime_error
	 */
	[[nodiscard]] uint32_t addLightEntry(uint32_t albedoSlot, uint32_t normalSlot,
		uint32_t positionSlot, uint32_t metalRoughSlot, uint32_t samplerSlot);

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
		res.getAssetTable().setPipelineIndex(assetId, entryIndex);
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
		if (std::strcmp(name, "light") == 0) {
			static const RGL::PipelineSpec defaultLight{
				"light", "generated_shaders/light.mesh.spv", "generated_shaders/light.frag.spv"};
			return &defaultLight;
		}
		return nullptr;
	}

	RGL::DeviceContext& device;  // 设备层引用（RenderPipeline 创建）
	RGL::Resources& res;         // 资源层引用（heap / sampler 槽 / scene data / env 烘焙）
	std::map<std::string, RGL::PipelineSpec> styles;          // 风格名 → SPV 规格（scene.lua）
	std::map<std::string, uint32_t> indexByName;              // 条目名 → 下标（启动期 name 解析）
	std::map<std::tuple<std::string, uint32_t, uint32_t>, uint32_t> modelEntryBySlot;  // 模型条目去重表：(规格名, 顶点 slot, 纹理 slot) → 下标
	std::vector<PipelineEntry> entries;                       // 管线表（下标 = 绘制契约）
};

}  // namespace RGL
