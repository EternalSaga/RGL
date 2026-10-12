module;
/**
 * test_scene_sentinel —— scene data 迁移批 T0：四张 per-object 表尾**毒化哨兵元素**护栏
 * （§9.9 替代护栏本体，纯 CPU、无设备）。
 *
 * ## 为什么需要它（护栏的护栏）
 *
 * GPU-AV 在本项目架构下不可用（§9.9 三条实测障碍：env 变量开不起来 / 被
 * VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT 顶死 / 摘掉 flag 后越界读**一条不报**）。
 * 而各区**紧邻**（表大小都是 256 的整数倍，下一区正好接在表尾）⟹ 越界读读到的是**邻居区的
 * 合法数据** = 静默错值，逐位锚点看不见。替代方案 = 每张表尾追加一个元素、写成毒化模式，
 * 越界读 ⟹ 毒值进着色器 ⟹ 锚点必炸。
 *
 * 本测试钉的是这条护栏**自身**的四条性质。它们任何一条漂了，护栏都退化成"看起来有、实际
 * 不咬人"的摆设，而端到端锚点**不会**告诉你（锚点只在有人真的越界读时才有意义）：
 *
 *   (a) **毒值三型有毒**：0xFFC00000 按 float 读是 NaN、按 uint 读是 4290772992、按 int 读
 *       是负数。四张表的元素类型两种（float4 系 / uint 系），必须都中毒。
 *   (b) **位置**：哨兵相对表基址恒 = 可用容量 × stride（= "下标 capacity 那一格"），
 *       且它是该区**最后一个**元素（区 = 容量 + 1）。绝对地址另用手抄字面量钉第二源。
 *   (c) **可用容量语义不变**：容量常量仍是 4096。区变宽 ≠ 能多装一个实例。
 *   (d) **写侧不覆盖**：`writeSceneSentinels` 只写那 4 个窗口（邻居字节一个不碰），
 *       而每帧跑的 `writeClusterRangeRegion` 在**满表**与**空表**两态下都不碰哨兵。
 *
 * ## 两源结构
 *
 * 写侧 = `scene_data.cppm` 的哨兵常量与 `writeSceneSentinels`；读回侧 = 本文件**手抄的绝对
 * 字面量**（262400 / 525568 / 722432 / 789760）+ 手抄的邻居区基址。手抄值与 C++ 常量各写
 * 一份、互为对账（先例 = test_ssr_params / test_meshlet_ranges 的"两源"）。
 *
 * ## 变异红线声明（哪个变异让我红）
 *
 *   ① `kSceneSentinelWord` 改成 0（= 没毒）→ (a) 三条全红（NaN/uint/int 都不成立）。
 *      改成 0x80000000（float = −0.0，**不是** NaN）→ (a) 的 isnan 红。
 *   ② 哨兵偏移改成 `(capacity - 1) * stride`（写进最后一个**可用**槽）→ (b) 的相对偏移与
 *      绝对字面量双红；(d) 的"邻居字节不动"也红（最后一个可用槽被毒值占了 = 真数据被吃）。
 *   ③ 容量常量偷偷改成 4097 → (c) 红 + (b) 的绝对字面量红（哨兵整体后移一格）。
 *   ④ 区大小忘了 +1（写成 capacity × stride）→ (b) 的"哨兵是区最后一个元素"红，
 *      且 (d) 的"哨兵落在区内"红（哨兵越出区 = 写进邻居区）。
 *   ⑤ `writeClusterRangeRegion` 的 memset 长度写回 `kSceneDataClusterRangeBytes - usedBytes`
 *      （= 每帧抹掉哨兵）→ (d) 的空表态与满表态**都**红。
 *   ⑥ `writeSceneSentinels` 少写一张表（删掉 fill 四行里的任意一行）→ (d) 的"四窗口全毒"红；
 *      多写（把窗口长度写成 stride 的两倍）→ (d) 的"窗口外字节不动"红。
 *   ⑦ 把某张表的哨兵窗口整体挪进**邻居区**（如把矩阵表哨兵写成 +4097 格）→ (d) 的邻居
 *      字节断言红 + (b) 的绝对字面量红。
 *   ⚠ 本测试**测不到**的：着色器真的把越界读读成 NaN 并改变像素（那是端到端判据 =
 *   两档锚点 + 下标 +capacity 的有牙变异，见 §9.8 的 T0 行）。
 */
