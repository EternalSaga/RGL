module;
#include <doctest/doctest.h>

/**
 * test_device_fault —— GPU fault 诊断通路的**纯函数面**（无设备、无 GPU、无窗口）。
 *
 * 被测对象 = `rgl/device_fault_impl.cppm` 里不碰 VkDevice 的那半：后端优先级、枚举翻译表、
 * 地址区间闭式、报告拼装、空批次读数、EXT 两段式的容量决策。
 *
 * ## 为什么这半边值得单独钉
 * 这个模块的全部价值在于**读出来的话可不可信**：一条 fault 报告最终是一行日志，而日志没有
 * 编译器。真正会错的地方不是 Vulkan 调用（那三步是直白搬运），而是
 * ① 后端选错（启用了 EXT 却调 KHR 入口 = 在未启用的扩展上调入口点）；
 * ② 枚举翻译表漏一个值（新值静默落进"无地址信息"= 一条假负读数）；
 * ③ 地址区间算错（spec 的 lower/upper 闭式，算错就指错内存）；
 * ④ "空报告"与"查询失败"两条出口措辞混同（前者只能说**驱动可见面**没有，后者连可见面都
 *    没读到；两者对归因的含义不同——而"可见面没有"本身**不**等于"无 fault"，见下）。
 * 这四件事全部落在纯函数上，本文件逐条钉。
 *
 * ## 🔴 已知不可达 / 缺口（写出来，别让「全绿」被读成「全验过」）
 *   · `collectKhr` / `collectExt` 的**调用序本身**（第一段 pFaultInfo=nullptr → 按 counts
 *     分配 → 第二段）要一个真 VkDevice 才能跑，本文件只钉它的**容量决策**
 *     （`planExtFaultCapacity`，即第二段三个入参怎么填）。调用序错了要靠实机跑 + VVL 兜。
 *   · `reportDeviceFaultOnLost` / `pollDeviceFaults` 的三条出口（未启用 / 查询失败 / 空 /
 *     有报告）是日志文本，无返回值可断言；本文件断言的是它们**拼出去的那些串**（同一批
 *     纯函数），出口选择本身归实机（本批的 MDI hang 矩阵就是那条路）。
 *   · `deriveDeviceFaultSupport` 要 PhysicalDevice（查 maxFaultCount），本文件只钉它不含
 *     设备的那半 —— `chooseDeviceFaultBackend`（优先级唯一落点）。
 *
 * ## 变异红线声明（每条"哪个变异让我红"；**九条全部实测**，最近一次 2026-10-20 本批重跑
 * 9/9 有红——⑦ 因文案改口径拆成 a/b 两条，其余与上一批同）
 * 执行方式 = `/tmp/opencode/n10p4/mutate_df.py`（⚠ 该路径随重启即失，**留的是配方**：
 * `shutil.copyfile` 备份/还原不带 mtime 坑、禁 git checkout、每条显式
 * `cmake --build build --target rgl_tests` 并核对**构建自身**返回码、全部还原后重跑全量
 * 断言全绿）。逐条写法 = 变异**改在源码上的那一行**，照抄即可重放；括号里是实测红在哪条。
 *   ① `chooseDeviceFaultBackend` 两个 if 顺序调换（先看 ext）→ 红「四格真值表」
 *   ② `classifyFaultAddressType` 的 `default: break` 改成 `return none` → 红「表外值落
 *      unrecognized」（这条是**假负读数**的形态：驱动给了个我们没抄的值，日志会说"无地址信息"）
 *   ③ `describeFaultAddress` 的 lower 写成 `reported & precision`（少取反）→ 红区间闭式
 *   ④ 同函数的 `precision <= 1` 改成 `< 1` → 红 precision=1 那条（边界，不是随便一个值）
 *   ⑤ `describeFaultFlags` 的表外位判据改成永不成立 → 红「位外点名」
 *   ⑥ `planExtFaultCapacity` 无视 `vendorBinaryEnabled` 直接透传 → 红「未启用必须传 0」
 *   ⑦a `describeEmptyFaultBatch` 去掉范围限定词（"**驱动可见面**无 fault"→"无 fault"）
 *      → 红「『驱动可见面没有』须与『无 fault』可分」
 *   ⑦b 同函数丢掉堵误读的那半句（"⚠ 这不等于「无 fault」"）→ 红同一条
 *      （⑦ 原本是一条"文案改成与查询失败同义"，2026-10-20 devcoredump 推翻旧措辞后
 *       拆成 a/b 两条：现在这条文案有**两个**不可删的半句，各钉各的）
 *   ⑧ `describeFaultAddressKind` 的 readInvalid 返回 writeInvalid 的串 → 红 2 条
 *      （两两不同 + formatFaultReport）
 * ⚠ 写变异的一条教训（本批一次通过，因为上一批踩过）：**弱化**判据而不是删掉它。
 * 删掉 `if` 会让被弃用的变量撞 `-Werror,-Wunused-*`，"编不过"看着像守住了，其实什么都没测。
 */
