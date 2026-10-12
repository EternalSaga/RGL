module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <chrono>           // RenderPipeline 创建耗时（批 9.5 启动基线；GMF 例外清单成员）
#include <fmt/format.h>
export module shader;

import rlstd;
import rlvk;
import vkerror;
import profiler;  // 批 9.5：applyDynamicState 的 vkcmd.count 计数（rlbase，只向下依赖）
import rllogger;  // 批 M：RenderPipeline 构造期 SPV 解析失败退回全烘的 warn 出声

namespace RGL {

/**
 * 加载 .spv 文件为 uint32 码流（二进制整读）。
 * @param filename SPV 文件路径（相对 CWD，即 build/rgl/）
 * @return SPV 码流（word 数组）；文件打不开返回 ShaderLoadFailed
 */
export [[nodiscard]] std::expected<std::vector<uint32_t>, Error>
loadShaderSpv(const std::string& filename) {
	std::ifstream file(filename, std::ios::ate | std::ios::binary);
	if (!file.is_open()) {
		return std::unexpected(Error{ErrorCode::ShaderLoadFailed,
			fmt::format("failed to open file: {}", filename)});
	}
	size_t fileSize = (size_t)file.tellg();
	std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
	file.close();
	return buffer;
}

/**
 * heap 映射的 compute shader object：binding→heap 槽一对一。
 *
 * push constant 无需映射条目——descriptor heap 下 push data 区自动供给（vkCmdPushDataEXT）。
 * 原三处同款模式（evssm_shadow / izb_shadow / gpu_cull）收成本点。
 *
 * @param dev            逻辑设备
 * @param spvPath        .spv 路径（相对 CWD，即 build/rgl/）
 * @param bindingToSlot  (binding, heap 槽) 对列表；binding 不必连续
 * @param descriptorSize descriptor heap 的槽大小
 * @throw std::runtime_error SPV 加载失败
 */
export [[nodiscard]] vk::raii::ShaderEXT
makeMappedCompute(vk::raii::Device& dev, const std::string& spvPath,
	std::span<const std::pair<uint32_t, uint32_t>> bindingToSlot,
	uint32_t descriptorSize) {
	auto spvResult = loadShaderSpv(spvPath);
	if (!spvResult)
		throw std::runtime_error(spvResult.error().describe());
	const auto spv = std::move(*spvResult);

	std::vector<VkDescriptorSetAndBindingMappingEXT> mappings;
	mappings.reserve(bindingToSlot.size());
	for (const auto& [binding, slot] : bindingToSlot) {
		VkDescriptorSetAndBindingMappingEXT m{};
		m.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
		m.descriptorSet = 0;
		m.firstBinding = binding;
		m.bindingCount = 1;
		m.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
		m.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
		m.sourceData.constantOffset.heapOffset = slot * descriptorSize;
		m.sourceData.constantOffset.heapArrayStride = descriptorSize;
		mappings.push_back(m);
	}
	VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{};
	mappingInfo.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT;
	mappingInfo.mappingCount = static_cast<uint32_t>(mappings.size());
	mappingInfo.pMappings = mappings.data();

	VkShaderCreateInfoEXT raw{};
	raw.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
	raw.pNext = &mappingInfo;
	raw.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
	raw.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	raw.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
	raw.codeSize = spv.size() * sizeof(uint32_t);
	raw.pCode = spv.data();
	raw.pName = "main";
	vk::ShaderCreateInfoEXT createInfo(raw);
	auto shaders = dev.createShadersEXT(std::vector<vk::ShaderCreateInfoEXT>{createInfo});
	return std::move(shaders[0]);
}

/**
 * push constant 里 heap push index 的字节偏移（= 第 5 个 uint32）。
 *
 * 两个使用方（env_prefilter / mip_gen）各自的
 * `static_assert(offsetof(<各自>PushData, heapIndex) == kPushIndexByte)` 现在锁**同一个数**——
 * 比原先两份各自 `constexpr` 更强：以前两个常量可以互相漂移而各自 assert 仍然通过，
 * 于是两个模块对「push index 在哪」静默不一致。
 */
export inline constexpr uint32_t kPushIndexByte = 5 * 4;

/**
 * 逐 mip level 的 heap 映射 compute shader object。
 *
 * 3 条 mapping：binding 0 = src sampled image、1 = dst storage image（均 resource heap，
 * **push-index 源**，heapOffset = 各自槽 × resourceDescriptorSize）；
 * binding 2 = sampler（sampler heap，**constant-offset 源**，全 level 共用一份、从不重写）。
 *
 * push-index 配方（非 constant-offset）= RADV heap storage image 唯一验证可用路径
 * （docs/DESCRIPTOR_HEAP_SPEC.md 坑 17）：push index 恒 0 → offset = heapOffset + 0×stride =
 * heapOffset（slot 固定，靠不同 level 的 shader object 绑不同槽换视图，非重写同槽）。
 *
 * 只取 descriptor **尺寸**而非 heap 对象 —— 故本模块不依赖 VmaBuffer（**不加依赖边**）。
 *
 * @param dev                    逻辑设备
 * @param code                   compute SPV 码流（调用方持有，逐 level 复用同一份）
 * @param srcSlot                src sampled image 描述符在 resource heap 的槽号
 * @param dstSlot                dst storage image 描述符在 resource heap 的槽号
 * @param samplerSlot            共享 sampler 在 sampler heap 的槽号
 * @param resourceDescriptorSize resource heap 槽大小
 * @param samplerDescriptorSize  sampler heap 槽大小
 * @param levelIndex             仅用于错误消息定位（= 正在建的 level 下标）
 * @param sub                    错误消息前缀（"env_prefilter" / "mip_gen"，各站排查抓手）
 * @return shader object；createShadersEXT 抛 vk::SystemError = 转 unexpected
 */
export [[nodiscard]] std::expected<vk::raii::ShaderEXT, Error>
makePerLevelComputeShader(vk::raii::Device& dev, const std::vector<uint32_t>& code,
	uint32_t srcSlot, uint32_t dstSlot, uint32_t samplerSlot,
	uint32_t resourceDescriptorSize, uint32_t samplerDescriptorSize,
	uint32_t levelIndex, std::string_view sub) {
	VkDescriptorSetAndBindingMappingEXT mappings[3]{};
	// binding 0 = src sampled image（resource heap，push-index 源，heapOffset = src 槽字节偏移）
	mappings[0].sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
	mappings[0].firstBinding = 0;
	mappings[0].bindingCount = 1;
	mappings[0].resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
	mappings[0].source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT;
	mappings[0].sourceData.pushIndex.heapOffset = srcSlot * resourceDescriptorSize;
	mappings[0].sourceData.pushIndex.pushOffset = kPushIndexByte;
	mappings[0].sourceData.pushIndex.heapIndexStride = resourceDescriptorSize;
	mappings[0].sourceData.pushIndex.heapArrayStride = resourceDescriptorSize;
	mappings[0].sourceData.pushIndex.pEmbeddedSampler = nullptr;
	mappings[0].sourceData.pushIndex.useCombinedImageSamplerIndex = VK_FALSE;
	// binding 1 = dst storage image（resource heap，push-index 源）。**整条 struct 赋值继承
	// binding 0 已填的全部字段**（含 heapIndexStride / heapArrayStride / pushOffset），随后只改
	// firstBinding 与 heapOffset——这是原 env_prefilter / mip_gen 两份实现的既有形态，
	// 逐字段照搬，**勿"补齐"也勿"删掉 stride"**（改了就是改渲染参数）。
	mappings[1] = mappings[0];
	mappings[1].firstBinding = 1;
	mappings[1].sourceData.pushIndex.heapOffset = dstSlot * resourceDescriptorSize;
	// binding 2 = sampler（sampler heap，constant-offset 源；一份全 level 共用、从不重写）
	mappings[2].sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
	mappings[2].firstBinding = 2;
	mappings[2].bindingCount = 1;
	mappings[2].resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
	mappings[2].source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
	mappings[2].sourceData.constantOffset.heapOffset = samplerSlot * samplerDescriptorSize;
	mappings[2].sourceData.constantOffset.heapArrayStride = 0;
	mappings[2].sourceData.constantOffset.pEmbeddedSampler = nullptr;

	VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{};
	mappingInfo.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT;
	mappingInfo.mappingCount = 3;
	mappingInfo.pMappings = mappings;

	VkShaderCreateInfoEXT raw{};
	raw.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
	raw.pNext = &mappingInfo;
	raw.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
	raw.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	raw.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
	raw.codeSize = code.size() * sizeof(uint32_t);
	raw.pCode = code.data();
	raw.pName = "main";
	vk::ShaderCreateInfoEXT createInfo(raw);
	try {
		auto built = dev.createShadersEXT(std::vector<vk::ShaderCreateInfoEXT>{createInfo});
		return std::move(built[0]);
	} catch (const vk::SystemError& e) {
		return std::unexpected(Error{ErrorCode::VulkanError,
			fmt::format("{}: compute shader object 创建失败（level {}）: {}", sub, levelIndex, e.what())});
	}
}

/** shader 对象对的 SPV 码流（mesh + fragment 两阶段，RenderPipeline 构造输入）。 */
export struct ShaderEffect {
	std::vector<uint32_t> meshStageSpv;
	std::vector<uint32_t> fragStageSpv;
};

/**
 * SPIR-V 接口签名（Phase 10 批 10.4 shader 热重载守卫）：解析 SPV 二进制提取"对外可见
 * 接口"的规范化文本——两次编译签名相等 ⇔ 新 shader 与在役 pipeline 的烘焙 binding
 * mapping / push argsSize / render pass 附件数 / mesh 工作组契约全部兼容（= L1
 * "同签名"判定，热换后不重启即正确）。
 *
 * 采集面（每条目一个 token，排序后 `;` 连接——声明序无关）：
 *  · 接口变量（OpVariable）：storage class 号 + **原始 decoration 集**（dec{值}:{操作数}
 *    原样入 token——Binding=33/DescriptorSet=34/Location=30 等语义无需硬编码，原始集
 *    相等即语义相等）+ 指向类型**递归结构 token**（类型 id 编译间漂移，必须展开到
 *    字面量：标量宽度/符号、向量/矩阵维、数组长度（常量解析）、struct 成员展开、
 *    image 系操作数字面量）
 *  · 执行模式（OpExecutionMode）：mode + 操作数字面量——mesh shader 的 LocalSize /
 *    OutputVertices / OutputPrimitivesEXT 在此钉死（cull groupCount = kTrianglesPerGroup
 *    契约若被改 = 签名变化被拦）。
 *
 * 已知边界（v1，L1 够用）：decoration group（OpGroupDecorate）不解析（slangc 直出
 * OpDecorate）；数组长度非常量时落 "?"；解析失败返回 "SPV_CORRUPT"（哨兵互不相等 →
 * 守卫必拒，fail-safe）。同型成员重排（纯 Offset decoration 漂移）经 struct 的
 * member decoration 集采集覆盖（appendDecos 的 MD 行）。
 */
export std::string spvInterfaceSignature(const std::vector<uint32_t>& spv);

/**
 * SPV 实际声明的 descriptor binding 集合（批 M，docs/constantbuffer_迁移计划.md §9.3：
 * mapping 按声明过滤，启动面不随 binding 总数线性膨胀）。
 *
 * 判据 = 所有**带 `Binding` 装饰（33）的 OpVariable** 的 binding 号：resource heap 描述符
 * 与 **sampler heap 描述符**（OpTypeSampler 变量同样带 Binding 装饰，漏了它 = 采样器没映射
 * = 画面坏）同路采集；push constant 变量无 Binding 装饰，天然不在集合里。复用
 * spvInterfaceSignature 的同一套遍历/decoration 采集机制（spvSplit），不另起解析器。
 *
 * 已知边界同 spvInterfaceSignature：OpGroupDecorate 不解析（slangc 直出 OpDecorate）。
 *
 * @param spv SPV 码流（word 数组）
 * @return 声明的 binding 号集合（可为空集 = 该 SPV 确实什么都不声明）；
 *         **nullopt = 解析失败**（魔数不对/截断/结构不认识）——调用方必须 fail-safe 退回
 *         全烘，**不可**把失败读成空集（空集 = 滤掉全部 mapping = 静默读零）
 */
export std::optional<std::set<uint32_t>> spvDeclaredBindings(const std::vector<uint32_t>& spv);

/**
 * 动态状态（Dynamic State）：与 shader object 解耦的"pass 状态"值集合。
 * 从 RenderPipeline 移出（原 recordRenderCommands 内烘焙），由调用点（Renderer）
 * 按 pass 传入——同一 mesh shader 可在不同 pass 用不同状态（Phase 7 G-buffer 多 pass）。
 * 默认值 = 当前 cube 的 opaque pass 状态（不透明、写深度、无混合、单采样）。
 * 注意：viewport/scissor 依赖窗口宽高，不在此结构，由 applyDynamicState 的
 * width/height 参数设置。
 */
export struct RenderState {
	vk::PolygonMode polygonMode = vk::PolygonMode::eFill;
	vk::CullModeFlagBits cullMode = vk::CullModeFlagBits::eNone;
	vk::FrontFace frontFace = vk::FrontFace::eCounterClockwise;
	bool rasterizerDiscard = false;
	bool conservative = false;  // 批 11.3-1d：OVERESTIMATE 保守光栅（IZB mask pass；EDS3 动态位，缺能力时 applyDynamicState 不 set = 行为不变）
	bool depthBiasEnable = false;
	float depthBiasSlope = 0.0f;
	float depthBiasScale = 0.0f;
	bool depthTest = true;
	bool depthClipEnable = true;  // edS3：深度裁剪开关（默认 true = 传统行为；per-pass 可动态关）
	bool depthWrite = true;
	vk::CompareOp depthCompareOp = vk::CompareOp::eLess;
	bool depthBoundsTest = false;
	bool stencilTest = false;
	vk::SampleCountFlagBits rasterizationSamples = vk::SampleCountFlagBits::e1;
	uint32_t sampleMask = 0xFFFFFFFF;
	bool alphaToCoverage = false;
	bool alphaToOne = false;
	bool colorBlend = false;
	vk::ColorComponentFlags colorWriteMask =
		vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
		vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
	bool logicOp = false;
};

/**
 * 设置全部动态状态（viewport/scissor 用 width/height，其余用 state）。
 * spec 要求相关 set 命令在 draw 之前调用即可（draw 时采样），与 bindShadersEXT
 * 的先后无约束。
 * @param cmd 命令缓冲（录制期）
 * @param area 渲染区域矩形（viewport + scissor）。**Phase 11 批 11.2 从 (width, height) 改为 rect**：
 *        阴影图集分块需要偏移原点（不是全部从 (0,0) 开始）；传全尺寸两参数就不够用。
 *        传 {0,0,w,h} 时与旧两参数形式**逐位一致**（见 @param 说明的 y 翻转）
 * @param area 视口：viewport y = area.y + area.h、h = -area.h（Vulkan 坐标原点左上）
 * @param state 状态值集合
 * @param depthClipDynamic edS3 能力门：true = 设备支持 extendedDynamicState3DepthClipEnable，
 *        才 setDepthClipEnableEXT；false = 扩展缺失，保持管线默认（干净 no-op，不 set）
 * @param colorAttachmentCount 颜色附件数（多颜色附件须对 [0, count) 全部设 blend enable
 *        与 write mask，VVL 07476/07478；默认 1 = forward）。**0 = depth-only pass**
 *        （Phase 11 批 11.2 阴影图）：色态整段跳过——setColorBlendEnableEXT /
 *        setColorWriteMaskEXT 的 attachmentCount 必须 > 0，传空数组是 VVL 报错
 *        （2026-09-29 实测：`vkCmdSetColorBlendEnableEXT(): attachmentCount must be
 *        greater than 0`），而没有色附件时这些状态本就无对象可作用。
 */
export void applyDynamicState(vk::raii::CommandBuffer& cmd, const vk::Rect2D& area,
	const RenderState& state, bool depthClipDynamic = false, uint32_t colorAttachmentCount = 1) {
	// Viewport & scissor（y 翻转：Vulkan 视口原点在左上，NDC y=+1 对应 area.y）
	const float x = static_cast<float>(area.offset.x);
	const float yTop = static_cast<float>(area.offset.y + area.extent.height);
	const float w = static_cast<float>(area.extent.width);
	const float h = static_cast<float>(area.extent.height);
	vk::Viewport viewport(x, yTop, w, -h, 0.0f, 1.0f);
	vk::Rect2D scissor(area.offset, area.extent);
	cmd.setViewportWithCount(viewport);
	cmd.setScissorWithCount(scissor);

	// Rasterization
	cmd.setPolygonModeEXT(state.polygonMode);
	cmd.setCullMode(state.cullMode);
	cmd.setFrontFace(state.frontFace);
	// 批 11.3 红线教训（2026-10-02 复盘）：曾在此加 conservativeDynamic 参数给**每个
	// pass** 显式写 conservative 模式——RADV 上「显式 set DISABLED」并非与「从不 set」
	// 逐位等价（污染 shadow 光栅 → CSM 影轮廓漂移），且「门开=门关」红线对这种共享
	// 代码污染天然瞎。正确形态 = IZB mask pass 内部一次性 set + 出 pass 复原（见
	// vkrenderer_passrec replayIzbMaskSegment）。
	cmd.setRasterizerDiscardEnable(state.rasterizerDiscard);
	cmd.setDepthBiasEnable(state.depthBiasEnable);
	cmd.setDepthBias(state.depthBiasScale, state.depthBiasSlope, 0.0f);

	// Depth & stencil
	cmd.setDepthTestEnable(state.depthTest);
	cmd.setDepthWriteEnable(state.depthWrite);
	cmd.setDepthCompareOp(state.depthCompareOp);
	cmd.setDepthBoundsTestEnable(state.depthBoundsTest);
	cmd.setStencilTestEnable(state.stencilTest);
	// edS3：depth clip enable 动态化（per-pass 控制；feature 可用才 set，缺失则 no-op）
	if (depthClipDynamic)
		cmd.setDepthClipEnableEXT(state.depthClipEnable);

	// Multisampling
	cmd.setRasterizationSamplesEXT(state.rasterizationSamples);
	cmd.setSampleMaskEXT(state.rasterizationSamples, state.sampleMask);
	cmd.setAlphaToCoverageEnableEXT(state.alphaToCoverage);
	cmd.setAlphaToOneEnableEXT(state.alphaToOne);

	// Color blending + 写掩码：多颜色附件（Phase 7 deferred geometry 4 MRT）须覆盖 [0, count)
	//（VVL 07476/07478：shader object 动态色态要求所有 active color attachment 均已设 blend enable
	// 与 write mask）。forward（count=1）与旧逐位一致。
	if (state.colorBlend && colorAttachmentCount > 0) {
		// hpp 包装类无聚合初始化（仅默认/拷贝/Vk* 构造）——裸 C 结构体逐字段填后包一层。
		// 注意 v341 头：单 colorBlendOp/alphaBlendOp 字段（非 src/dst 各一，新版头才拆分）
		VkColorBlendEquationEXT raw{};
		raw.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		raw.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		raw.colorBlendOp = VK_BLEND_OP_ADD;
		raw.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		raw.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		raw.alphaBlendOp = VK_BLEND_OP_ADD;
		const std::vector<vk::ColorBlendEquationEXT> eqs(colorAttachmentCount,
			vk::ColorBlendEquationEXT(raw));
		cmd.setColorBlendEquationEXT(0, eqs);
	}
	// count == 0 = depth-only pass（阴影图，见 @param 注）→ 整段跳过，count ≥1 逐位不变
	if (colorAttachmentCount > 0) {
		const std::vector<vk::Bool32> blendEnable(colorAttachmentCount,
			state.colorBlend ? VK_TRUE : VK_FALSE);
		cmd.setColorBlendEnableEXT(0, blendEnable);
		const std::vector<vk::ColorComponentFlags> writeMasks(colorAttachmentCount,
			vk::ColorComponentFlags(state.colorWriteMask));
		cmd.setColorWriteMaskEXT(0, writeMasks);
	}
	cmd.setLogicOpEnableEXT(state.logicOp);
	// 批 9.5 vkcmd.count 口径：本函数 = 每 pass 一次的动态状态命令块。18 = 无条件 18 条
	// （viewport/scissor/polygonMode/cullMode/frontFace/rasterizerDiscard/depthBiasEnable/
	// depthBias/depthTest/depthWrite/depthCompareOp/depthBoundsTest/stencilTest/
	// rasterizationSamples/sampleMask/alphaToCoverage/alphaToOne/logicOpEnable），
	// 条件三项与各 if 同判据。**改上面的分支必须同步改这里**（漏改 = 计数悄悄失真）。
	profilerAddCommands(18u
		+ (depthClipDynamic ? 1u : 0u)
		+ (state.colorBlend && colorAttachmentCount > 0 ? 1u : 0u)
		+ (colorAttachmentCount > 0 ? 2u : 0u));
}

/**
 * Phase 7 D1：base binding 之外的单条追加映射（PipelineBindings::extra 条目）。
 * sampler=false → resource heap 描述符（heapSlot × descriptorSize）；sampler=true →
 * sampler heap 字节偏移（samplerHeapOffset）。binding 显式（避开 base 0..5 段）。
 */
export struct PipelineExtraBinding {
	uint32_t binding = 0;
	bool sampler = false;
	uint32_t heapSlot = 0;
	vk::DeviceSize samplerHeapOffset = 0;
};

/**
 * binding mapping 烘焙参数（Phase 5.5 聚合：env 追加后构造参数膨胀到 13 个，改 struct
 * 数据驱动表达 cube/model/IBL 形态）。heap 槽 = 绝对 slot（resource heap），sampler 走
 * **字节偏移**（sampler heap，OpTypeSampler 映射的 spec 正确字段 heapOffset，见
 * RenderPipeline 内注释）——换算（slot × descriptorSize）在调用点（ShaderRegistry）做。
 */
export struct PipelineBindings {
	uint32_t descriptorSize = 0;   // resource heap 步长（max(buffer, image) 运行时查询）
	uint32_t slotVertex = 0;       // binding 0：顶点 storage buffer 槽
	uint32_t slotIndex = 0;        // binding 1（非 textured 形态）：索引槽
	uint32_t slotTexture = 0;      // binding 1（textured 形态）：SAMPLED_IMAGE 槽
	uint32_t samplerHeapOffset = 0;  // binding 2（textured）：sampler heap 字节偏移
	uint32_t slotSceneData = 0;    // binding 3（hasSceneData）：scene data storage 槽
	uint32_t slotSceneWet = 0;     // binding 32（迁移批 1，hasSceneData 恒带上）：湿面区
	                             // UBO 子范围槽（SceneDataService::wetSlot；未声明 binding 32
	                             // 的条目由批 M 的 SPV 声明过滤自动裁掉——不在此登记消费者）
	uint32_t slotSceneIrradiance = 0;  // binding 27（迁移批 2，hasSceneData 恒带上）：辐照度区
	                             // UBO 子范围槽（SceneDataService::irradianceSlot；裁未声明者同批 M）
	uint32_t slotSceneLights = 0;  // binding 24（迁移批 3，hasSceneData 恒带上）：灯数组区
	                             // UBO 子范围槽（SceneDataService::lightSlot；裁未声明者同批 M）
	uint32_t slotSceneShadowViewProj = 0;  // binding 28（迁移批 S-1，hasSceneData 恒带上）：阴影
	                             // 光空间 VP 区 UBO 子范围槽（SceneDataService::shadowViewProjSlot；
	                             // 裁未声明者同批 M）
	uint32_t slotSceneShadowParams = 0;  // binding 29（迁移批 S-2，hasSceneData 恒带上）：阴影
	                             // 采样参数区 UBO 子范围槽（SceneDataService::shadowParamsSlot；
	                             // 裁未声明者同批 M）
	// 类型化迁移 批 T1：逐对象四张表的 SSBO 子范围槽（SceneDataService::objectMatrixSlot 等）。
	// 与上面三条同纪律 = hasSceneData 一律带上、C++ 侧不登记谁用，未声明的 binding 由批 M 的
	// SPV 声明过滤自动裁掉。元素步长由 shader 侧 StructuredBuffer<T> 的类型给（R4 逐次构建
	// 对账 ArrayStride == 反射 sizeof），C++ 侧不再往下传 stride。
	uint32_t slotObjectMatrices = 0;   // binding 23：对象世界矩阵表（StructuredBuffer<ObjectMatrix>）
	uint32_t slotObjectDraws = 0;      // binding 26：对象绘制属性表（StructuredBuffer<ObjectDraw>）
	uint32_t slotObjectMVPs = 0;       // binding 25：对象 MVP 表（StructuredBuffer<ObjectMatrix>）
	uint32_t slotObjectClusterRanges = 0;  // binding 33：对象簇区间表（StructuredBuffer<ObjectClusterRange>）
	uint32_t slotEnv = 0;          // binding 4（hasEnv）：env cubemap SAMPLED_IMAGE 槽
	uint32_t envSamplerHeapOffset = 0;  // binding 5（hasEnv）：sky sampler heap 字节偏移
	bool textured = false;
	bool hasSceneData = false;
	bool hasEnv = false;
	// Phase 7 D1：任意追加映射（base binding 0..5 之外，按 firstBinding 显式指定）。
	// light pass 采样 G-buffer 用：binding 4-7 → 4 个 RT 的 SAMPLED_IMAGE 堆槽、binding 8 → sampler。
	// sampler=true → sourceData 走 sampler heap 字节偏移；false → resource heap slot×descriptorSize。
	std::vector<PipelineExtraBinding> extra;
};

/**
 * shader object 创建统计（批 9.5 观测面 = 条目/启动基线的唯一数据源）。
 *
 * RenderPipeline 是 VkShaderEXT 的**唯一生产者**（每实例一次 vkCreateShadersEXT、
 * 固定 mesh + frag 两个句柄），故本处计数即"管线表条目数 / 句柄数 / 创建总耗时 /
 * binding mapping 条数"四个数——docs/shader_object_组合成本与优化预留.md 的旧基线
 * （312 条目 / 1252 句柄 / ≈2.3s，2026-09-28 forward + picking 临时开）由此转为可复测。
 *
 * ⚠ 口径 = **累计构造次数**（含 shader 热重载换入的新条目），不是"当前存活条目数"；
 * 启动基线在帧循环前读一次即等于条目表大小。
 */
export struct ShaderObjectStats {
	uint64_t pipelines = 0;  // RenderPipeline 构造次数（启动期 = 管线表条目数）
	uint64_t handles = 0;    // VkShaderEXT 句柄数（恒 = 2 × pipelines）；**只含管线表**——
	                         // mip_gen / env_prefilter 的 compute shader object 直接调
	                         // vkCreateShadersEXT、不走本类，不在本账内
	uint64_t mappings = 0;   // binding mapping 条数累计（条目爆炸的第二维度）
	uint64_t createNs = 0;   // vkCreateShadersEXT 累计 wall clock（ns）
};

/** shader object 创建统计累加器（inline 函数局部静态 = 跨 TU 唯一实例）。 */
namespace shaderStatsDetail {
	/** @return 可写统计单例（RenderPipeline 构造期自增；读侧走 shaderObjectStats）。 */
	inline ShaderObjectStats& mutableStats() {
		static ShaderObjectStats s;
		return s;
	}
}

/** @return shader object 创建统计快照（见 ShaderObjectStats 口径说明）。 */
export inline ShaderObjectStats shaderObjectStats() {
	return shaderStatsDetail::mutableStats();
}

/**
 * RenderPipeline：纯 shader object 包装（mesh + frag 两个 VkShaderEXT + 烘焙的
 * descriptor heap binding mapping）。动态状态不再内聚于此——见
 * RenderState / applyDynamicState。RAII：设备销毁前由本对象持有句柄。
 */
export class RenderPipeline {
	vk::raii::ShaderEXT meshShader = nullptr;
	vk::raii::ShaderEXT fragShader = nullptr;

