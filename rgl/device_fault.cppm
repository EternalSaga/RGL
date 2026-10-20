export module device_fault;

import rlstd;
import rlvk;

/**
 * GPU 故障（device fault）诊断通路 —— **纯旁路**，不改任何提交语义。
 *
 * 动机（2026-10-20 MDI 阴影段偶发 GPU hang 归因）：journalctl 只给 `ring gfx_0.0.0 timeout`
 * = CP 超时，不给"GPU 当时在访问哪个地址 / 执行哪条指令"。本模块把 `VK_KHR_device_fault`
 * （回落 `VK_EXT_device_fault`）接进来，在 device lost 之后抓一次报告并逐条打日志。
 *
 * ## 🔴 空报告是**有范围的负读数**，不是"无 fault"
 * "报告为空"必须**明打**（静默 = 把"查了没有"读成"没查"），但它只说明**驱动可见面**没有
 * fault，**不**说明没有 fault。本项目的实测就是这条区分的学费：device_fault 八次报空，
 * 而内核 devcoredump 照样抓到 `[gfxhub] Page fault observed` + fault 地址恰 = 0x0（债-140）
 * ——fault 存在，只是没进驱动的 fault ring。⟹ 这条读数的正确用法是把嫌疑**收窄**（从"驱动
 * 可见的 GPUVM 越界"收到"驱动不报的那类"），**不是**"排除越界"。
 * 三种打法都把这次归因做废：静默、打成"查询失败"、打成"无 fault"。
 *
 * ## 两后端的差别（决定为什么 KHR 优先）
 *  · `VK_EXT_device_fault`（rev 2）：`vkGetDeviceFaultInfoEXT` **只能在 device 已 lost 后**查，
 *    两段式（先取 counts 再取数据），报告形态 = 一条 description + 地址数组 + 厂商数组。
 *  · `VK_KHR_device_fault`（rev 1）：`vkGetDeviceFaultReportsKHR(device, timeout, ...)`
 *    **可任意时刻调用**、timeout=0 = 不阻塞轮询、且 `deviceFaultReportMasked` 位能让驱动把
 *    **它自己内部恢复掉、没上报 device lost 的 masked fault** 也吐出来。
 *    ⟹ 对本项目"偶发但没炸"的 GPU 侧问题，masked 上报才是有肉的半边（本机 RADV 实测该位 = true）。
 * 两后端的报告结构不同，本模块先归一成 `FaultReportView`（纯数据），打印与翻译只写一份。
 *
 * ## 生命周期红线（AGENTS「CreateInfo 的 pNext 引用的结构必须是类成员」）
 * feature 结构（`vk::PhysicalDeviceFaultFeaturesKHR/EXT`）由 `DeviceContext` 持成员，
 * `createDevice` 按支持位条件挂进 required 链尾；本模块只**读**探测结果，不持有链。
 *
 * ## 不进关键路径
 * device lost 抓取 = 只在退出路径跑一次；轮询 = `diagnostics.fault_poll` 门（默认 false）且
 * 只在 KHR + masked 已启用时干活。不支持 = 整模块 no-op（不报错不 warn，INFO 一行记探测结果）。
 */