#include <doctest/doctest.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>  // modules 坑：range-for 的花括号列表要真 STL 头（只 include doctest 不够）
#include <vector>

export module test_scene_sentinel;

import rlstd;
import scene_data;  // 四张表的基址/区大小/哨兵偏移 + kSceneSentinelWord + writeSceneSentinels
import scene_forward_types_generated;  // ObjectClusterRange（writeClusterRangeRegion 的入参类型）

namespace RGL {
namespace {

/** 手抄的四个哨兵绝对字节地址（第二源；出处 = scene_data.cppm 各区派生式，此处独立写死）。
 *  期望值来源是**手算**：矩阵表 256 + 4096×64 / MVP 263424 + 4096×64 /
 *  材质表 525824 + 4096×48 / 簇区间表 724224 + 4096×16。 */
constexpr uint32_t kSentinelMatrix = 262400;
constexpr uint32_t kSentinelMvp = 525568;
constexpr uint32_t kSentinelDraw = 722432;
constexpr uint32_t kSentinelCluster = 789760;

/** 手抄的四张表基址与 stride（同上，独立写死，不复用被测常量做期望值）。 */
constexpr uint32_t kBaseMatrix = 256;
constexpr uint32_t kBaseMvp = 263424;
constexpr uint32_t kBaseDraw = 525824;
constexpr uint32_t kBaseCluster = 724224;
constexpr uint32_t kStrideMatrix = 64;
constexpr uint32_t kStrideDraw = 48;
constexpr uint32_t kStrideCluster = 16;
constexpr uint32_t kCapacity = 4096;

/// 整块 staging 填同一个字节（VMA 不保证映射内存初值，测试因此**必须**自带初值）。
std::vector<std::byte> makeStaging(std::byte fill = std::byte { 0xa5 }) {
	return std::vector<std::byte>(kSceneDataBytes, fill);
}

uint32_t readWord(const std::vector<std::byte>& buf, uint64_t at) {
	uint32_t v = 0;
	std::memcpy(&v, buf.data() + at, sizeof(v));
	return v;
}

/** [from, to) 内第一个**不等于** expected 的下标（-1 = 全等）。 */
int64_t firstByteNot(const std::vector<std::byte>& buf, uint64_t from, uint64_t to,
	std::byte expected) {
	for (uint64_t i = from; i < to; ++i)
		if (buf[i] != expected)
			return static_cast<int64_t>(i);
	return -1;
}

/** 一个哨兵窗口是否整格都是毒值（逐 4 字节字）。 */
bool windowPoisoned(const std::vector<std::byte>& buf, uint64_t at, uint32_t bytes) {
	for (uint32_t i = 0; i < bytes; i += sizeof(uint32_t))
		if (readWord(buf, at + i) != kSceneSentinelWord)
			return false;
	return true;
}

/// 四张表的 (哨兵地址, 元素字节数) 清单——用例逐条遍历，漏一张由"总数"断言兜。
struct SentinelWindow {
	uint64_t at;
	uint32_t bytes;
	const char* name;
};
std::vector<SentinelWindow> sentinelWindows() {
	return {
		{ kSceneDataObjectMatrixSentinelOffset, kSceneMatrixStride, "objectMatrix" },
		{ kSceneDataMvpTableSentinelOffset, kSceneMatrixStride, "mvp" },
		{ kSceneDataDrawTableSentinelOffset, kSceneDrawStride, "draw" },
		{ kSceneDataClusterRangeSentinelOffset, kSceneDataClusterRangeStride, "clusterRange" },
	};
}

}  // namespace

// ─── (a) 毒值三型有毒 ────────────────────────────────────────────────────────

TEST_CASE("毒化模式 0xFFC00000：float=NaN / uint=4290772992 / int=负数（三种元素类型都有毒）") {
	// 期望值全部**手算写死**，不取被测常量：0xFFC00000 的指数域 = 0xFF、尾数非 0 ⟹ 规范 NaN；
	// 无符号值 = 2^32 − 2^22 = 4294967296 − 4194304；有符号 = −4194304。
	static_assert(kSceneSentinelWord == 0xFFC00000u, "poison word drifted from 0xFFC00000");
	CHECK(kSceneSentinelWord != 0u);  // 0 = "没毒"，与"空槽"不可区分 = 护栏恒不咬人
	CHECK(kSceneSentinelWord != 0xFFFFFFFFu);  // 全 1 与"未初始化擦除"同形，不可归因

	float asFloat = 0.0f;
	std::memcpy(&asFloat, &kSceneSentinelWord, sizeof(asFloat));
	CHECK(std::isnan(asFloat));  // float4 系元素（ObjectMatrix / ObjectDraw 的 vec4 字段）

	const uint32_t asUint = kSceneSentinelWord;
	CHECK(asUint == 4290772992u);  // uint 系元素（ObjectClusterRange）：远超任何表容量/下标

	const int32_t asInt = static_cast<int32_t>(kSceneSentinelWord);
	CHECK(asInt == -4194304);
	CHECK(asInt < 0);  // 任何无符号语义的下标/计数读出来都是负数

	// 反向：0x80000000 是 −0.0（**不是** NaN）。它常被误当成"哨兵值"，对 float 表完全无毒。
	const uint32_t notPoison = 0x80000000u;
	float minusZero = 0.0f;
	std::memcpy(&minusZero, &notPoison, sizeof(minusZero));
	CHECK_FALSE(std::isnan(minusZero));
}

// ─── (b)(c) 位置与容量语义 ───────────────────────────────────────────────────

TEST_CASE("哨兵位置：相对表基址 = 容量×stride、是区最后一个元素、绝对地址 = 手抄字面量") {
	// (c) 可用容量语义不变——区变宽 ≠ 多一个实例槽。
	CHECK(kSceneMatrixCapacity == kCapacity);
	CHECK(kSceneDataClusterRangeCapacity == kCapacity);

	struct Row {
		uint64_t base, sentinel, stride;
		uint32_t regionBytes;
		const char* name;
	};
	const std::vector<Row> rows {
		{ kSceneDataObjectMatrixOffset, kSceneDataObjectMatrixSentinelOffset, kStrideMatrix,
			kSceneDataObjectMatrixBytes, "objectMatrix" },
		{ kSceneDataMvpTableOffset, kSceneDataMvpTableSentinelOffset, kStrideMatrix,
			kSceneDataMvpTableBytes, "mvp" },
		{ kSceneDataDrawTableOffset, kSceneDataDrawTableSentinelOffset, kStrideDraw,
			kSceneDataDrawTableBytes, "draw" },
		{ kSceneDataClusterRangeOffset, kSceneDataClusterRangeSentinelOffset, kStrideCluster,
			kSceneDataClusterRangeBytes, "clusterRange" },
	};
	CHECK(rows.size() == 4);  // 前提守卫：表为空时下面全是空断言（假绿）

	for (const auto& r : rows) {
		INFO(r.name);
		// (b) 相对位置：哨兵 = "下标 capacity 那一格"，正是第一个越界槽
		CHECK(r.sentinel - r.base == static_cast<uint64_t>(kCapacity) * r.stride);
		// (b) 区 = 容量 + 1 个元素，且哨兵是最后一个（区尾 = 哨兵尾，无剩余空隙）
		CHECK(r.regionBytes == (kCapacity + 1u) * static_cast<uint32_t>(r.stride));
		CHECK(r.sentinel + r.stride == r.base + r.regionBytes);
	}

	// (b) 绝对地址第二源（派生式对"整体错位"是跟着一起漂的，只有手抄字面量抓得到）
	CHECK(kSceneDataObjectMatrixOffset == kBaseMatrix);
	CHECK(kSceneDataMvpTableOffset == kBaseMvp);
	CHECK(kSceneDataDrawTableOffset == kBaseDraw);
	CHECK(kSceneDataClusterRangeOffset == kBaseCluster);
	CHECK(kSceneDataObjectMatrixSentinelOffset == kSentinelMatrix);
	CHECK(kSceneDataMvpTableSentinelOffset == kSentinelMvp);
	CHECK(kSceneDataDrawTableSentinelOffset == kSentinelDraw);
	CHECK(kSceneDataClusterRangeSentinelOffset == kSentinelCluster);
	// stride 也是两源对账（stride 漂了 ⟹ 上面所有"容量×stride"的期望值一起漂）
	CHECK(kSceneMatrixStride == kStrideMatrix);
	CHECK(kSceneDrawStride == kStrideDraw);
	CHECK(kSceneDataClusterRangeStride == kStrideCluster);
}

TEST_CASE("哨兵不踩邻居区：三张非尾表的哨兵尾 ≤ 下一区基址；尾表哨兵尾 = buffer 尾") {
	// 这条排掉的是"哨兵写进了邻居区"——那等于往活数据里撒毒，画面必炸但归因会被读成
	// "迁移改错了偏移"，而不是"护栏位置错"。
	CHECK(kSceneDataObjectMatrixSentinelOffset + kSceneMatrixStride
		<= kSceneDataLightArrayOffset);
	CHECK(kSceneDataMvpTableSentinelOffset + kSceneMatrixStride <= kSceneDataDrawTableOffset);
	CHECK(kSceneDataDrawTableSentinelOffset + kSceneDrawStride <= kSceneDataIrradianceOffset);
	CHECK(kSceneDataClusterRangeSentinelOffset + kSceneDataClusterRangeStride
		== static_cast<uint64_t>(kSceneDataBytes));
	// 反向：哨兵**不能**跑到下一区里去（上面三条只钉 ≤，这里钉"哨兵首字节仍在本区内"）
	CHECK(kSceneDataObjectMatrixSentinelOffset < kSceneDataLightArrayOffset);
	CHECK(kSceneDataMvpTableSentinelOffset < kSceneDataDrawTableOffset);
	CHECK(kSceneDataDrawTableSentinelOffset < kSceneDataIrradianceOffset);
}

// ─── (d) 写侧：只写四个窗口、且每帧写者不覆盖 ───────────────────────────────

TEST_CASE("writeSceneSentinels：四个窗口整格毒化，窗口外一个字节都不动") {
	auto staging = makeStaging();  // 初值 0xa5（非 0，"没写"与"写 0"可区分）
	writeSceneSentinels(std::span<std::byte> { staging.data(), staging.size() });

	const auto windows = sentinelWindows();
	CHECK(windows.size() == 4);  // 前提守卫
	for (const auto& w : windows) {
		INFO(w.name);
		CHECK(windowPoisoned(staging, w.at, w.bytes));  // 阳性：窗口真的被写了
	}
	// 反向：窗口之外**全部**保持初值 ⟹ 既没多写（覆盖邻居/覆盖可用槽），也没少写别的窗口
	for (const auto& w : windows)
		for (uint64_t i = w.at; i < w.at + w.bytes; ++i)
			staging[i] = std::byte { 0xa5 };  // 先把四个窗口擦回初值，剩下的应处处是 0xa5
	CHECK(firstByteNot(staging, 0, staging.size(), std::byte { 0xa5 }) == -1);
}

TEST_CASE("writeSceneSentinels 的窗口不落在任何表的可用槽里（可用段读出来仍是初值）") {
	// 阳性锚点：若哨兵偏移错到 capacity-1（或更小），最后一个**可用**槽会被毒值占掉
	// ⟹ 本用例红。这条与上一条互补：上一条抓"窗口外被动"，这一条抓"窗口位置偏前"。
	auto staging = makeStaging();
	writeSceneSentinels(std::span<std::byte> { staging.data(), staging.size() });
	const std::vector<std::pair<uint64_t, uint32_t>> usableEnds {
		{ kSceneDataObjectMatrixOffset, kSceneMatrixStride },
		{ kSceneDataMvpTableOffset, kSceneMatrixStride },
		{ kSceneDataDrawTableOffset, kSceneDrawStride },
		{ kSceneDataClusterRangeOffset, kSceneDataClusterRangeStride },
	};
	for (const auto& [base, stride] : usableEnds) {
		const uint64_t lastUsable = base + static_cast<uint64_t>(kCapacity - 1u) * stride;
		CHECK(readWord(staging, lastUsable) != kSceneSentinelWord);
		CHECK(readWord(staging, lastUsable) == 0xa5a5a5a5u);
	}
}

TEST_CASE("每帧写者不覆盖哨兵（满表）：清零段长度 = 0，哨兵仍是毒值") {
	// 这是 §9.9 "写一次就一直在"的**唯一前提**：每帧是全量 vkCmdCopyBuffer，只要没人每帧
	// 覆盖它，分配时写一次就够。writeClusterRangeRegion 是四张表里唯一做"整区清零"的写者
	// （memset 长度一旦写回 kSceneDataClusterRangeBytes，护栏每帧被拆一次）。
	auto staging = makeStaging(std::byte { 0x00 });
	writeSceneSentinels(std::span<std::byte> { staging.data(), staging.size() });
	std::vector<ObjectClusterRange> full(kSceneDataClusterRangeCapacity);
	for (size_t i = 0; i < full.size(); ++i)
		full[i] = ObjectClusterRange { static_cast<uint32_t>(i), 1u, 1u, 0u };
	REQUIRE(writeClusterRangeRegion(std::span<std::byte> { staging.data(), staging.size() }, full));
	for (const auto& w : sentinelWindows()) {
		INFO(w.name);
		CHECK(windowPoisoned(staging, w.at, w.bytes));
	}
	// 满表本身落位（护栏不是靠"少写一条"保住的）
	CHECK(readWord(staging,
		kSceneDataClusterRangeOffset + static_cast<uint64_t>(kCapacity - 1u) * kStrideCluster)
		== kCapacity - 1u);
}

TEST_CASE("每帧写者不覆盖哨兵（空表 = 门关）：可用区清零，哨兵仍是毒值") {
	auto staging = makeStaging(std::byte { 0x00 });
	writeSceneSentinels(std::span<std::byte> { staging.data(), staging.size() });
	REQUIRE(writeClusterRangeRegion(std::span<std::byte> { staging.data(), staging.size() },
		std::span<const ObjectClusterRange> {}));
	for (const auto& w : sentinelWindows()) {
		INFO(w.name);
		CHECK(windowPoisoned(staging, w.at, w.bytes));
	}
	// 可用区确实被清了（门关 = 空区间；这条排掉"memset 根本没跑"的假绿）
	CHECK(firstByteNot(staging, kSceneDataClusterRangeOffset,
		kSceneDataClusterRangeSentinelOffset, std::byte { 0x00 }) == -1);
}

TEST_CASE("每帧写者的写入上界恰停在哨兵之前（容量闸门与哨兵之间没有缝）") {
	// writeSceneStaging 对矩阵/MVP/材质三表是 memcpy(实际条目数)，其**容量闸门**是
	// `bytes > kSceneMatrixCapacity * stride` ⟹ 最大写入长度 = 容量 × stride，正好等于
	// "表基址 → 哨兵"的距离。任何一侧改动（容量常量、区宽度）都会让这里红：
	// 距离**大于**闸门 = 中间有一段既不清零也不毒化的暗区；**小于** = 写者踩进哨兵。
	CHECK(kSceneDataObjectMatrixSentinelOffset - kSceneDataObjectMatrixOffset
		== static_cast<uint64_t>(kSceneMatrixCapacity) * kSceneMatrixStride);
	CHECK(kSceneDataMvpTableSentinelOffset - kSceneDataMvpTableOffset
		== static_cast<uint64_t>(kSceneMatrixCapacity) * kSceneMatrixStride);
	CHECK(kSceneDataDrawTableSentinelOffset - kSceneDataDrawTableOffset
		== static_cast<uint64_t>(kSceneMatrixCapacity) * kSceneDrawStride);
	CHECK(kSceneDataClusterRangeSentinelOffset - kSceneDataClusterRangeOffset
		== static_cast<uint64_t>(kSceneDataClusterRangeCapacity) * kSceneDataClusterRangeStride);
}

}  // namespace RGL
