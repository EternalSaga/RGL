module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <chrono>           // RenderPipeline 创建耗时（批 9.5 启动基线；GMF 例外清单成员）
#include <fmt/format.h>
export module shader;

import rlstd;
import rlvk;
import vkerror;
import compute_access_log;  // B2-2：ComputeAccessLog（工厂在此追加访问记录；纯值模块，rlbase）
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
 * descriptor heap **槽号**的强类型包装（债-13 护栏，docs/DESCRIPTOR_HEAP_SPEC.md 坑 5：
 * `heapOffset`/`heapArrayStride` 单位是**字节**不是 slot——把槽号当字节偏移传 = 静默读错
 * 槽，VVL 一条都抓不到）。**无 uint32_t 隐式转换**：调用点必须写 `HeapSlot{slot}` 显式
 * 声明"我传的是槽号"，槽→字节的乘法只发生在换算现场（makeMappedCompute 内 /
 * shaderDetail::slotBytes）。命名同纪律：`slotXxx` 字段 = 槽、`xxxOffset` 字段 = 字节。
 * 拦住的真实错法 = 把 `slot * descriptorSize` 的乘积再喂进"槽号"参数（乘两次）。
 */
export struct HeapSlot {
	uint32_t slot;
};

/**
 * compute binding **变量名** → shader 里 `[[vk::binding(N)]]` 号（构建期接口表查表）。
 *
 * 🔴 **纯函数**：不碰 VkDevice、不读 SPV、不做运行期反射——查的是构建期产物
 * `rgl/generated/compute_bindings_generated.cppm`（compute-access 管线的第二份产物，
 * 与 compute_access.json 同源同次生成）。引擎运行期拿不到「名字→号」映射（SPV 不持有名字），
 * 所以这张表**只能**在构建期生成，运行期只是一次查表；不引入 SPIRV-Reflect / slang 运行期
 * 查询是定案（判据 → docs/compute_调研.md §7.12）。
 *
 * @param shader      shader 文件名（basename，如 "izb_create.comp.slang"，见 computeShaderKey）
 * @param bindingName shader 里的 binding 变量名（如 "head"）
 * @return 命中 = binding 号；未命中 = nullopt（**不抛**——「shader 不在表里」与「名字不在该
 *         shader 里」是两类错、消息不同，分开报归调用方，见 resolveComputeBindings）
 */
export [[nodiscard]] std::optional<uint32_t>
resolveComputeBinding(std::string_view shader, std::string_view bindingName);

/**
 * spvPath → 接口表的 shader 键：取 basename、`.spv` 换成 `.slang`。
 *
 * `generated_shaders/izb_create.comp.spv` → `izb_create.comp.slang`。路径不以 `.spv` 结尾时
 * **不猜**：派生出的键查不到 ⟹ 调用方抛"shader 不在表里"并原样点名这个键（比静默兜底成
 * 别的名字可归因）。
 */
export [[nodiscard]] std::string computeShaderKey(const std::string& spvPath);

/**
 * 把调用点按**名字**声明的映射条目解析成 (binding 号, 堆槽)——makeMappedCompute 的前置。
 *
 * 存在的理由 = 杀掉「手抄 binding 号」这条静默错：调用点原先写 `{0, HeapSlot{headSlot}}`，
 * shader 里把 `[[vk::binding(0)]]` 改成 5 时**没有任何东西会响**（无编译错、无反射校验、
 * 无 VVL 报错），只是往错的堆槽读数据。现在写 `{"head", HeapSlot{headSlot}}`，号查构建期
 * 接口表，shader 改号 C++ 不动 = 行为跟着 shader 走；shader 改名 = 启动即抛并点名。
 *
 * 🔴 **构造期红线**：本函数（以及它查的表）只准在 shader object 创建时调用。查表本身是
 * 个位数长度的线性扫，白送的每帧开销不是重点——重点是它一旦被挪进每帧路径，「创建期烘死」
 * 这个 makeMappedCompute 的语义就没了（vkCreateShadersEXT 之后映射不可改）。
 * 低成本护栏 = 本函数唯一的调用者 makeMappedCompute 返回 move-only 的 shader object 句柄，
 * 天然只出现在构造路径；没加"调用次数计数器"那类运行期断言——那要给模块加全局可变状态
 * （ECS 护栏的计数基线会因此漂），不划算。
 *
 * 🔴 与 makeMappedCompute 分开（而不是并成一个函数）= 让这三类错误在**无 GPU 无窗口**的
 * `just test` 里就能测到消息本体（tests/test_compute_bindings.cppm）。
 *
 * @param spvPath  .spv 路径（shader 身份由它推导，见 computeShaderKey）
 * @param named    (binding 变量名, 堆槽) 对列表；名字不必连续、也不必覆盖 shader 的全部 binding
 * @throw std::runtime_error 三类错误，消息各不同且都可归因：
 *                           ① shader 不在接口表里（消息列出表里有哪些 shader）
 *                           ② binding 名不在该 shader 里（点名 shader + 错名 + 全部合法名）
 *                           ③ 同一个名字传了两次（一个 binding 只能映射到一个堆槽）
 */