namespace RGL {

/** 探测到的后端形态。`none` = 驱动没这两个扩展（或 feature 位为假）⟹ 整模块 no-op。 */
export enum class DeviceFaultBackend : uint8_t { none, khr, ext };

/** 后端 → 白话（INFO 行用它，读者不用去猜 khr/ext 差在哪）。 */
export std::string_view describeDeviceFaultBackend(DeviceFaultBackend backend);

/**
 * 本 device 的 fault 通路事实（`DeviceContext` 构造期算一次，之后只读）。
 * @field backend 走哪条路（KHR 优先，回落 EXT）
 * @field reportMasked `deviceFaultReportMasked` **已启用**（不是"驱动支持"——启用受
 *         `diagnostics.fault_poll` 门控，理由见 `deriveDeviceFaultSupport` 的注释）
 * @field maxFaultCount 一次能取回几条报告（`VkPhysicalDeviceFaultPropertiesKHR`；本机 = 1）
 */
export struct DeviceFaultSupport {
	DeviceFaultBackend backend = DeviceFaultBackend::none;
	bool reportMasked = false;
	uint32_t maxFaultCount = 0;
	/** 通路是否可用（false = 两个调用点都直接返回，零 Vulkan 调用）。 */
	[[nodiscard]] bool active() const { return backend != DeviceFaultBackend::none; }
};

/**
 * 🔴 **后端优先级的唯一落点**：KHR 命中就用 KHR，否则才看 EXT；两边都命中也只选 KHR。
 *
 * 为什么必须只有一处：扩展启用（features.cppm 的 `getDeviceExtensions`）、feature 结构链接
 * （vkdevice 的条件链）、实际调用哪个入口（本模块两个出口）三处必须选同一条路。选歪的形态
 * 是"启用了 EXT 而代码走 KHR 入口"= 在未启用的扩展上调它的入口点，VVL 未必抓得到而驱动行为
 * 未定义。三处都调本函数，谁都不自己写 if。
 * @param khrAvailable KHR 扩展可用且其 `deviceFault` feature 位为真
 * @param extAvailable EXT 回落路径可用（KHR 未命中时才生效）
 */
export DeviceFaultBackend chooseDeviceFaultBackend(bool khrAvailable, bool extAvailable);

/**
 * 由 device 创建期的支持位派生后端选择 + 属性读数。
 *
 * 🔴 `maskedEnabled` 传的是**门控后的**结果（`diagnostics.fault_poll`），不是"驱动支持"：
 * 按 spec，启用 `deviceFaultReportMasked` 后驱动可以**内部恢复**故障而不再向应用报
 * device lost——默认开 = 可能把我们要查的那个 hang 变成"什么都没发生"。门开 = 用户明确
 * 要这份数据，才承担这个语义变化。
 * @param physicalDevice 用于查 `maxDeviceFaultCount`（KHR 后端才查；EXT 无此属性）
 * @param khrEnabled KHR 扩展可用且 `deviceFault` 位为真
 * @param extEnabled EXT 回落路径可用（KHR 未命中时才生效）
 * @param maskedEnabled 已按门控决定要启用 masked 上报
 */
export DeviceFaultSupport deriveDeviceFaultSupport(const vk::raii::PhysicalDevice& physicalDevice,
	bool khrEnabled, bool extEnabled, bool maskedEnabled);

/**
 * `addressType` 归一分类（翻译表的输入侧）。
 * 🔴 表外的值落 `unrecognized` 而**不是** `none`——`none` 是驱动明确说"这条不含地址信息"，
 * 与"我们读不懂这个数"是两件事，混成一条会让未来的新枚举值静默变成一条假负读数。
 */
export enum class FaultAddressKind : uint8_t {
	none,                  ///< 驱动明确说：这条报告不含页故障也不含指令指针
	readInvalid,           ///< 非法读
	writeInvalid,          ///< 非法写
	executeInvalid,        ///< 试图执行不可执行内存
	ipUnknown,             ///< 故障发生时的指令指针（未必与故障本身相关）
	ipInvalid,             ///< 非法指令对应的指令指针
	ipFault,               ///< 与故障直接相关的指令指针
	unrecognized,          ///< 翻译表外的原始值（驱动/规格比本文件新）
};

/** 原始 `VkDeviceFaultAddressTypeKHR` 值 → 分类（EXT 是同一组值的别名，共用）。 */
export FaultAddressKind classifyFaultAddressType(uint32_t rawAddressType);

/** 分类 → 白话（ERROR 行的正文）。 */
export std::string_view describeFaultAddressKind(FaultAddressKind kind);

/** KHR `VkDeviceFaultInfoKHR::flags` → 白话位清单（`watchdog 超时` 那条正是 hang 的签名）。 */
export std::string describeFaultFlags(uint32_t flags);

/**
 * `reportedAddress` + `addressPrecision` → 可读地址区间。
 * spec 给的闭式：`lower = reported & ~(precision-1)`、`upper = reported | (precision-1)`；
 * precision ≤ 1 = 精确地址（不做区间，免得读者把"精确"读成"上下界相同是巧合"）。
 */
export std::string describeFaultAddress(uint64_t reportedAddress, uint64_t addressPrecision);

/**
 * 两后端归一后的一条报告（纯数据 = 可单测；0/空串 = 该字段驱动没填）。
 * EXT 后端没有 flags/groupId（那条路只有 description + 地址数组 + 厂商数组），留 0。
 */
export struct FaultReportView {
	uint32_t flags = 0;                     ///< KHR 专属：`VkDeviceFaultFlagsKHR`
	uint64_t groupId = 0;                   ///< KHR 专属：同一故障的多条报告共享的组号
	std::string description;                ///< 驱动给的人类可读描述（两后端都有）
	FaultAddressKind faultKind = FaultAddressKind::none;
	uint64_t faultAddress = 0;
	uint64_t faultAddressPrecision = 0;
	FaultAddressKind instructionKind = FaultAddressKind::none;
	uint64_t instructionAddress = 0;
	uint64_t instructionAddressPrecision = 0;
	std::string vendorDescription;          ///< 厂商描述串
	uint64_t vendorFaultCode = 0;
	uint64_t vendorFaultData = 0;
};

/** 一条报告 → 单行可读文本（地址按区间、类型按白话、厂商码十六进制）。 */
export std::string formatFaultReport(const FaultReportView& report);

/**
 * 空批次的读数（🔴 本模块最重要的一条**负读数**，措辞口径 2026-10-20 被实测改过一次）。
 *
 * 它说的是"**驱动可见面**无 fault"，并且必须自带一句"这不等于「无 fault」"——因为本项目的
 * 八次 hang 里本模块恒报空，而内核 devcoredump 抓到了 `[gfxhub] Page fault observed`
 * @ VA 0x0（债-140）。旧措辞「本次是纯 hang 无 fault」把这条读用成了"排除越界"，方向是错的。
 *
 * 与"查询失败"那条出口（措辞含『无读数』）严格分开：那条连驱动可见面都没读到，本条读到了、
 * 是空的。两者对归因的含义不同，混同 = 把"驱动不报的那类"又读回"没 fault"。
 */
export std::string describeEmptyFaultBatch(DeviceFaultBackend backend);

/**
 * EXT 两段式第二段的**入参容量**（纯函数 = 无设备可单测；真实调用点只做搬运）。
 *
 * 为什么值得单独成一个函数：第二段的三个入参不是"我要几条"而是"我的数组有多大"，且
 * `vendorBinarySize` 在未启用 `deviceFaultVendorBinary` 时**必须传 0**——传非 0 时驱动仍会
 * 置 0 且不碰缓冲区，于是"我给了它没写"与"它写满了"在日志里长得一样。
 */
export struct ExtFaultCapacity {
	uint32_t addressInfoCount = 0;   ///< 地址数组容量（回写 = 实际条数）
	uint32_t vendorInfoCount = 0;    ///< 厂商数组容量
	uint64_t vendorBinarySize = 0;   ///< 厂商 dump 缓冲区字节数（未启用该 feature = 恒 0）
};

/** 由第一段的 counts 规划第二段入参容量（规则见 `ExtFaultCapacity` 注释）。 */
export ExtFaultCapacity planExtFaultCapacity(uint32_t addressInfoCount, uint32_t vendorInfoCount,
	bool vendorBinaryEnabled, uint64_t reportedBinarySize);

/**
 * device lost 之后抓一次 fault 报告并逐条打日志（**只在退出路径调用**）。
 *
 * 三条出口，每条都出声：① 通路未启用 → INFO 点名"没探测到扩展"；② 抓到 → 每条一版 ERROR
 * （`formatFaultReport`）；③ **报告为空** → ERROR 明打"驱动可见面无 fault（≠ 无 fault）"。
 * @param device 已 lost 的 device（EXT 路径要求 device 已 lost；KHR 路径任意时刻可调用）
 * @param support `DeviceContext::getDeviceFaultSupport()`
 * @param context 调用点定位串（例如 "VK_ERROR_DEVICE_LOST on submit"），进日志正文
 */
export void reportDeviceFaultOnLost(const vk::raii::Device& device,
	const DeviceFaultSupport& support, std::string_view context);

/**
 * 轮询一次 masked fault（`diagnostics.fault_poll` 门内调用；timeout=0 = 不阻塞）。
 *
 * 只在 KHR 后端且 `reportMasked` 已启用时干活，其余形态**零 Vulkan 调用**直接返回 0。
 * 抓到即 ERROR 点名并返回条数；没抓到不打日志（每帧打一行"没有故障"会把日志冲干净，
 * 而"没有"这件事由启动期那行 INFO 的探测结果 + 退出时的汇总承担）。
 * @return 本次取回的报告条数（这些报告**已被消费**：spec 规定每条只返回一次）
 */
export uint32_t pollDeviceFaults(const vk::raii::Device& device, const DeviceFaultSupport& support);

/** 退出时打一行轮询账（抓到过多少条、调用过多少次）；从未启用 = 不出声。 */
export void reportDeviceFaultPollSummary();

}  // namespace RGL