export module test_device_fault;

import rlstd;
import device_fault;

namespace RGL {
namespace {

/** 用底层枚举值当输入（🔴 不取被测的 classify 函数当期望值出处，也不引 rlvk 只为拿个常量）。 */
constexpr uint32_t kAddrNone = 0;
constexpr uint32_t kAddrRead = 1;
constexpr uint32_t kAddrWrite = 2;
constexpr uint32_t kAddrExecute = 3;
constexpr uint32_t kAddrIpUnknown = 4;
constexpr uint32_t kAddrIpInvalid = 5;
constexpr uint32_t kAddrIpFault = 6;

constexpr uint32_t kFlagDeviceLost = 1u << 0;
constexpr uint32_t kFlagMemoryAddress = 1u << 1;
constexpr uint32_t kFlagInstructionAddress = 1u << 2;
constexpr uint32_t kFlagVendor = 1u << 3;
constexpr uint32_t kFlagWatchdogTimeout = 1u << 4;
constexpr uint32_t kFlagOverflow = 1u << 5;

}  // namespace

// ─── 后端选择（优先级唯一落点）─────────────────────────────────────────────────

TEST_CASE("chooseDeviceFaultBackend：四格真值表，两条都可用时 KHR 赢") {
	// 变异红线 ①：把两个 if 的顺序调换 → 第三格红（EXT 抢走 KHR = 丢掉 masked 上报能力，
	// 而日志里 backend 仍显示"有通路"，是最难发现的一种降级）。
	CHECK(chooseDeviceFaultBackend(false, false) == DeviceFaultBackend::none);
	CHECK(chooseDeviceFaultBackend(false, true) == DeviceFaultBackend::ext);
	CHECK(chooseDeviceFaultBackend(true, false) == DeviceFaultBackend::khr);
	CHECK_MESSAGE(chooseDeviceFaultBackend(true, true) == DeviceFaultBackend::khr,
	    "两条都可用必须选 KHR（EXT rev 2 没有 masked 上报）");
}

TEST_CASE("describeDeviceFaultBackend：三条互不相同，none 那条要说清 no-op 的原因") {
	const auto none = describeDeviceFaultBackend(DeviceFaultBackend::none);
	const auto khr = describeDeviceFaultBackend(DeviceFaultBackend::khr);
	const auto ext = describeDeviceFaultBackend(DeviceFaultBackend::ext);
	CHECK(none != khr);
	CHECK(none != ext);
	CHECK(khr != ext);
	// none 的正文要含"未启用"——它是启动日志里唯一说明"为什么没有 fault 段"的地方
	CHECK(none.find("未启用") != std::string_view::npos);
	// KHR 那条必须点出 masked（读者据此判断轮询门有没有意义）
	CHECK(khr.find("masked") != std::string_view::npos);
}

// ─── 地址类型翻译表 ───────────────────────────────────────────────────────────

TEST_CASE("classifyFaultAddressType：七个规格值各有归属，表外值落 unrecognized 而不是 none") {
	CHECK(classifyFaultAddressType(kAddrNone) == FaultAddressKind::none);
	CHECK(classifyFaultAddressType(kAddrRead) == FaultAddressKind::readInvalid);
	CHECK(classifyFaultAddressType(kAddrWrite) == FaultAddressKind::writeInvalid);
	CHECK(classifyFaultAddressType(kAddrExecute) == FaultAddressKind::executeInvalid);
	CHECK(classifyFaultAddressType(kAddrIpUnknown) == FaultAddressKind::ipUnknown);
	CHECK(classifyFaultAddressType(kAddrIpInvalid) == FaultAddressKind::ipInvalid);
	CHECK(classifyFaultAddressType(kAddrIpFault) == FaultAddressKind::ipFault);
	// 变异红线 ②：default 若写成 return none，下面两条会**绿**——所以断言的是"不等于 none"
	CHECK(classifyFaultAddressType(99u) != FaultAddressKind::none);
	CHECK(classifyFaultAddressType(99u) == FaultAddressKind::unrecognized);
	CHECK(classifyFaultAddressType(0xFFFF'FFFFu) == FaultAddressKind::unrecognized);
	// 七个规格值必须映射到七个不同分类（漏抄一个 case 就撞车）
	const uint32_t all[] = {kAddrNone, kAddrRead, kAddrWrite, kAddrExecute, kAddrIpUnknown,
		kAddrIpInvalid, kAddrIpFault};
	std::vector<FaultAddressKind> seen;
	for (const uint32_t raw : all)
		seen.push_back(classifyFaultAddressType(raw));
	for (size_t i = 0; i < seen.size(); ++i)
		for (size_t j = i + 1; j < seen.size(); ++j)
			CHECK_MESSAGE(seen[i] != seen[j], "第 ", i, " 与第 ", j, " 个规格值撞在同一分类上");
}

TEST_CASE("describeFaultAddressKind：八条白话两两不同（读日志的人靠它区分故障形态）") {
	const std::string_view kinds[] = {
		describeFaultAddressKind(FaultAddressKind::none),
		describeFaultAddressKind(FaultAddressKind::readInvalid),
		describeFaultAddressKind(FaultAddressKind::writeInvalid),
		describeFaultAddressKind(FaultAddressKind::executeInvalid),
		describeFaultAddressKind(FaultAddressKind::ipUnknown),
		describeFaultAddressKind(FaultAddressKind::ipInvalid),
		describeFaultAddressKind(FaultAddressKind::ipFault),
		describeFaultAddressKind(FaultAddressKind::unrecognized),
	};
	for (size_t i = 0; i < 8; ++i)
		for (size_t j = i + 1; j < 8; ++j)
			CHECK_MESSAGE(kinds[i] != kinds[j], "第 ", i, " 与第 ", j, " 条翻译串相同（变异红线 ⑧）");
	// 语义抽查：读与写必须是**不同**的两种故障，且各自的串里点得出来
	CHECK(describeFaultAddressKind(FaultAddressKind::readInvalid).find("读")
	    != std::string_view::npos);
	CHECK(describeFaultAddressKind(FaultAddressKind::writeInvalid).find("写")
	    != std::string_view::npos);
	// unrecognized 的措辞必须**不许**读者把它当成"驱动说没有地址信息"
	CHECK(describeFaultAddressKind(FaultAddressKind::unrecognized).find("别当成")
	    != std::string_view::npos);
}

// ─── KHR flags ────────────────────────────────────────────────────────────────

TEST_CASE("describeFaultFlags：逐位翻、组合可分、零有位外点名") {
	CHECK(describeFaultFlags(0u).find("flags = 0") != std::string::npos);
	// watchdog 超时 = 本批 hang 的签名，必须翻成读得懂的话
	CHECK(describeFaultFlags(kFlagWatchdogTimeout).find("watchdog") != std::string::npos);
	CHECK(describeFaultFlags(kFlagDeviceLost).find("device lost") != std::string::npos);
	CHECK(describeFaultFlags(kFlagOverflow).find("溢出") != std::string::npos);
	// 组合：两条单条的串必须都在，且用分隔符连着（不是互相吞掉）
	const auto combo = describeFaultFlags(kFlagMemoryAddress | kFlagInstructionAddress);
	CHECK(combo.find("含内存地址") != std::string::npos);
	CHECK(combo.find("含指令地址") != std::string::npos);
	CHECK(combo.find(",") != std::string::npos);
	CHECK(combo != describeFaultFlags(kFlagMemoryAddress));
	// 变异红线 ⑤：表外的位必须出声并带上原始位值（否则"驱动给了新信息"读成"驱动没给"）
	const auto unknown = describeFaultFlags(1u << 20);
	CHECK(unknown.find("未识别") != std::string::npos);
	CHECK(unknown.find("100000") != std::string::npos);  // {:#x} 的 1<<20
	// 已知 + 未知混在一起：两条都要在（丢掉已知那条 = 少报一个事实）
	const auto mixed = describeFaultFlags(kFlagVendor | (1u << 21));
	CHECK(mixed.find("含厂商信息") != std::string::npos);
	CHECK(mixed.find("未识别") != std::string::npos);
}

// ─── 地址区间闭式（spec 原文公式）─────────────────────────────────────────────

TEST_CASE("describeFaultAddress：precision≤1 走精确分支，否则按 spec 闭式给区间") {
	// 期望值**手算**（lower = r & ~(p-1)、upper = r | (p-1)），不调被测函数
	CHECK(describeFaultAddress(0x7f12'3456ull, 0).find("精确地址") != std::string::npos);
	// 变异红线 ④：`<= 1` 写成 `< 1` 时 precision=1 会掉进区间分支，算出 lower=1·0=0 的鬼话
	CHECK(describeFaultAddress(0x7f12'3456ull, 1).find("精确地址") != std::string::npos);
	CHECK(describeFaultAddress(0x7f12'3456ull, 1).find("0x7f123456") != std::string::npos);
	// 两个分支的十六进制写法一致（都不补零）：一处补零一处不补，读者会以为两个地址不同量级
	CHECK(describeFaultAddress(0x40ull, 0).starts_with("0x40"));
	// 4KB 精度：0x7f123456 → [0x7f123000, 0x7f123fff]
	const auto page = describeFaultAddress(0x7f12'3456ull, 0x1000);
	CHECK(page.find("0x7f123000") != std::string::npos);
	CHECK(page.find("0x7f123fff") != std::string::npos);
	CHECK(page.find("4KB") != std::string::npos);
	// 2MB 精度（GPU 大页常见值）
	const auto huge = describeFaultAddress(0x7f12'3456ull, 0x200000);
	CHECK(huge.find("0x7f000000") != std::string::npos);
	CHECK(huge.find("0x7f1fffff") != std::string::npos);
	CHECK(huge.find("2MB") != std::string::npos);
	// 非 2 的幂次对齐的精度（驱动不该给，但给了也不能算出错位的区间下界）
	const auto odd = describeFaultAddress(0x1000'0000ull, 512);
	CHECK(odd.find("512B") != std::string::npos);
	CHECK(odd.find("0x10000000") != std::string::npos);
}

// ─── 报告拼装 ─────────────────────────────────────────────────────────────────

TEST_CASE("formatFaultReport：填了什么就出什么，没填的字段不编内容") {
	FaultReportView r;
	r.description = "VM fault on page";
	r.flags = kFlagMemoryAddress | kFlagWatchdogTimeout;
	r.groupId = 42;
	r.faultKind = FaultAddressKind::writeInvalid;
	r.faultAddress = 0xdead'beefull;
	r.faultAddressPrecision = 0x1000;
	r.instructionKind = FaultAddressKind::ipFault;
	r.instructionAddress = 0x1000ull;
	r.instructionAddressPrecision = 1;
	r.vendorDescription = "radv: CSN idle timeout";
	r.vendorFaultCode = 0xAB;
	r.vendorFaultData = 0xCD;
	const auto line = formatFaultReport(r);
	CHECK(line.find("VM fault on page") != std::string::npos);
	CHECK(line.find("watchdog") != std::string::npos);
	CHECK(line.find("group=42") != std::string::npos);
	CHECK(line.find("非法写") != std::string::npos);
	CHECK(line.find("与故障直接相关的指令指针") != std::string::npos);
	CHECK(line.find("radv: CSN idle timeout") != std::string::npos);
	CHECK(line.find("0xab") != std::string::npos);
	CHECK(line.find("0xcd") != std::string::npos);

	// 空报告（驱动只给了个 description 都没有）：不能编出地址或厂商内容
	FaultReportView bare;
	const auto bareLine = formatFaultReport(bare);
	CHECK(bareLine.find("(驱动未给描述)") != std::string::npos);
	CHECK(bareLine.find("地址=") == std::string::npos);
	CHECK(bareLine.find("厂商") == std::string::npos);
	// 只有访存类型不同 ⟹ 两行必须不同（否则翻译表等于没起作用）
	FaultReportView readOne = bare, writeOne = bare;
	readOne.faultKind = FaultAddressKind::readInvalid;
	writeOne.faultKind = FaultAddressKind::writeInvalid;
	CHECK(formatFaultReport(readOne) != formatFaultReport(writeOne));
}

TEST_CASE("describeEmptyFaultBatch：『驱动可见面没有』须与『无 fault』和『没查成』都可分") {
	// 🔴 本用例的口径在 2026-10-20 被实测改过一次，改的理由比断言本身值钱：
	// 旧文案写的是「本次是纯 hang 无 fault」，而 devcoredump 抓到内核层 `[gfxhub] Page fault
	// observed` + fault 地址恰 = 0x0 ⟹ **驱动可见面报空 ≠ 无 fault**（fault 没进驱动的 fault
	// ring）。旧措辞会把读者送到"排除 GPUVM 越界"那个错方向上，所以文案收窄成"驱动可见面"，
	// 并显式堵一句"这不等于「无 fault」"。
	// ⚠ 那次只改了实现没改本用例 ⟹ HEAD 红过一轮。教训：**改日志文案 = 改判据**，
	// 断言这些串的测试必须同批改，否则"文案与判据分家"要等下一次换构建标志才现形。
	const auto empty = describeEmptyFaultBatch(DeviceFaultBackend::khr);
	// 前半：范围限定词必须在（少了它就又变回"无 fault"那个过强结论）
	CHECK(empty.find("驱动可见面") != std::string::npos);
	// 后半：堵误读的那半句必须在，且要指到"无 fault"这个具体被堵掉的读法
	CHECK(empty.find("不等于「无 fault」") != std::string::npos);
	// 指向下一步取证（本模块自己不给结论兜底，把读者送去能给出答案的地方）
	CHECK(empty.find("devcoredump") != std::string::npos);
	// 变异红线 ⑦：与"查询失败"那条出口仍须可分（那条说的是调用本身失败、连"可见面"都没读到）
	CHECK(empty.find("无读数") == std::string::npos);
	CHECK(empty.find("VK_KHR_device_fault") != std::string::npos);  // 带后端，读数可归因
	CHECK(describeEmptyFaultBatch(DeviceFaultBackend::ext).find("VK_EXT_device_fault")
	    != std::string::npos);
}

// ─── EXT 两段式的容量决策 ─────────────────────────────────────────────────────

TEST_CASE("planExtFaultCapacity：counts 原样当容量，未启用 vendorBinary 时那一项必须归 0") {
	const auto cap = planExtFaultCapacity(3u, 2u, true, 4096ull);
	CHECK(cap.addressInfoCount == 3u);
	CHECK(cap.vendorInfoCount == 2u);
	CHECK(cap.vendorBinarySize == 4096ull);
	// 变异红线 ⑥：直接透传时本断言红。规则本体：未启用 deviceFaultVendorBinary 时传非 0，
	// 驱动仍会置 0 且**不碰**缓冲区 ⟹ "我给了它没写"与"它写满了"在日志里长得一样。
	const auto noBinary = planExtFaultCapacity(3u, 2u, false, 4096ull);
	CHECK(noBinary.vendorBinarySize == 0ull);
	CHECK(noBinary.addressInfoCount == 3u);  // 另两项不受影响（别顺手清零）
	CHECK(noBinary.vendorInfoCount == 2u);
	// 第一段给 0 时第二段也必须 0（不能凭空给容量，那会让驱动以为数组更大）
	const auto zero = planExtFaultCapacity(0u, 0u, true, 0ull);
	CHECK(zero.addressInfoCount == 0u);
	CHECK(zero.vendorInfoCount == 0u);
	CHECK(zero.vendorBinarySize == 0ull);
}

// ─── vendor crash dump（vkGetDeviceFaultDebugInfoKHR，本批补全的通路）────────────

TEST_CASE("classifyVendorBinary：三种负读数各指一个责任方，互不可混") {
	// 🔴 本条用例的**四条**断言顺序就是责任顺序，而这条判据是实测逼出来的：第一版只有
	// featureOff/empty 两态，本机第一次实跑就把"RADV 根本没 advertise 该 feature"报成了
	// "我们没启用"——让人去查自己的接线，而问题在驱动。加 `unsupported` 第四态才归得对因。
	// ① 驱动没 advertise → 责任在驱动，且**不看** enabled（无从启用）
	CHECK(classifyVendorBinary(false, false, 0ull) == VendorBinaryOutcome::unsupported);
	CHECK(classifyVendorBinary(false, true, 0ull) == VendorBinaryOutcome::unsupported);
	CHECK(classifyVendorBinary(false, true, 4096ull) == VendorBinaryOutcome::unsupported);
	// ② 驱动 advertise 了而我们没启用 → 责任在我们（本引擎按构造到不了，到得了就是接线 bug）
	CHECK(classifyVendorBinary(true, false, 0ull) == VendorBinaryOutcome::featureOff);
	// ③ 已启用而 size=0 → 驱动这次没给
	CHECK(classifyVendorBinary(true, true, 0ull) == VendorBinaryOutcome::empty);
	// ④ 有数据
	CHECK(classifyVendorBinary(true, true, 1ull) == VendorBinaryOutcome::dump);
	CHECK(classifyVendorBinary(true, true, 4096ull) == VendorBinaryOutcome::dump);

	const auto unsup = describeVendorBinaryOutcome(VendorBinaryOutcome::unsupported, 0ull);
	const auto off = describeVendorBinaryOutcome(VendorBinaryOutcome::featureOff, 0ull);
	const auto empty = describeVendorBinaryOutcome(VendorBinaryOutcome::empty, 0ull);
	const auto dump = describeVendorBinaryOutcome(VendorBinaryOutcome::dump, 4096ull);
	// 四条两两不同（同一段 hex 四种下一步，读日志的人靠措辞分责任方）
	const std::string all[] = {unsup, off, empty, dump};
	for (size_t i = 0; i < 4; ++i)
		for (size_t j = i + 1; j < 4; ++j)
			CHECK_MESSAGE(all[i] != all[j], "第 ", i, " 与第 ", j, " 条措辞相同");
	// ① 必须把责任指向驱动，且**不许**让人去查我们的接线（第一版就是错在这里）
	CHECK(unsup.find("没 advertise") != std::string::npos);
	CHECK(unsup.find("不是本引擎的接线问题") != std::string::npos);
	CHECK(unsup.find("查 deriveDeviceFaultSupport") == std::string::npos);
	// ② 才是指向我们自己，并给出查哪条链
	CHECK(off.find("接线 bug") != std::string::npos);
	CHECK(off.find("deriveDeviceFaultSupport") != std::string::npos);
	// ③ 点名"已启用"，并带上 size 读数（本机换到有 vendorBinary 的驱动上会走这条或 ④）
	CHECK(empty.find("已启用") != std::string::npos);
	CHECK(dump.find("4096") != std::string::npos);
}

TEST_CASE("vendorBinaryFileName：空 tag 不许退化成固定名（第二次崩溃会覆盖第一次的现场）") {
	CHECK(vendorBinaryFileName("frame185") == "rgl-fault-frame185.bin");
	CHECK(vendorBinaryFileName("frame32") != vendorBinaryFileName("frame33"));
	// 空 tag 的退化名必须是**显式可辨**的 noframe，而不是悄悄复用某个真实帧的名字
	CHECK(vendorBinaryFileName("") == "rgl-fault-noframe.bin");
	CHECK(vendorBinaryFileName("").find("noframe") != std::string::npos);
	CHECK_FALSE(vendorBinaryDumpDir().empty());
}

}  // namespace RGL