export [[nodiscard]] std::vector<std::pair<uint32_t, HeapSlot>> resolveComputeBindings(
	const std::string& spvPath,
	std::span<const std::pair<std::string_view, HeapSlot>> named);

/**
 * 追加 compute 访问日志并与 hazard 表对账（B2-2；`makeMappedCompute` 的构造期后半）。
 *
 * 🔴 **挂点在工厂 = 它是唯一同时知道 (shader, param → 槽) 全事实的地方**：反射只给方向
 * （`compute_bindings_generated`）、`offgraph_sync` 只给边、各模块只知道自己那几个槽，
 * 只有这里三样同时在场。挂到别处（各模块自己调）= 把「记得申报」重新变成人工的。
 *
 * 四步：① `ComputeAccessLog::add` 把本次绑定与既有记录相交，返回**新增** hazard 类（按类去重）
 * ② 每条新增查 hazard 表（`offgraph_sync`，同层 rlgfx）③ 未覆盖 → throw（与「binding 名不在
 * 接口表里」同形：构造期 fail-fast，不留运行期）④ 覆盖 → **首次确认**时打一行 info。
 *
 * 🔴 **零命令**：本函数不发任何 Vulkan 命令、不碰 barrier，纯记账 + 抛（逐位不变是硬验收）。
 * 🔴 **构造期**：只在 shader object 创建时调用；EVSSM / IZB 首帧懒构造 ⟹ 对账也在那一刻，
 * 不每帧重算（日志是 append-only、没有清空口，形态上就不给每帧路径留位置）。
 * @param spvPath .spv 路径（shader 身份由它推导，同 resolveComputeBindings）
 * @param named   调用点给的 (binding 变量名, 堆槽) 列表（方向由接口表按名字查）
 * @param log     访问日志宿主（产品侧 = `Resources::computeAccessLog()`）
 * @throw std::runtime_error 存在未被 hazard 表覆盖的访问对（消息点名两侧 + 槽号 + 表名）
 */
export void logComputeAccess(const std::string& spvPath,
	std::span<const std::pair<std::string_view, HeapSlot>> named, ComputeAccessLog& log);

