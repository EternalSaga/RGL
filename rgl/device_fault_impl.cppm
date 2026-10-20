module;
#include <vulkan/vulkan.h>  // VK_DEVICE_FAULT_* 宏（宏不随模块导出；vkerror/features 先例）
#include <fmt/format.h>     // 地址/码的十六进制拼装（GMF 只放第三方头；先例 = render_list:mdi）
module device_fault:impl;

import device_fault;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;

namespace RGL {

namespace {

/** 定长 char 数组 → string（驱动的 description 不保证 NUL 结尾，不能直接当 C 串用）。 */
std::string fixedString(const char* text, size_t capacity) {
	size_t n = 0;
	for (; n < capacity && text[n] != '\0'; ++n)
		;
	return std::string(text, n);
}

/** 字节数量级白话（地址精度列用）。 */
std::string describeSpan(uint64_t bytes) {
	if (bytes >= (1ull << 20) && bytes % (1ull << 20) == 0)
		return fmt::format("{}MB", bytes / (1ull << 20));
	if (bytes >= 1024ull && bytes % 1024ull == 0)
		return fmt::format("{}KB", bytes / 1024ull);
	return fmt::format("{}B", bytes);
}

/** 轮询账（进程级；只有主线程的帧循环会调 pollDeviceFaults，无并发）。 */
struct PollStats {
	uint64_t calls = 0;   ///< 轮询调用次数（门开才计数，门关时连这里都不动）
	uint64_t masked = 0;  ///< 抓到的 masked fault 条数
};
/** 轮询账的可变单例（函数内 static = 无静态初始化顺序问题；退出时由 reportDeviceFaultPollSummary 读）。 */
PollStats& pollStats() {
	static PollStats stats;
	return stats;
}

}  // namespace

std::string_view describeDeviceFaultBackend(DeviceFaultBackend backend) {
	switch (backend) {
	case DeviceFaultBackend::none:
		return "未启用（驱动无 VK_KHR_device_fault / VK_EXT_device_fault，或 deviceFault 位为假）";
	case DeviceFaultBackend::khr:
		return "VK_KHR_device_fault（可任意时刻查询 + masked 上报）";
	case DeviceFaultBackend::ext:
		return "VK_EXT_device_fault（仅 device lost 后可查，两段式）";
	}
	// 穷尽 switch 不写 default（-Wcovered-switch-default）：新增枚举值时编译器点名这里。
	// 下面这行只在"枚举值来自表外"（内存被写坏 / 驱动给了新数）时可达。
	return "未识别的后端枚举值";
}

DeviceFaultBackend chooseDeviceFaultBackend(bool khrAvailable, bool extAvailable) {
	if (khrAvailable)
		return DeviceFaultBackend::khr;
	if (extAvailable)
		return DeviceFaultBackend::ext;
	return DeviceFaultBackend::none;
}

DeviceFaultSupport deriveDeviceFaultSupport(const vk::raii::PhysicalDevice& physicalDevice,
	bool khrEnabled, bool extEnabled, bool maskedEnabled) {
	DeviceFaultSupport support;
	support.backend = chooseDeviceFaultBackend(khrEnabled, extEnabled);
	if (support.backend == DeviceFaultBackend::none)
		return support;  // none：后面一次 Vulkan 调用都不发
	support.reportMasked = maskedEnabled && support.backend == DeviceFaultBackend::khr;
	if (support.backend == DeviceFaultBackend::khr) {
		// 只在 KHR 已启用时挂这个属性结构：挂未启用扩展的结构进 pNext 链时驱动可忽略，
		// 忽略后读到的是零值，会被当成"设备说最多 0 条"⟹ 轮询永不取数据且不出声。
		const auto props = physicalDevice
		    .getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceFaultPropertiesKHR>();
		support.maxFaultCount = props.get<vk::PhysicalDeviceFaultPropertiesKHR>().maxDeviceFaultCount;
	}
	return support;
}

FaultAddressKind classifyFaultAddressType(uint32_t rawAddressType) {
	// 值表来自 VkDeviceFaultAddressTypeKHR（EXT 是同一组值的别名 ⟹ 两后端共用本表）。
	// 🔴 表外值落 unrecognized 而不是 none：none 是驱动明确说"这条不含地址信息"，与
	// "我们读不懂这个数"是两件事，混成一条会让未来的新枚举值静默变成一条假负读数。
	switch (rawAddressType) {
	case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_KHR:
		return FaultAddressKind::none;
	case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_KHR:
		return FaultAddressKind::readInvalid;
	case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_KHR:
		return FaultAddressKind::writeInvalid;
	case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_KHR:
		return FaultAddressKind::executeInvalid;
	case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_KHR:
		return FaultAddressKind::ipUnknown;
	case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_KHR:
		return FaultAddressKind::ipInvalid;
	case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_KHR:
		return FaultAddressKind::ipFault;
	default:
		break;  // 规格比本表新（本函数入参是 uint32，编译器无法穷尽，这里必须留兜底）
	}
	return FaultAddressKind::unrecognized;
}

std::string_view describeFaultAddressKind(FaultAddressKind kind) {
	switch (kind) {
	case FaultAddressKind::none:
		return "无地址信息（驱动明确说这条不含页故障也不含指令指针）";
	case FaultAddressKind::readInvalid:
		return "非法读（GPU 读了一个无效地址）";
	case FaultAddressKind::writeInvalid:
		return "非法写（GPU 写了一个无效地址）";
	case FaultAddressKind::executeInvalid:
		return "执行不可执行内存";
	case FaultAddressKind::ipUnknown:
		return "故障发生时的指令指针（未必与故障本身相关）";
	case FaultAddressKind::ipInvalid:
		return "非法指令的指令指针";
	case FaultAddressKind::ipFault:
		return "与故障直接相关的指令指针";
	case FaultAddressKind::unrecognized:
		return "未识别的 addressType 原始值（驱动或规格比本翻译表新，别当成『无地址信息』）";
	}
	return "未识别的分类枚举值";
}

std::string describeFaultFlags(uint32_t flags) {
	// 逐位翻（顺序 = 位序），命中才拼。表外的位**必须出声**：静默丢掉一个位 = 把"驱动给了
	// 新信息"读成"驱动没给"。
	constexpr uint32_t kKnownBits = VK_DEVICE_FAULT_FLAG_DEVICE_LOST_KHR
	    | VK_DEVICE_FAULT_FLAG_MEMORY_ADDRESS_KHR | VK_DEVICE_FAULT_FLAG_INSTRUCTION_ADDRESS_KHR
	    | VK_DEVICE_FAULT_FLAG_VENDOR_KHR | VK_DEVICE_FAULT_FLAG_WATCHDOG_TIMEOUT_KHR
	    | VK_DEVICE_FAULT_FLAG_OVERFLOW_KHR;
	const std::pair<uint32_t, std::string_view> table[] = {
		{VK_DEVICE_FAULT_FLAG_DEVICE_LOST_KHR, "导致 device lost"},
		{VK_DEVICE_FAULT_FLAG_MEMORY_ADDRESS_KHR, "含内存地址"},
		{VK_DEVICE_FAULT_FLAG_INSTRUCTION_ADDRESS_KHR, "含指令地址"},
		{VK_DEVICE_FAULT_FLAG_VENDOR_KHR, "含厂商信息"},
		{VK_DEVICE_FAULT_FLAG_WATCHDOG_TIMEOUT_KHR, "watchdog 超时（hang 的签名）"},
		{VK_DEVICE_FAULT_FLAG_OVERFLOW_KHR, "报告溢出（还有没取回的）"},
	};
	std::string out;
	for (const auto& [bit, text] : table)
		if ((flags & bit) != 0u)
			out += (out.empty() ? std::string() : std::string(", ")) + std::string(text);
	if ((flags & ~kKnownBits) != 0u)
		out += (out.empty() ? std::string() : std::string(", "))
		    + fmt::format("未识别的 flag 位 {:#x}", flags & ~kKnownBits);
	return out.empty() ? "无（flags = 0）" : out;
}

std::string describeFaultAddress(uint64_t reportedAddress, uint64_t addressPrecision) {
	// spec 闭式：lower = reported & ~(precision-1)、upper = reported | (precision-1)。
	// precision ≤ 1 时该式退化（=0 更会把下界算成 0），单独走"精确"分支。
	if (addressPrecision <= 1)
		return fmt::format("{:#x}（精确地址）", reportedAddress);
	const uint64_t lower = reportedAddress & ~(addressPrecision - 1);
	const uint64_t upper = reportedAddress | (addressPrecision - 1);
	return fmt::format("{:#x}–{:#x}（非精确，精度 {}）", lower, upper, describeSpan(addressPrecision));
}

std::string formatFaultReport(const FaultReportView& report) {
	std::string line = fmt::format("fault: {}",
	    report.description.empty() ? std::string("(驱动未给描述)") : report.description);
	if (report.flags != 0u)
		line += fmt::format(" | flags={} [{}]", fmt::format("{:#08x}", report.flags),
		    describeFaultFlags(report.flags));
	if (report.groupId != 0u)
		line += fmt::format(" | group={}", report.groupId);
	if (report.faultKind != FaultAddressKind::none)
		line += fmt::format(" | 访存={} 地址={}", describeFaultAddressKind(report.faultKind),
		    describeFaultAddress(report.faultAddress, report.faultAddressPrecision));
	if (report.instructionKind != FaultAddressKind::none)
		line += fmt::format(" | 取指={} 地址={}", describeFaultAddressKind(report.instructionKind),
		    describeFaultAddress(report.instructionAddress, report.instructionAddressPrecision));
	if (!report.vendorDescription.empty() || report.vendorFaultCode != 0u
	    || report.vendorFaultData != 0u)
		line += fmt::format(" | 厂商: {} code={:#x} data={:#x}",
		    report.vendorDescription.empty() ? std::string("(无描述)") : report.vendorDescription,
		    report.vendorFaultCode, report.vendorFaultData);
	return line;
}

std::string describeEmptyFaultBatch(DeviceFaultBackend backend) {
	return fmt::format(
	    "device fault 报告为空 = **驱动可见面**无 fault（后端={}）。⚠ 这不等于「无 fault」——"
	    "债-140 的 devcoredump 实测：驱动报空的那类 hang，内核层照样能看到 page fault"
	    "（fault @ VA 0x0）。下一步 = 抓 devcoredump（root 可读，见债-140 的取证段），"
	    "不要转去改渲染路径『试试看』",
	    describeDeviceFaultBackend(backend));
}

ExtFaultCapacity planExtFaultCapacity(uint32_t addressInfoCount, uint32_t vendorInfoCount,
	bool vendorBinaryEnabled, uint64_t reportedBinarySize) {
	return ExtFaultCapacity {
		.addressInfoCount = addressInfoCount,
		.vendorInfoCount = vendorInfoCount,
		// 未启用该 feature 时**必须**传 0（规则本体见接口 ExtFaultCapacity 注释）
		.vendorBinarySize = vendorBinaryEnabled ? reportedBinarySize : 0ull,
	};
}

namespace {

/** KHR 后端：取回当前可用的报告（timeoutNs=0 = 不阻塞）。返回 unexpected = 调用本身失败。 */
std::expected<std::vector<FaultReportView>, std::string> collectKhr(
	const vk::raii::Device& device, uint64_t timeoutNs) {
	auto [result, reports] = device.getFaultReportsKHR(timeoutNs);
	if (result != vk::Result::eSuccess && result != vk::Result::eIncomplete
	    && result != vk::Result::eTimeout)
		return std::unexpected(
		    fmt::format("vkGetDeviceFaultReportsKHR 返回 {}（查询本身失败，非『无故障』）",
		        vk::to_string(result)));
	std::vector<FaultReportView> out;
	out.reserve(reports.size());
	for (const auto& r : reports) {
		FaultReportView v;
		v.flags = static_cast<uint32_t>(r.flags);
		v.groupId = r.groupId;
		v.description = fixedString(r.description, sizeof(r.description));
		v.faultKind = classifyFaultAddressType(static_cast<uint32_t>(r.faultAddressInfo.addressType));
		v.faultAddress = r.faultAddressInfo.reportedAddress;
		v.faultAddressPrecision = r.faultAddressInfo.addressPrecision;
		v.instructionKind =
		    classifyFaultAddressType(static_cast<uint32_t>(r.instructionAddressInfo.addressType));
		v.instructionAddress = r.instructionAddressInfo.reportedAddress;
		v.instructionAddressPrecision = r.instructionAddressInfo.addressPrecision;
		v.vendorDescription = fixedString(r.vendorInfo.description, sizeof(r.vendorInfo.description));
		v.vendorFaultCode = r.vendorInfo.vendorFaultCode;
		v.vendorFaultData = r.vendorInfo.vendorFaultData;
		out.push_back(std::move(v));
	}
	return out;
}

/**
 * EXT 后端：两段式（第一段 pFaultInfo=nullptr 拿 counts，第二段按 counts 分配再取）。
 *
 * 一条 EXT 报告 = 一个 description + N 个地址 + M 条厂商信息；本函数把它**摊平**成
 * max(1, N+M) 条 `FaultReportView`（地址/厂商逐条一行才读得动，description 只给第一条，
 * 其余由 formatFaultReport 打成"(驱动未给描述)"）。N=M=0 但 description 非空时保留那一条。
 */
std::expected<std::vector<FaultReportView>, std::string> collectExt(const vk::raii::Device& device) {
	vk::DeviceFaultCountsEXT counts{};
	counts.sType = vk::StructureType::eDeviceFaultCountsEXT;
	if (auto r = device.getFaultInfoEXT(&counts, nullptr); r != vk::Result::eSuccess)
		return std::unexpected(fmt::format(
		    "vkGetDeviceFaultInfoEXT 第一段（取 counts）返回 {}", vk::to_string(r)));

	std::vector<vk::DeviceFaultAddressInfoKHR> addrs(counts.addressInfoCount);
	std::vector<vk::DeviceFaultVendorInfoKHR> vendors(counts.vendorInfoCount);
	const auto cap = planExtFaultCapacity(counts.addressInfoCount, counts.vendorInfoCount,
	    /*vendorBinaryEnabled = */ false, counts.vendorBinarySize);
	vk::DeviceFaultInfoEXT info{};
	info.sType = vk::StructureType::eDeviceFaultInfoEXT;
	info.pAddressInfos = addrs.empty() ? nullptr : addrs.data();
	info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
	info.pVendorBinaryData = nullptr;  // 未启用 deviceFaultVendorBinary（本机 RADV 实测该位 false）
	counts.addressInfoCount = cap.addressInfoCount;
	counts.vendorInfoCount = cap.vendorInfoCount;
	counts.vendorBinarySize = cap.vendorBinarySize;
	if (auto r = device.getFaultInfoEXT(&counts, &info); r != vk::Result::eSuccess)
		return std::unexpected(fmt::format(
		    "vkGetDeviceFaultInfoEXT 第二段（取数据）返回 {}", vk::to_string(r)));

	const std::string description = fixedString(info.description, sizeof(info.description));
	std::vector<FaultReportView> out;
	for (uint32_t i = 0; i < counts.addressInfoCount; ++i) {
		FaultReportView one;
		if (i == 0u)
			one.description = description;
		one.faultKind = classifyFaultAddressType(static_cast<uint32_t>(addrs[i].addressType));
		one.faultAddress = addrs[i].reportedAddress;
		one.faultAddressPrecision = addrs[i].addressPrecision;
		out.push_back(std::move(one));
	}
	for (uint32_t i = 0; i < counts.vendorInfoCount; ++i) {
		FaultReportView one;
		if (out.empty())
			one.description = description;
		one.vendorDescription = fixedString(vendors[i].description, sizeof(vendors[i].description));
		one.vendorFaultCode = vendors[i].vendorFaultCode;
		one.vendorFaultData = vendors[i].vendorFaultData;
		out.push_back(std::move(one));
	}
	if (out.empty() && !description.empty())
		out.push_back(FaultReportView { .description = description });
	return out;
}

}  // namespace

void reportDeviceFaultOnLost(const vk::raii::Device& device, const DeviceFaultSupport& support,
	std::string_view context) {
	auto* logger = RLLogger::getInstance();
	if (!support.active()) {
		// 出口 ①：通路未启用。INFO 不出 ERROR——没这个能力不是错误，但必须留一行，
		// 否则"日志里没有 fault 段"会被读成"有通路但没抓到"。
		if (logger)
			logger->info("device fault: 通路未启用（本次 hang 的 fault 侧无读数）—— 后端={}；"
			             "触发点={}",
			    describeDeviceFaultBackend(support.backend), context);
		return;
	}
	auto collected =
	    support.backend == DeviceFaultBackend::khr ? collectKhr(device, 0) : collectExt(device);
	if (!collected) {
		// 出口 ②：查询失败。措辞必须与"空报告"相反——这里**不能**得出"纯 hang"的结论。
		if (logger)
			logger->error("device fault: 查询失败（{}）—— fault 侧**无读数**，不可据此判定"
			              "『没有 fault』: {}",
			    context, collected.error());
		return;
	}
	if (collected->empty()) {
		// 出口 ③：查到了，是空的 = 纯 hang。
		if (logger)
			logger->error("device fault: {} —— {}", context, describeEmptyFaultBatch(support.backend));
		return;
	}
	for (const auto& report : *collected)
		if (logger)
			logger->error("device fault: {} —— {}", context, formatFaultReport(report));
}

uint32_t pollDeviceFaults(const vk::raii::Device& device, const DeviceFaultSupport& support) {
	// 门关 / 非 KHR / masked 未启用 = 零 Vulkan 调用直接返回（诊断旁路的"零成本"半边）。
	if (!support.active() || !support.reportMasked)
		return 0;
	pollStats().calls++;
	auto collected = collectKhr(device, /*timeoutNs = */ 0);
	if (!collected) {
		if (auto* logger = RLLogger::getInstance())
			logger->error("device fault 轮询: 查询失败（masked 通路），本次轮询无读数: {}",
			    collected.error());
		return 0;
	}
	for (const auto& report : *collected) {
		pollStats().masked++;
		if (auto* logger = RLLogger::getInstance())
			logger->error("device fault 轮询: 抓到驱动**内部恢复过**、未上报 device lost 的 "
			              "masked fault —— {}",
			    formatFaultReport(report));
	}
	return static_cast<uint32_t>(collected->size());
}

void reportDeviceFaultPollSummary() {
	const auto stats = pollStats();
	if (stats.calls == 0u)
		return;  // 从未启用 = 不出声（启用与否由启动期那行 INFO 承担，不在这里重复一遍）
	if (auto* logger = RLLogger::getInstance())
		logger->info("device fault 轮询账: 调用 {} 次，抓到 masked fault {} 次", stats.calls, stats.masked);
}

}  // namespace RGL