    public:
	/**
	 * 创建 mesh + frag 两个 VkShaderEXT（vk::createShadersEXT 一次两个），
	 * binding mapping 按 bindings 形态烘焙（running index 连续填，firstBinding 显式）。
	 * 🔴 批 M（§9.3）：**只为 mesh + frag 两份 SPV 并集里真声明了的 binding 发 mapping**
	 *（判据 = spvDeclaredBindings）；未声明的 binding 即使 bindings 形态里带了也不烘。
	 * 任一份 SPV 解析失败 ⟹ 退回全烘（= 批 M 之前的行为，恒正确）+ warn 点名条目。
	 * 方向不变的红线：「SPV 声明了却无 mapping」是 VVL 错误——所以过滤只敢滤「没声明的」，
	 * 声明了的一定向烘（两侧同向约束见 ShaderRegistry 各 add*Entry 注释）。
	 * bindings.textured 缺省 = cube 形态（2 条目：binding 0/1）。
	 * textured=true = 模型形态（3 条目，Phase 3）：binding 1 → SAMPLED_IMAGE 描述符槽
	 *（resource heap，slotTexture），binding 2 → sampler heap 槽（OpTypeSampler 映射，
	 * heapOffset 字节偏移——独立 sampler 的 spec 正确字段，见下方注释；
	 * vkCmdBindSamplerHeapEXT 每帧绑定）。
	 * 注意：sampler 走独立 sampler heap 而非 pEmbeddedSampler——2026-08-27 spike
	 * 交叉测试两者三驱动均 PASS（docs/archive/heap_texture_research.md §九），此处维持独立
	 * sampler heap（docs/archive/phase3_plan.md R1 回退链的既选路径，已验证可用）。
	 * hasSceneData=true（Phase 5 批次 2，模型形态）追加 binding 3 → scene data storage
	 * buffer 槽（光照 + 对象矩阵表，SceneDataService::ensure；不声明 binding 3 的
	 * mesh/unlit frag 该条被批 M 过滤掉，不再白烘）。
	 * hasEnv=true（Phase 5.5）追加 binding 4/5 → env cubemap 槽 + sky sampler 偏移
	 *（仅 import env.slang 的风格消费——pbr/phong/unlit 不声明 = 批 M 后不烘）。
	 * extra（Phase 7 D1）续 base 追加（light pass 采 G-buffer 4 RT + sampler）。
	 * @param device 设备句柄
	 * @param effect mesh + frag 两阶段 SPV 码流
	 * @param bindings mapping 烘焙参数（见 PipelineBindings）
	 * @param debugName 条目名（仅进 warn/throw 消息定位用，批 M：解析失败退全烘要点名条目）
	 */
	RenderPipeline(vk::raii::Device& device, const ShaderEffect& effect,
		const PipelineBindings& bindings, std::string_view debugName = {}) {
		const uint32_t descriptorSize = bindings.descriptorSize;
		// 容量 48（迁移批 1 从 24 抬，§9.7 红线：过滤发生在计数之后，容量必须盖过「全烘」
		// 的最坏条目数 + 后续批 +12 binding 的扩张面）：model 条目已用 14（base 0-5 +
		// binding 32 + extra 6/7/8/12/13/15/16），meshlet 条目再 +5 个簇 buffer（17-21），
		// 批 T1 再 +4 张逐对象表（23/25/26/33）。
		// 上限判据 = 最多 extra 的条目，别贴边。批 M 过滤只让**实际烘焙数**减不让它增，
		// 越界保护（add 内 throw 点名条目）防的是未来 extra 列表继续加长时静默越界写栈。
		VkDescriptorSetAndBindingMappingEXT mappings[48]{};
		// 批 M（§9.3）：mesh + frag 两份 SPV 声明 binding 的**并集**（一个 binding 可能只在
		// 一侧声明，如纹理/sampler 只在 frag）。任一解析失败（nullopt）= filter 关 = 全烘
		// + warn——全烘恒正确（「有 mapping 未声明」空转无害既证），静默滤错才是画面事故。
		const auto meshDeclared = spvDeclaredBindings(effect.meshStageSpv);
		const auto fragDeclared = spvDeclaredBindings(effect.fragStageSpv);
		const bool filter = meshDeclared.has_value() && fragDeclared.has_value();
		std::set<uint32_t> declared;
		if (filter) {
			declared = *meshDeclared;
			declared.insert(fragDeclared->begin(), fragDeclared->end());
		} else if (auto* logger = RLLogger::getInstance()) {
			logger->warn("spvDeclaredBindings: 条目 '{}' 的 SPV 解析失败（mesh 解析={} frag "
				"解析={}），本条目退回全烘（批 M 过滤失效方向 = 正确方向，但启动面收益丢失）",
				debugName, meshDeclared.has_value(), fragDeclared.has_value());
		}
		// 顺序填充（Phase 5.5 批次 4 修正：原固定下标 [0/1/2/3/4/5] 假设 textured=true——
		// 天空盒首个 textured=false+hasSceneData+hasEnv 组合下 mappings[2] 留空却计入
		// count → VVL "sType must be..." + envSampler 被挤出 count → "no mapping for
		// binding 5"。改 running index 按实际启用的 binding 连续填，firstBinding 显式携带）
		uint32_t n = 0;
		const auto add = [&](uint32_t firstBinding, vk::DeviceSize heapOffset,
			vk::DeviceSize arrayStride) {
			// 批 M：SPV 没声明的 binding 不烘（解析失败时 filter 关 = 恒烘，行为 = 批 M 前）
			if (filter && !declared.contains(firstBinding))
				return;
			if (n >= 48)
				throw std::runtime_error(fmt::format(
					"RenderPipeline '{}': mapping 条目数超过 mappings[48] 容量（binding {} 无处放）",
					debugName, firstBinding));
			auto& m = mappings[n++];
			m.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
			m.descriptorSet = 0;
			m.firstBinding = firstBinding;
			m.bindingCount = 1;
			m.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
			m.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
			m.sourceData.constantOffset.heapOffset = heapOffset;
			m.sourceData.constantOffset.heapArrayStride = arrayStride;
		};

		add(0, bindings.slotVertex * descriptorSize, descriptorSize);
		// binding 1：非纹理形态 = 索引 slot；纹理形态 = SAMPLED_IMAGE 描述符槽（resource heap）
		add(1, (bindings.textured ? bindings.slotTexture : bindings.slotIndex) * descriptorSize,
			descriptorSize);
		if (bindings.textured) {
			// OpTypeSampler 映射：sampler heap 槽（字节偏移，共享一份 sampler 描述符）。
			// spec（descriptorheaps.adoc :1664-1665 + VUID 11253）：独立 OpTypeSampler 的
			// sampler heap 偏移写 heapOffset（"mapped resource or sampler"）；samplerHeapOffset
			// 字段仅组合 image sampler 用（:1671-1674）——2026-08-27 交叉测试定位的字段误用
			//（spike 首版同款，docs/archive/heap_texture_research.md §九.2 根因 2）
			add(2, bindings.samplerHeapOffset, 0);
		}
		if (bindings.hasSceneData) {
			add(3, bindings.slotSceneData * descriptorSize, descriptorSize);
			// 迁移批 1：湿面区 UBO 载体（binding 32）。**hasSceneData 一律带上、不在 C++ 侧
			// 登记谁用**——SPV 没声明 binding 32 的条目被上面的批 M 过滤自动裁掉（§9.3/§9.7；
			// 「声明了未 mapping」才是 VUID-11292 启动炸，方向别记反）。
			add(32, bindings.slotSceneWet * descriptorSize, descriptorSize);
			// 迁移批 2：辐照度区 UBO 载体（binding 27），同批 1 纪律（恒带上、过滤裁未声明者）
			add(27, bindings.slotSceneIrradiance * descriptorSize, descriptorSize);
			// 迁移批 3：灯数组区 UBO 载体（binding 24），同批 1 纪律
			add(24, bindings.slotSceneLights * descriptorSize, descriptorSize);
			// 迁移批 S-1：阴影光空间 VP 区 UBO 载体（binding 28），同批 1 纪律
			add(28, bindings.slotSceneShadowViewProj * descriptorSize, descriptorSize);
			// 迁移批 S-2：阴影采样参数区 UBO 载体（binding 29），同批 1 纪律
			add(29, bindings.slotSceneShadowParams * descriptorSize, descriptorSize);
			// 类型化迁移 批 T1：逐对象四张表 SSBO 载体（binding 23/25/26/33），同批 1 纪律。
			// 只有 import scene_tables.slang 且真用到某张表的条目才在 SPV 里声明它（未用到的
			// 表被 Slang 死代码消除掉），批 M 过滤把剩下的裁干净 ⟹ 烘的恰好是用的。
			add(23, bindings.slotObjectMatrices * descriptorSize, descriptorSize);
			add(25, bindings.slotObjectMVPs * descriptorSize, descriptorSize);
			add(26, bindings.slotObjectDraws * descriptorSize, descriptorSize);
			add(33, bindings.slotObjectClusterRanges * descriptorSize, descriptorSize);
		}
		if (bindings.hasEnv) {
			// Phase 5.5：binding 4 → env cubemap（resource heap SAMPLED_IMAGE 槽）；
			// binding 5 → sky sampler（sampler heap 字节偏移，OpTypeSampler 映射规则同 binding 2）
			add(4, bindings.slotEnv * descriptorSize, descriptorSize);
			add(5, bindings.envSamplerHeapOffset, 0);
		}
		// Phase 7 D1：追加映射（light pass 采样 G-buffer 4 RT + sampler 等），running index 续 base
		for (const auto& e : bindings.extra)
			add(e.binding,
				e.sampler ? e.samplerHeapOffset : descriptorSize * e.heapSlot,
				e.sampler ? 0 : descriptorSize);
		const uint32_t mappingCount = n;

		VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{};
		mappingInfo.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT;
		mappingInfo.mappingCount = mappingCount;
		mappingInfo.pMappings = mappings;

		VkShaderCreateInfoEXT meshCreateInfoRaw{};
		meshCreateInfoRaw.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
		meshCreateInfoRaw.pNext = &mappingInfo;
		// INDIRECT_BINDABLE：进 DGC execution set 的 shader 必须带（VUID-11032/11154；
		// 非 DGC 路无害，统一加免得管线表再分两类）
		meshCreateInfoRaw.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT | VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT | VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT;
		meshCreateInfoRaw.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
		meshCreateInfoRaw.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
		meshCreateInfoRaw.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
		meshCreateInfoRaw.codeSize = effect.meshStageSpv.size() * sizeof(uint32_t);
		meshCreateInfoRaw.pCode = effect.meshStageSpv.data();
		meshCreateInfoRaw.pName = "main";
		vk::ShaderCreateInfoEXT meshCreateInfo(meshCreateInfoRaw);

		VkShaderCreateInfoEXT fragCreateInfoRaw{};
		fragCreateInfoRaw.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
		fragCreateInfoRaw.pNext = &mappingInfo;
		fragCreateInfoRaw.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT | VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT;
		fragCreateInfoRaw.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		fragCreateInfoRaw.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
		fragCreateInfoRaw.codeSize = effect.fragStageSpv.size() * sizeof(uint32_t);
		fragCreateInfoRaw.pCode = effect.fragStageSpv.data();
		fragCreateInfoRaw.pName = "main";
		vk::ShaderCreateInfoEXT fragCreateInfo(fragCreateInfoRaw);

		std::vector<vk::ShaderCreateInfoEXT> createInfos = {meshCreateInfo, fragCreateInfo};
		// 批 9.5 观测：条目/句柄/mapping 计数 + vkCreateShadersEXT wall clock（口径见
		// ShaderObjectStats 注释）。纯计数，不改任何创建参数。
		const auto createBegin = std::chrono::steady_clock::now();
		auto shaders = device.createShadersEXT(createInfos);
		auto& stats = shaderStatsDetail::mutableStats();
		++stats.pipelines;
		stats.handles += 2;  // mesh + frag（本函数唯一句柄产出点）
		stats.mappings += mappingCount;
		stats.createNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - createBegin).count());
		meshShader = std::move(shaders[0]);
		fragShader = std::move(shaders[1]);
	}
	/**
	 * 绑定 mesh + frag shader object 到命令缓冲（未使用的传统 stage 传 VK_NULL_HANDLE）。
	 * 只做 bindShadersEXT——动态状态由 applyDynamicState 单独设置（调用点在 Renderer）。
	 * @param cmd 命令缓冲（录制期，draw 之前调用）
	 */
	void bindShaders(vk::raii::CommandBuffer& cmd) const {
		std::array<vk::ShaderStageFlagBits, 7> stages = {
			vk::ShaderStageFlagBits::eVertex,
			vk::ShaderStageFlagBits::eTessellationControl,
			vk::ShaderStageFlagBits::eTessellationEvaluation,
			vk::ShaderStageFlagBits::eGeometry,
			vk::ShaderStageFlagBits::eTaskEXT,
			vk::ShaderStageFlagBits::eMeshEXT,
			vk::ShaderStageFlagBits::eFragment};

		std::array<vk::ShaderEXT, 7> shaderHandles = {
			vk::ShaderEXT(nullptr), vk::ShaderEXT(nullptr),
			vk::ShaderEXT(nullptr), vk::ShaderEXT(nullptr),
			vk::ShaderEXT(nullptr),
			vk::ShaderEXT(*meshShader),
			vk::ShaderEXT(*fragShader)};

		cmd.bindShadersEXT(stages, shaderHandles);
	}
	/** @return mesh 阶段 shader 句柄（DGC execution set 一次性写入，Phase 7b；inactive 路不消费）。 */
	[[nodiscard]] vk::ShaderEXT meshShaderHandle() const { return *meshShader; }
	/** @return frag 阶段 shader 句柄（DGC execution set 一次性写入，Phase 7b；inactive 路不消费）。 */
	[[nodiscard]] vk::ShaderEXT fragShaderHandle() const { return *fragShader; }
};

}  // namespace RGL