namespace {
/**
 * 烘 (binding 号, 堆槽) → mapping 并建 shader object（两个公开入口的共同后半）。
 *
 * 🔴 **不导出**：公开面只有 `makeMappedCompute`（名字键，产品代码唯一入口）与
 * `makeMappedComputeByBinding`（按号，只给接口表面不到的探针 shader）。把这段留在匿名
 * 作用域，是为了让"绕过接口表直接给号"在模块外**没有第三条路**可走。
 */
[[nodiscard]] vk::raii::ShaderEXT createMappedCompute(
	vk::raii::Device& dev, const std::vector<uint32_t>& spv,
	std::span<const std::pair<uint32_t, HeapSlot>> bindingToSlot, uint32_t descriptorSize) {
	std::vector<VkDescriptorSetAndBindingMappingEXT> mappings;
	mappings.reserve(bindingToSlot.size());
	for (const auto& [binding, heapSlot] : bindingToSlot) {
		VkDescriptorSetAndBindingMappingEXT m{};
		m.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
		m.descriptorSet = 0;
		m.firstBinding = binding;
		m.bindingCount = 1;
		m.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
		m.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
		m.sourceData.constantOffset.heapOffset = heapSlot.slot * descriptorSize;  // 槽→字节换算现场（坑 5）
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

}  // namespace

/**
 * heap 映射的 compute shader object：binding→heap 槽一对一（**产品代码唯一入口**）。
 *
 * push constant 无需映射条目——descriptor heap 下 push data 区自动供给（vkCmdPushDataEXT）。
 * 原三处同款模式（evssm_shadow / izb_shadow / gpu_cull）收成本点。
 *
 * 🔴 映射表的键 = shader 里的 **binding 变量名**（不是号）。号由构建期接口表查得
 * （见 resolveComputeBindings），调用点手抄号的那条静默错就此消失。
 *
 * @param dev            逻辑设备
 * @param spvPath        .spv 路径（相对 CWD，即 build/rgl/）
 * @param bindingToSlot  (binding 变量名, 槽号) 对列表；名字不必连续，也不必覆盖 shader 全部
 *                       binding（没给映射的 binding 走驱动默认）。🔴 第二元 = **槽号**
 *                       （HeapSlot 强类型，坑 5 护栏），×descriptorSize 的换算在本函数内
 * @param descriptorSize resource heap 的槽大小（字节）
 * @param log            compute 访问日志宿主（B2-2）。给了就在**烘映射之前**追加记录并与
 *                       `offgraph_sync` 的 hazard 表对账，未覆盖即抛（构造期 fail-fast）。
 *                       缺省 nullptr = 零知识（`just test` 里的设备级探针与不需要对账的调用方）
 * @throw std::runtime_error SPV 加载失败，或 binding 名解析失败（三类，见 resolveComputeBindings），
 *                           或存在未申报的 compute hazard（见 logComputeAccess）
 */
export [[nodiscard]] vk::raii::ShaderEXT
makeMappedCompute(vk::raii::Device& dev, const std::string& spvPath,
	std::span<const std::pair<std::string_view, HeapSlot>> bindingToSlot,
	uint32_t descriptorSize, ComputeAccessLog* log = nullptr) {
	auto spvResult = loadShaderSpv(spvPath);
	if (!spvResult)
		throw std::runtime_error(spvResult.error().describe());
	const auto spv = std::move(*spvResult);
	// 名字 → 号（构造期一次，见 resolveComputeBindings 的构造期红线）
	const auto resolved = resolveComputeBindings(spvPath, bindingToSlot);
	// 🔴 hazard 对账排在 createShadersEXT **之前**：未覆盖就抛，一个句柄都不建（零命令红线）。
	//    放在之后也能抛，但那时已经付了一次 vkCreateShadersEXT，且"构造期 fail-fast"的现场
	//    会多一个刚建好就销毁的句柄——归因时容易怀疑到 mapping 上。
	if (log)
		logComputeAccess(spvPath, bindingToSlot, *log);
	return createMappedCompute(dev, spv, resolved, descriptorSize);
}

/**
 * 按 **binding 号**直填的 heap 映射 compute shader object——🔴 **产品代码不要用**。
 *
 * 存在的唯一理由：`tests/shaders/` 下那几枚驱动语义探针（dheap_*.comp.slang）不在
 * 接口表面里（接口表只反射 `rgl/shaders/`，把测试探针塞进引擎产物 = 让引擎的 binding 表
 * 跟着测试走）。它们要验的是**驱动怎么解释 mapping**，与"名字→号"无关，所以直接给号，
 * 但仍复用同一段烘映射的代码（复制那段 = 探针测的不再是引擎的烘焙路径）。
 *
 * 与 makeMappedCompute 的唯一差别 = 不查表、不校验、🔴 **也不接访问日志**（B2-2：探针 shader
 * 不在 compute-access 反射面里，接了日志只会派生出查不到方向的假 hazard）。产品代码请走名字键版。
 */
export [[nodiscard]] vk::raii::ShaderEXT
makeMappedComputeByBinding(vk::raii::Device& dev, const std::string& spvPath,
	std::span<const std::pair<uint32_t, HeapSlot>> bindingToSlot, uint32_t descriptorSize) {
	auto spvResult = loadShaderSpv(spvPath);
	if (!spvResult)
		throw std::runtime_error(spvResult.error().describe());
	return createMappedCompute(dev, *spvResult, bindingToSlot, descriptorSize);
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
	/** 可选 task stage 的码流（★M-3T-a，docs/mdi_plan.md §19.16）。
	 *  🔴 **空 = 无 task stage** ⟹ RenderPipeline 走改前形态（两条 createInfo +
	 *  `NO_TASK_SHADER_BIT` + bindShaders 第 5 槽传 null）。
	 *  🔴 `= {}` 同 `PipelineSpec::taskSpv`：本构建开 `-Werror,-Wmissing-field-initializers`，
	 *  不带 DMI 则 `ShaderEffect{mesh, frag}`（shader_hot_reload_impl.cppm）当场编不过。 */
	std::vector<uint32_t> taskStageSpv = {};
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
 *        与 write mask，VVL 07476/07478；默认 1 = 单色附件 pass）。**0 = depth-only pass**
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
	// 与 write mask）。单色附件（count=1）与旧逐位一致（历史实测）。
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
	uint32_t slotSceneHeader = 0;  // binding 22（迁移批 S-5，hasSceneData 恒带上）：头区
	                             // UBO 子范围槽（SceneDataService::headerSlot；裁未声明者同批 M）
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
	uint32_t slotSceneSsr = 0;  // binding 31（迁移批 S-3，hasSceneData 恒带上）：SSR 区
	                             // UBO 子范围槽（SceneDataService::ssrSlot；裁未声明者同批 M）
	uint32_t slotSceneTaa = 0;  // binding 30（迁移批 S-4，hasSceneData 恒带上）：TAA 区
	                             // UBO 子范围槽（SceneDataService::taaSlot；裁未声明者同批 M）
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
	uint64_t handles = 0;    // VkShaderEXT 句柄数（无 task stage 的条目 = 2 × pipelines；带 task 的
	                         // ★M-3T-a 条目 = 3，判据 = `spec.taskSpv` 非空）；**只含管线表**——
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

/** RenderPipeline 的内部拼装细节（不导出：只服务本模块的构造函数）。 */
namespace shaderDetail {
	/**
	 * heap **字节偏移**（heapOffset 终值）的强类型载体（债-13 护栏，坑 5：heapOffset
	 * 单位 = 字节）。bakeBindingMappings 的 add() 只收本类型 ⟹「忘乘 descriptorSize
	 * 直接把槽号传进 heapOffset」= 编译失败（原形态是裸 uint32_t，槽/字节同型可互换，
	 * shader_registry_fullscreen.cppm 头注释记的 15.1 sampler 偏移事故即此族）。
	 * 资源堆槽走 slotBytes()（乘法在唯一一处），sampler 堆字节偏移走显式聚合初始化
	 * ——两种单位在 add() 边界上必须各自表态。
	 */
	export struct HeapByteOffset {
		vk::DeviceSize bytes;
	};
	/** 资源堆槽号 → 字节偏移（坑 5 的「×descriptorSize」唯一合法拼写）。 */
	export inline HeapByteOffset slotBytes(uint32_t slot, uint32_t descriptorSize) {
		return {static_cast<vk::DeviceSize>(slot) * descriptorSize};
	}

	/**
	 * 拼一份 shader object 的 createInfo（★M-3T-a，docs/mdi_plan.md §19.16）。
	 *
	 * 抽出来的理由很实在：task 可选之后 stage 有 2~3 段同构拼装，各写一遍会把 RenderPipeline
	 * 构造函数顶过样式阈值（120 代码行 / 复杂度 15），而三段之间**只有 stage / nextStage /
	 * flags 三个参数不同**——参数化比复制三份更不容易漂。
	 * @param spv         本阶段 SPV 码流
	 * @param stage       本阶段位
	 * @param nextStage   后继阶段位（frag 无后继 = 0）
	 * @param flags       DESCRIPTOR_HEAP / INDIRECT_BINDABLE / NO_TASK_SHADER 的组合，由调用方定
	 * @param mappingInfo 条目级 binding mapping（各 stage **共用同一份**：映射是条目级的，不分 stage）
	 * @return C 侧 createInfo（pCode/pNext 是**借用**，调用方须保证两者活到 createShadersEXT 返回）
	 */
	inline VkShaderCreateInfoEXT makeStageCreateInfo(const std::vector<uint32_t>& spv,
		VkShaderStageFlags stage, VkShaderStageFlags nextStage,
		VkShaderCreateFlagsEXT flags, const void* mappingInfo) {
		VkShaderCreateInfoEXT info{};
		info.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
		info.pNext = mappingInfo;
		info.flags = flags;
		// createInfo 的 stage/nextStage 字段是 **FlagBits**（单值枚举）而非 Flags，故显式回落；
		// 参数收 Flags 是为了让调用点能写 0 表示"无后继阶段"（frag 那一档）。
		info.stage = static_cast<VkShaderStageFlagBits>(stage);
		info.nextStage = static_cast<VkShaderStageFlagBits>(nextStage);
		info.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
		info.codeSize = spv.size() * sizeof(uint32_t);
		info.pCode = spv.data();
		info.pName = "main";  // 全项目 shader 入口统一叫 main（Slang 侧 [shader(...)] 的函数名）
		return info;
	}

	/**
	 * **全部 stage** SPV 声明 binding 的并集（批 M，§9.3）。一个 binding 可能只在一侧声明，
	 * 如纹理/sampler 只在 frag。
	 *
	 * @return 并集；**任一 stage 解析失败 = nullopt**，调用方据此退回全烘（全烘恒正确——
	 *         「有 mapping 未声明」空转无害既证，静默滤错才是画面事故，失效方向 = 正确方向）。
	 *
	 * 🔴 ★M-3T-a 起 stage 数是 2~3（task 可选），必须**全部**进并集：漏 task 的后果不是"少烘
	 * 一条"而是反方向——将来 task 一声明 descriptor 就被滤掉 = 「声明了却无 mapping」=
	 * VUID-11292 启动炸。今天的 task 不声明任何 binding ⟹ 本改动对既有条目逐字节等价。
	 *
	 * 导出理由：本函数是**纯函数**（输入码流、输出集合，不要 VkDevice）⟹ 按分层判据必须能在
	 * `just test` 里测。用例 = `tests/test_shader_mapping.cppm`。
	 */
	export inline std::optional<std::set<uint32_t>> declaredBindingsUnion(
		const ShaderEffect& effect, std::string_view debugName) {
		// 🔴 **只把存在的 stage 放进列表**：用 nullopt 当"本条目没有这个 stage"的哨兵，会和
		// "解析失败"撞成同一个值（实测踩过：479 条全部误判成解析失败 → 全烘 + 刷屏，而终图
		// 逐位不变，A3 照样绿 ⟹ 只有 warnings 计数暴露它）。列表里没有 = 不存在，无需第三种状态。
		std::vector<std::optional<std::set<uint32_t>>> byStage;
		byStage.reserve(3);
		byStage.push_back(spvDeclaredBindings(effect.meshStageSpv));
		byStage.push_back(spvDeclaredBindings(effect.fragStageSpv));
		if (!effect.taskStageSpv.empty())
			byStage.push_back(spvDeclaredBindings(effect.taskStageSpv));
		static constexpr std::array<std::string_view, 3> kStageNames{"mesh", "frag", "task"};
		std::set<uint32_t> merged;
		for (size_t s = 0; s < byStage.size(); ++s) {
			if (byStage[s]) {
				merged.insert(byStage[s]->begin(), byStage[s]->end());
				continue;
			}
			if (auto* logger = RLLogger::getInstance())
				logger->warn("spvDeclaredBindings: 条目 '{}' 的 {} 阶段 SPV 解析失败，本条目退回全烘"
					"（批 M 过滤失效方向 = 正确方向，但启动面收益丢失）",
					debugName, kStageNames[s]);
			return std::nullopt;
		}
		return merged;
	}

	/**
	 * 烘一个条目的 descriptor heap binding mapping。
	 *
	 * ★M-3T-a 起从 RenderPipeline 构造函数里抽出来：它跟"创建 shader object"本来就是两件事
	 * （输入只有 SPV 与 binding 形态、不碰 device），混在一起会把构造函数顶过样式阈值。
	 *
	 * 顺序填充（Phase 5.5 批次 4 修正：原固定下标 [0/1/2/3/4/5] 假设 textured=true——天空盒首个
	 * textured=false+hasSceneData+hasEnv 组合下 mappings[2] 留空却计入 count → VVL
	 * "sType must be..." + envSampler 被挤出 count → "no mapping for binding 5"）⟹ 按 running
	 * index 连续填实际启用的 binding，firstBinding 显式携带。
	 * @param effect    各阶段 SPV（批 M「声明了什么」的判据来源，见 declaredBindingsUnion）
	 * @param bindings  mapping 烘焙参数（见 PipelineBindings）
	 * @param debugName 条目名（仅进 warn/throw 消息定位用，批 M：解析失败退全烘要点名条目）
	 * @param out       输出缓冲，**容量即上限**（越界 throw 点名条目，防未来 extra 列表加长时静默越界写栈）
	 * @return 实际写入的 mapping 条数
	 *
	 * 导出理由同 declaredBindingsUnion（纯函数、无设备）：批 M「烘的恰好是声明的」这条契约的
	 * 唯一可机器验收面就是本函数的返回值，用例 = `tests/test_shader_mapping.cppm`。
	 */
	export inline uint32_t bakeBindingMappings(const ShaderEffect& effect,
		const PipelineBindings& bindings, std::string_view debugName,
		std::span<VkDescriptorSetAndBindingMappingEXT> out) {
		const uint32_t descriptorSize = bindings.descriptorSize;
		const auto declared = declaredBindingsUnion(effect, debugName);
		const bool filter = declared.has_value();  // nullopt = 有 stage 解析失败 = 恒烘（= 批 M 前行为）
		uint32_t n = 0;
		// 🔴 heapOffset 参数 = HeapByteOffset 强类型（坑 5 护栏）：资源堆槽一律经
		// slotBytes()（×descriptorSize 的唯一拼写处），sampler 堆字节偏移显式包装——
		// 传裸槽号进本 lambda = 编译失败，不是 review 意见。
		const auto add = [&](uint32_t firstBinding, HeapByteOffset heapOffset,
			vk::DeviceSize arrayStride) {
			// 批 M：SPV 没声明的 binding 不烘（解析失败时 filter 关 = 恒烘，行为 = 批 M 前）
			if (filter && !declared->contains(firstBinding))
				return;
			if (n >= out.size())
				throw std::runtime_error(fmt::format(
					"RenderPipeline '{}': mapping 条目数超过输出容量 {}（binding {} 无处放）",
					debugName, out.size(), firstBinding));
			auto& m = out[n++];
			m.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
			m.descriptorSet = 0;
			m.firstBinding = firstBinding;
			m.bindingCount = 1;
			m.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
			m.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
			m.sourceData.constantOffset.heapOffset = heapOffset.bytes;
			m.sourceData.constantOffset.heapArrayStride = arrayStride;
		};

		add(0, slotBytes(bindings.slotVertex, descriptorSize), descriptorSize);
		// binding 1：非纹理形态 = 索引 slot；纹理形态 = SAMPLED_IMAGE 描述符槽（resource heap）
		add(1, slotBytes(bindings.textured ? bindings.slotTexture : bindings.slotIndex, descriptorSize),
			descriptorSize);
		if (bindings.textured) {
			// OpTypeSampler 映射：sampler heap 槽（字节偏移，共享一份 sampler 描述符）。
			// spec（descriptorheaps.adoc :1664-1665 + VUID 11253）：独立 OpTypeSampler 的
			// sampler heap 偏移写 heapOffset（"mapped resource or sampler"）；samplerHeapOffset
			// 字段仅组合 image sampler 用（:1671-1674）——2026-08-27 交叉测试定位的字段误用
			//（spike 首版同款，docs/archive/heap_texture_research.md §九.2 根因 2）
			add(2, HeapByteOffset{bindings.samplerHeapOffset}, 0);
		}
		if (bindings.hasSceneData) {
			// scene data 各载体（binding 号 = §9.5 定案表）。**hasSceneData 一律全带上、不在
			// C++ 侧登记谁用**——SPV 没声明的那条被上面的批 M 过滤自动裁掉（§9.3/§9.7；
			// 「声明了却未 mapping」才是 VUID-11292 启动炸，方向别记反）。
			// 表驱动而非逐条 add()：迁移加区只加一个条目，本函数不随区数一条条长胖。
			// 批 T1 的四张表只有 import scene_tables.slang 且真用到某张表的条目才在 SPV 里声明它
			//（未用到的被 Slang 死代码消除）⟹ 烘的恰好是用的。
			// 🔴 批 N+1：binding 3（全区 sceneData）这一条已删——scene.slang 不再声明它，
			// 留着 = 给一个不存在的 binding 烘 mapping。
			const std::pair<uint32_t, uint32_t> sceneRegions[] = {
				{22, bindings.slotSceneHeader}, {32, bindings.slotSceneWet},
				{27, bindings.slotSceneIrradiance}, {24, bindings.slotSceneLights},
				{28, bindings.slotSceneShadowViewProj}, {29, bindings.slotSceneShadowParams},
				{31, bindings.slotSceneSsr}, {30, bindings.slotSceneTaa},
				{23, bindings.slotObjectMatrices}, {25, bindings.slotObjectMVPs},
				{26, bindings.slotObjectDraws}, {33, bindings.slotObjectClusterRanges}};
			for (const auto& [regionBinding, regionSlot] : sceneRegions)
				add(regionBinding, slotBytes(regionSlot, descriptorSize), descriptorSize);
		}
		if (bindings.hasEnv) {
			// Phase 5.5：binding 4 → env cubemap（resource heap SAMPLED_IMAGE 槽）；
			// binding 5 → sky sampler（sampler heap 字节偏移，OpTypeSampler 映射规则同 binding 2）
			add(4, slotBytes(bindings.slotEnv, descriptorSize), descriptorSize);
			add(5, HeapByteOffset{bindings.envSamplerHeapOffset}, 0);
		}
		// Phase 7 D1：追加映射（light pass 采样 G-buffer 4 RT + sampler 等），running index 续 base
		for (const auto& e : bindings.extra)
			add(e.binding,
				e.sampler ? HeapByteOffset{e.samplerHeapOffset}
				          : slotBytes(e.heapSlot, descriptorSize),
				e.sampler ? 0 : descriptorSize);
		return n;
	}
}

/**
 * RenderPipeline：纯 shader object 包装（mesh + frag 两个 VkShaderEXT + 烘焙的
 * descriptor heap binding mapping；★M-3T-a 起可再带**可选的第三个** task 句柄，判据 =
 * `ShaderEffect::taskStageSpv` 非空）。动态状态不再内聚于此——见
 * RenderState / applyDynamicState。RAII：设备销毁前由本对象持有句柄。
 */
export class RenderPipeline {
	vk::raii::ShaderEXT meshShader = nullptr;
	vk::raii::ShaderEXT fragShader = nullptr;
	/** 可选 task 阶段句柄（★M-3T-a，docs/mdi_plan.md §19.16）。
	 *  🔴 `taskHasStage == false` 时**保持空且不解引用**——`taskShaderHandle()` 是唯一出口。 */
	vk::raii::ShaderEXT taskShader = nullptr;
	bool taskHasStage = false;

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
	 * hasSceneData=true（Phase 5 批次 2，模型形态）追加 scene data 各**子范围**载体槽
	 *（binding 表 = ShaderRegistry::fillSceneRegionSlots，本文件不抄；批 N+1 起**没有**
	 * 全区 binding 3 那一条了）；SPV 未声明的那条被批 M 过滤掉，不再白烘。
	 * hasEnv=true（Phase 5.5）追加 binding 4/5 → env cubemap 槽 + sky sampler 偏移
	 *（仅 import env.slang 的 frag 消费——现 = 透明 frag 一族；deferred_geometry 不采样 env
	 *（它只写 G-buffer）⟹ 不声明 = 批 M 后不烘，light pass 的 env 在本地 9/10）。
	 * extra（Phase 7 D1）续 base 追加（light pass 采 G-buffer 4 RT + sampler）。
	 * @param device 设备句柄
	 * @param effect mesh + frag 两阶段 SPV 码流
	 * @param bindings mapping 烘焙参数（见 PipelineBindings）
	 * @param debugName 条目名（仅进 warn/throw 消息定位用，批 M：解析失败退全烘要点名条目）
	 */
	RenderPipeline(vk::raii::Device& device, const ShaderEffect& effect,
		const PipelineBindings& bindings, std::string_view debugName = {}) {
		// 容量 48（迁移批 1 从 24 抬，§9.7 红线：过滤发生在计数之后，容量必须盖过「全烘」
		// 的最坏条目数 + 后续批 +12 binding 的扩张面）：model 条目已用 14（base 0-5 +
		// binding 32 + extra 6/7/8/12/13/15/16），meshlet 条目再 +5 个簇 buffer（17-21），
		// 批 T1 再 +4 张逐对象表（23/25/26/33）。上限判据 = 最多 extra 的条目，别贴边。
		// 批 M 过滤只让**实际烘焙数**减不让它增；越界保护在 bakeBindingMappings 内 throw
		// 点名条目，防的是未来 extra 列表继续加长时静默越界写栈。
		VkDescriptorSetAndBindingMappingEXT mappings[48]{};
		const uint32_t mappingCount = shaderDetail::bakeBindingMappings(effect, bindings, debugName, mappings);

		VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{};
		mappingInfo.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT;
		mappingInfo.mappingCount = mappingCount;
		mappingInfo.pMappings = mappings;

		// ★M-3T-a（docs/mdi_plan.md §19.16）：可选 task stage。**空 = 完全走改前形态**（两条
		// createInfo + NO_TASK_SHADER_BIT + bindShaders 第 5 槽 null），其余条目一字节不动。
		const bool hasTask = !effect.taskStageSpv.empty();

		// DESCRIPTOR_HEAP + INDIRECT_BINDABLE 三段同值（后者：进 DGC execution set 的 shader
		// 必须带，VUID-11032/11154；非 DGC 路无害，统一加免得管线表再分两类）
		constexpr VkShaderCreateFlagsEXT kStageFlags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT
			| VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT;
		// 🔴 `NO_TASK_SHADER_BIT` **只在没有 task stage 时带**。该 bit 的语义是"本 mesh 不存在配对的
		// task"，带上了还往 bindShaders 的 task 槽塞实句柄 = VUID-VkShaderCreateInfoEXT-flags-08695
		// 的反向违例。**反过来漏带**的历史代价 = NVIDIA Xid 13 engine reset（mesh 不带该 bit 而 task
		// 绑 null → 驱动查空指针，docs/nvidia_compatibility_report.md §十）。两个方向都在此钉住。
		const VkShaderCreateFlagsEXT meshFlags = kStageFlags
			| (hasTask ? VkShaderCreateFlagsEXT(0) : VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT);

		// 数组序 = 返回句柄的下标序：task(可选) → mesh → frag。🔴 与 nextStage 链**无关**
		//（链是 stage 属性：task.nextStage=MESH、mesh.nextStage=FRAG、frag 无后继 = 0）。
		std::vector<vk::ShaderCreateInfoEXT> createInfos;
		createInfos.reserve(hasTask ? 3 : 2);
		if (hasTask)
			createInfos.push_back(vk::ShaderCreateInfoEXT(shaderDetail::makeStageCreateInfo(
				effect.taskStageSpv, VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT,
				kStageFlags, &mappingInfo)));
		createInfos.push_back(vk::ShaderCreateInfoEXT(shaderDetail::makeStageCreateInfo(
			effect.meshStageSpv, VK_SHADER_STAGE_MESH_BIT_EXT, VK_SHADER_STAGE_FRAGMENT_BIT,
			meshFlags, &mappingInfo)));
		createInfos.push_back(vk::ShaderCreateInfoEXT(shaderDetail::makeStageCreateInfo(
			effect.fragStageSpv, VK_SHADER_STAGE_FRAGMENT_BIT, 0, kStageFlags, &mappingInfo)));

		// 批 9.5 观测：条目/句柄/mapping 计数 + vkCreateShadersEXT wall clock（口径见
		// ShaderObjectStats 注释）。纯计数，不改任何创建参数。
		const auto createBegin = std::chrono::steady_clock::now();
		auto shaders = device.createShadersEXT(createInfos);
		auto& stats = shaderStatsDetail::mutableStats();
		++stats.pipelines;
		stats.handles += createInfos.size();  // mesh + frag（+ 可选 task）；本函数唯一句柄产出点
		stats.mappings += mappingCount;
		stats.createNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - createBegin).count());
		if (hasTask) {
			taskShader = std::move(shaders[0]);
			taskHasStage = true;
			meshShader = std::move(shaders[1]);
			fragShader = std::move(shaders[2]);
		} else {
			meshShader = std::move(shaders[0]);
			fragShader = std::move(shaders[1]);
		}
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
			taskShaderHandle(),  // ★M-3T-a：无 task stage 的条目恒 = VK_NULL_HANDLE（= 改前形态）
			vk::ShaderEXT(*meshShader),
			vk::ShaderEXT(*fragShader)};

		cmd.bindShadersEXT(stages, shaderHandles);
	}
	/** @return task 阶段 shader 句柄；**无 task stage 的条目 = VK_NULL_HANDLE**（★M-3T-a）。
	 *  bindShaders 的第 5 槽（`eTaskEXT`）只经本函数取值 ⟹ "有没有 task"只有一个判定点，
	 *  不会出现"句柄存在但 flags 带了 NO_TASK"那种两侧不同向的形态。 */
	[[nodiscard]] vk::ShaderEXT taskShaderHandle() const {
		return taskHasStage ? vk::ShaderEXT(*taskShader) : vk::ShaderEXT(nullptr);
	}
	/** @return mesh 阶段 shader 句柄（DGC execution set 一次性写入，Phase 7b；inactive 路不消费）。 */
	[[nodiscard]] vk::ShaderEXT meshShaderHandle() const { return *meshShader; }
	/** @return frag 阶段 shader 句柄（DGC execution set 一次性写入，Phase 7b；inactive 路不消费）。 */
	[[nodiscard]] vk::ShaderEXT fragShaderHandle() const { return *fragShader; }
};

}  // namespace RGL