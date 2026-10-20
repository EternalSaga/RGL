module;
#include <doctest/doctest.h>

/**
 * test_heap_slot_audit —— 描述符堆**槽内容**对账（债-140 第三击 ①）的纯函数面。
 *
 * 被测对象 = `rgl/meshlet_shadow_args_impl.cppm` 里不碰 VkDevice 的那两个函数
 * （`auditHeapSlot` / `describeHeapSlotAudit`）。真实槽内容要一个 VkDevice + 描述符堆才拿得到
 * （那是 `tests/test_meshlet_shadow_args.cppm` 的设备套件在干的事），本文件用**合成堆内存**
 * 把三种现场各造一遍——这三种现场对归因的含义完全不同，所以必须**两两可分**：
 *
 *   ① 恒等            → 槽没问题，嫌疑往别处找；
 *   ② 全零（快照非零）→ 有人把槽清零了 = fault@0 的直接对应形态；
 *   ③ 全零（快照也零）→ **构造期那次描述符写入根本没生效**（我们写的是真实 BDA，
 *                        槽不可能全零）——同一条 allZero，但下一步查的地方相反；
 *   ④ 非零但与快照不同 → 槽号被复用 / 别人的 writeResourceDescriptorsEXT 越界写进来。
 *
 * ## 为什么这条网值得单独写测试（而两网替代不了）
 * ASan 不插桩描述符堆（mmap 的设备可见内存，不在它 CPU 侧的插桩面里）；GPU-AV 查描述符
 * **索引**越界，不查槽里存的**地址值**。所以"槽里是 0"这件事只有自己对账才知道——
 * 而对账逻辑本身写错（比如把"与快照相同"当成唯一判据）就会把 ③ 洗成通过，
 * 那是**放过了本案最像根因的那一条**。这条判据写不出像素，只能写成断言。
 *
 * ## 变异红线声明（每条"改在源码哪一行"，照抄即可重放）
 *   Ⓐ `ok()` 去掉 `&& !allZero` → 红「快照全零 + 当前全零」那条（③ 被洗成通过）
 *   Ⓑ `describeHeapSlotAudit` 里把 `allZero` 分支挪到 `matchesSnapshot` **之后**
 *      → 红同一条（措辞变成"与启动快照逐字节相同"）
 *   Ⓒ `auditHeapSlot` 的 `same` 判定改成"只看前 8 字节" → 红「只有尾部字节不同也要报不符」
 *   Ⓓ 地址匹配只比原值、不比掩掉高 16 位的形态 → 红「只有尾部字节不同也要报不符」
 *      （该字段现在**只在判负时**才算，所以能触发它的用例是判负的那条；本机 RADV 实测
 *       就是掩码形态：槽前 8 字节 = 0x0000800103532330，BDA = 0xffff800103532330）
 *   Ⓔ `allZero` 的初值从 `true` 改成 `false` → 红「全零」两条
 *   Ⓕ 把 `sizeMismatch` 的早退删掉 → 红「长度不同」那条（会拿短 span 去比长 span）
 */
export module test_heap_slot_audit;

import rlstd;
import meshlet_shadow_args;

namespace RGL {
namespace {

constexpr std::byte b(unsigned v) { return static_cast<std::byte>(v); }

/** 造一段"描述符槽"：前 8 字节 = 小端地址，尾部塞几个非零标志字节（模拟驱动的 size/控制位）。 */
std::vector<std::byte> slotWithAddress(uint64_t addr, uint8_t tail = 0xac) {
	std::vector<std::byte> s(32, b(0));
	for (int i = 0; i < 8; ++i)
		s[static_cast<size_t>(i)] = b(static_cast<unsigned>((addr >> (8 * i)) & 0xFFull));
	s[8] = b(tail);
	s[9] = b(tail ^ 0x3cu);
	return s;
}

/** 本机实测的那对值（从启动日志 `heap slot audit: 审计已启用` 那行抄）：
 *  BDA 高 16 位被驱动掩掉后落在槽前 8 字节。 */
constexpr uint64_t kRealBda = 0xffff800103532330ull;
constexpr uint64_t kRealBdaMasked = 0x0000800103532330ull;

}  // namespace

TEST_CASE("auditHeapSlot：与快照逐字节相同 = 通过（且热路径不拼 hex、不扫地址）") {
	const auto snap = slotWithAddress(kRealBdaMasked);
	const auto audit = auditHeapSlot(snap, snap, kRealBda, 8258u);
	CHECK(audit.ok());
	CHECK(audit.matchesSnapshot);
	CHECK_FALSE(audit.allZero);
	CHECK_FALSE(audit.sizeMismatch);
	CHECK(audit.slot == 8258u);
	CHECK(audit.expectedAddress == kRealBda);
	// 🔴 通过时**不算** hex、**不扫地址**（实测：无条件做 → 审计自身 0.027 ms/帧，比它要
	// 保护的整条对账贵三个数量级）。两者都是"解释一次失败"才用得上的字段；通过时的现场由
	// 构造期那行基准快照 hex 承担。认掩码地址这件事改由「只有尾部字节不同」那条用例覆盖
	// （那里判负，才会去扫地址）。
	CHECK(audit.currentHex.empty());
	CHECK_FALSE(audit.containsExpectedAddress);
}

TEST_CASE("auditHeapSlot 反向用例①：槽被清零（fault@0 的直接对应形态）必须点名") {
	const auto snap = slotWithAddress(kRealBdaMasked);
	const std::vector<std::byte> zeroed(32, b(0));
	const auto audit = auditHeapSlot(zeroed, snap, kRealBda, 8258u);
	CHECK_FALSE(audit.ok());
	CHECK(audit.allZero);
	CHECK_FALSE(audit.matchesSnapshot);
	CHECK_FALSE(audit.containsExpectedAddress);
	const auto text = describeHeapSlotAudit(audit);
	// 措辞必须把"这是什么"和"下一步查哪"都写出来（读日志的人不在现场）
	CHECK(text.find("全零") != std::string::npos);
	CHECK(text.find("fault@0") != std::string::npos);
	CHECK(text.find("binding 34") != std::string::npos);
	// 快照非零 ⟹ 是"后来被清零"，不是"从没写进去"，措辞不许指错方向
	CHECK(text.find("构造期快照本身就是零") == std::string::npos);
}

TEST_CASE("auditHeapSlot 反向用例①′：快照与当前**都**全零 = 构造期写入没生效（不许洗成通过）") {
	// 🔴 本条是这套用例里最值钱的一条：如果 ok() 只看"与快照相同"，这里会**绿**，
	// 而它描述的正是"我们从来没把描述符写进这个槽"——fault@0 最直接的成因。
	const std::vector<std::byte> zeroed(32, b(0));
	const auto audit = auditHeapSlot(zeroed, zeroed, kRealBda, 8258u);
	CHECK(audit.matchesSnapshot);  // 确实与基准相同……
	CHECK(audit.allZero);          // ……但基准本身是零
	CHECK_MESSAGE(!audit.ok(), "全零必须判负，即使它与快照相同（变异红线 Ⓐ）");
	const auto text = describeHeapSlotAudit(audit);
	CHECK(text.find("全零") != std::string::npos);
	// 且要指对方向：这次不是"被清零"，是"从没写进去"（变异红线 Ⓑ：分支顺序错了这里红）
	CHECK(text.find("构造期快照本身就是零") != std::string::npos);
	CHECK(text.find("没生效") != std::string::npos);
}

TEST_CASE("auditHeapSlot 反向用例②：槽里是**另一个非零地址**（陈旧值/槽号被复用）") {
	const auto snap = slotWithAddress(kRealBdaMasked);
	const auto stale = slotWithAddress(0x0000deadbee00000ull, 0x11);  // 别的 buffer 的地址
	const auto audit = auditHeapSlot(stale, snap, kRealBda, 8258u);
	CHECK_FALSE(audit.ok());
	CHECK_FALSE(audit.allZero);  // 🔴 与"全零"那条区分开：这是第三种现场
	CHECK_FALSE(audit.matchesSnapshot);
	CHECK_FALSE(audit.containsExpectedAddress);
	const auto text = describeHeapSlotAudit(audit);
	CHECK(text.find("与启动快照不符") != std::string::npos);
	CHECK(text.find("改写了这个槽") != std::string::npos);
	// 与"全零"那条区分开：用 fault@0 当判别词（全零分支有、本分支没有）。
	// ⚠ 不能拿"全零"当判别词——本分支的措辞里就有"但非全零"这个子串。
	CHECK(text.find("fault@0") == std::string::npos);
	CHECK(text.find("非零") != std::string::npos);
}

TEST_CASE("auditHeapSlot：只有尾部字节不同也必须报不符（描述符的控制位不是装饰）") {
	auto snap = slotWithAddress(kRealBdaMasked);
	auto cur = snap;
	cur[31] = b(static_cast<unsigned>(cur[31]) ^ 0xFFu);  // 地址 8 字节完全相同
	const auto audit = auditHeapSlot(cur, snap, kRealBda, 7u);
	CHECK_FALSE(audit.matchesSnapshot);
	CHECK_FALSE(audit.ok());
	CHECK_FALSE(audit.allZero);
	// 变异红线 Ⓒ：只看前 8 字节的话这里会"通过"，而驱动私有字段（size/对齐/控制位）被改
	// 一样会让设备读到错的东西。用"地址相同"这一条把它逼出来：
	CHECK(audit.containsExpectedAddress);  // 地址确实还在原处……
	// ……但结论仍须是不符，且要**指得出是哪个字节**（hex 若截断到 16，第 31 字节的差异
	// 就完全看不见 = 把现场拍糊了；本断言就是逼 hex 覆盖整个描述符 + 报出差异下标）
	CHECK(audit.hasDiff);
	CHECK(audit.firstDiffByte == 31u);
	// 判负时才拼 hex（见上），且必须**覆盖到第 31 字节**——截断的话这条现场就只剩"不符"
	// 两个字，读日志的人看不到到底哪里变了。
	CHECK(!audit.currentHex.empty());
	CHECK(audit.currentHex.size() >= 2 * 31 + 1);  // "xx " 分隔，够到第 31 字节
	const auto same = auditHeapSlot(snap, snap, kRealBda, 7u);
	CHECK_FALSE(same.hasDiff);
}

TEST_CASE("auditHeapSlot：长度不同 = 调用方 bug，早退且不拿短 span 去比长 span") {
	const auto snap = slotWithAddress(kRealBdaMasked);
	const std::vector<std::byte> shortRead(8, b(0));
	const auto audit = auditHeapSlot(shortRead, snap, kRealBda, 1u);
	CHECK(audit.sizeMismatch);
	CHECK_FALSE(audit.ok());
	const auto text = describeHeapSlotAudit(audit);
	CHECK(text.find("descriptorSize") != std::string::npos);
	CHECK(text.find("调用方 bug") != std::string::npos);
	// 变异红线 Ⓕ：早退被删掉时这里会串进"全零"或"改写"的措辞——把本模块自己的 bug
	// 报成驱动的问题，方向完全相反。
	CHECK(text.find("fault@0") == std::string::npos);
}

TEST_CASE("describeHeapSlotAudit：四种现场的措辞两两不同（同一段 hex，四种下一步）") {
	const auto snap = slotWithAddress(kRealBdaMasked);
	const std::vector<std::byte> zeroed(32, b(0));
	const std::vector<std::byte> shortRead(8, b(0));
	const auto stale = slotWithAddress(0x0000deadbee00000ull, 0x11);
	const std::string texts[] = {
		describeHeapSlotAudit(auditHeapSlot(snap, snap, kRealBda, 1u)),
		describeHeapSlotAudit(auditHeapSlot(zeroed, snap, kRealBda, 1u)),
		describeHeapSlotAudit(auditHeapSlot(zeroed, zeroed, kRealBda, 1u)),
		describeHeapSlotAudit(auditHeapSlot(stale, snap, kRealBda, 1u)),
	};
	for (size_t i = 0; i < 4; ++i)
		for (size_t j = i + 1; j < 4; ++j)
			CHECK_MESSAGE(texts[i] != texts[j], "第 ", i, " 与第 ", j, " 种现场措辞相同");
	// 每条都自带槽号与 hex（红了要能定位是哪个槽、里面是什么）
	for (const auto& t : texts) {
		CHECK(t.find("heap 槽 1") != std::string::npos);
		CHECK(t.find("0xffff800103532330") != std::string::npos);
	}
}

}  // namespace RGL
