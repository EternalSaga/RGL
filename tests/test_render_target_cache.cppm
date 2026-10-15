module;
/**
 * test_render_target_cache —— RenderTargetCache 的**多描述符 / 跨重建槽复用**契约
 *（EVSSM 矩图迁入 RT 缓存那一刀的回归网）。
 *
 * ## 为什么整份文件在 gpu 套件
 *
 * 被测面 = 「向 descriptor heap 领号 + 写描述符 + 建 VMA 图」，判据要一个真 VkDevice
 * 才有答案（`just test` 的无设备子集测不到，理由与分流纪律见 tests/CMakeLists.txt
 * 文件头）。**无设备机器上本文件是 FAIL 不是 SKIP**——静默跳过 = 假绿。
 *
 * ## 为什么这里自建 AppContext→DeviceContext→Swapchain→Resources 全栈
 *
 * `test_meshlet_shadow_args` 文件头缺口 ② 明确反对"为了测一个服务去拖进整个引擎装配"，
 * 那条判断对它成立（它测的是纯 CPU 切片）。本文件不同：**被测者本身就是"要 Resources
 * 的那一层"**——RenderTargetCache 的三个依赖（allocator / descriptorHeap / allocSlot）
 * 全在 Resources 上，绕开它就得在测试里重造一套 heap + 槽台账，那才是"重造一条更干净
 * 但更假的路"。Swapchain 走 headless（VK_EXT_headless_surface，不建窗口），代价 =
 * 本文件继承引擎的硬性前置（VVL + 9 项设备扩展），与 test_device_primitives 同口径。
 *
 * ## 变异红线（哪条变异让我红；答不出 = 摆设）
 *
 *  - **M1 去掉重建路径的「跨重建复用槽」守卫**（`render_target_cache.cppm` 里
 *    `else if (firstBuild)` 改成 `else if (true)`，即每次重建重新领号）
 *    → 用例 2「跨重建复用」的逐槽相等断言**必须红**（同时用例 3 的领号计数也红）。
 *    这条同时证明用例 2 **不是恒真**：它的输入确实触发了重建（generation 断言在场）。
 *  - **M2 `heapSlot(name)` 退化成"取第 0 项"**（`sampleDescriptor()` 改成
 *    `descriptors.empty() ? nullptr : &descriptors[0]`）→ 用例 1 **必须红**：
 *    规格里的采样项刻意放在 index 1，取第 0 项会拿到一个 storage 槽。
 *  - **M3 预发号被忽略、缓存照领新号**（`if (dspec.heapSlot)` 改成 `if (false)`）
 *    → 用例 3「采样项拿到的就是台账预发的那个号」+ 领号计数**必须红**。
 *  - **M4 重建后不更新描述符**（把描述符循环整体包进 `if (firstBuild)`）
 *    → 用例 2 的「重建后 viewCreate.image == 新图句柄」**必须红**（复用槽 ≠ 什么都没做）。
 *  - **M5 规格漂移守卫短路**（项数对账改成永真放行）→ 用例 4 的漂移报错断言红，
 *    且"报错后附件未被改动"的半边同时红（两条半边各守一种坏法）。
 *  - **M6（B2-4③′ 债-124）erase 不还槽 / 把预发号槽也还了** → 用例 5 的最小号复用
 *    与"预发号不被领回"断言红（不还 = 槽泄漏；乱还 = 别人的槽被发两次）。
 *  - **M7（B2-4③′）retire 立刻销毁 / 台账不拦重复 retire / generation 地板缺失**
 *    → 用例 6 的对应断言红（in-flight UAF / 销毁记账被吞 / 新图带旧布局值跑）。
 *  - **M8（B2-4③′ resize 冒烟实测抓出的洞）退役重建不继承描述符链**（`ensure` 里
 *    「同名退役条目在场 → move 其 descriptors」的继承块删掉）→ 用例 7 的槽号相等
 *    与链地址相等断言**必须红**（真程序症状：light pass 的描述符集仍指退役节点的链，
 *    退役窗口内持续采旧尺寸 G-buffer、due 后悬空 = resize 后整幅横向压缩 720/800）。
 *
 * 反向用例（"不该触发的不触发"）：同 extent 二次 ensure 不 bump generation（用例 2 末段）、
 * 未 ensure 的名字 → nullopt（用例 1 末段）、合法单采样项规格不报错（用例 4 前半）。
 */
#include <doctest/doctest.h>
#include <cstdint>
#include <initializer_list>  // 🔴 modules 坑：`spec.descriptors = {a, b, …}` 要真 STL 头
#include <optional>
#include <vector>

export module test_render_target_cache;

import rlstd;
import rlvk;
import vkerror;
import vk_instance;    // AppContext（headless 引导）
import vk_device;      // DeviceContext
import vk_swapchain;   // Swapchain（headless = 不建窗口，Resources 的 extent 来源）
import vulkan_window;  // SDLWindow 前置类型（Swapchain 形参，本层传 nullptr）
import vk_resources;   // Resources（allocator + descriptor heap + 分用途段领槽）
import render_target_cache;  // 被测者：RenderTargetCache / RtAttachmentSpec / RtDescriptor

namespace RGL::testing::rtcache {

/** 最小值（modules 坑：rlstd 不导出 <algorithm>，std::ranges::min_element 在测试 TU 里不可见；手写，先例 = Resources::allocSlot）。 */
uint32_t minOf(const std::vector<uint32_t>& v) {
	uint32_t m = ~uint32_t{0};
	for (const auto x : v)
		m = x < m ? x : m;
	return m;
}

/**
 * headless 引擎栈（构造序 = 成员声明序 = 依赖序；析构反序 ⟹ 缓存与 Resources
 * 先于 device 销毁，VMA/heap 不会拿到已销毁的 VkDevice）。
 *
 * @throws 无可用设备 / 缺 VK_LAYER_KHRONOS_validation / 缺 required 设备扩展
 *   —— 一律向上抛，doctest 记失败（**不 skip**，理由见文件头）。
 */
export struct EngineStack {
	AppContext app{nullptr, /*headless=*/true};
	DeviceContext dev{app};
	Swapchain sc{dev, nullptr, /*headless=*/true, 64, 64};
	Resources resources{app, dev, sc};
	RenderTargetCache cache{resources, dev};
};

constexpr vk::Format kFmt = vk::Format::eR16G16B16A16Sfloat;  // 与 EVSSM 矩图同格式
constexpr vk::Extent2D kExtentA{16, 16};
constexpr vk::Extent2D kExtentB{32, 24};  // 与 A **两轴都不同**（单轴相同会被"只比一轴"蒙过）

/** 一个 per-mip storage 描述符规格（GENERAL，与矩图同形态）。 */
RGL::RtDescriptorSpec storageItem(uint32_t mip) {
	return RGL::RtDescriptorSpec{
		.range = {vk::ImageAspectFlagBits::eColor, mip, 1, 0, 1},
		.type = vk::DescriptorType::eStorageImage,
		.layout = vk::ImageLayout::eGeneral,
		.heapSlot = std::nullopt,
	};
}

/** 一个全 mip sampled 描述符规格；slot 有值 = 外部台账预发号。 */
RGL::RtDescriptorSpec sampledItem(uint32_t mipLevels, std::optional<uint32_t> slot) {
	return RGL::RtDescriptorSpec{
		.range = {vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1},
		.type = vk::DescriptorType::eSampledImage,
		.layout = vk::ImageLayout::eGeneral,
		.heapSlot = slot,
	};
}

/**
 * 多描述符规格：4 个 per-mip storage + 1 个全 mip sampled。
 * 🔴 sampled 刻意放在 **index 1**（不是首项也不是末项）——"取第 0 项"的实现必须红。
 */
RGL::RtAttachmentSpec multiSpec(std::optional<uint32_t> sampledSlot = std::nullopt) {
	RGL::RtAttachmentSpec spec{};
	spec.mipLevels = 4;
	spec.usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
	spec.dedicatedMemory = true;  // 与矩图同档（顺带覆盖这个 flag 的创建路径）
	spec.descriptors = {
		storageItem(0), sampledItem(4, sampledSlot), storageItem(1), storageItem(2), storageItem(3),
	};
	return spec;
}

std::vector<uint32_t> slotsOf(const RGL::RtAttachment& rt) {
	std::vector<uint32_t> out;
	for (const auto& d : rt.descriptors)
		out.push_back(d.heapSlot);
	return out;
}

uint32_t takeSlot(RGL::Resources& resources, const char* name) {
	const auto slot = resources.allocSlot(RGL::SlotSegment::Runtime, name);
	REQUIRE(slot.has_value());
	return *slot;
}

}  // namespace RGL::testing::rtcache

// ═══════════════════════════════════════════════════════════════════════════
// 用例
// ═══════════════════════════════════════════════════════════════════════════

TEST_SUITE("gpu") {

using RGL::testing::rtcache::EngineStack;
using RGL::testing::rtcache::kExtentA;
using RGL::testing::rtcache::kExtentB;
using RGL::testing::rtcache::kFmt;
using RGL::testing::rtcache::multiSpec;
using RGL::testing::rtcache::minOf;
using RGL::testing::rtcache::slotsOf;
using RGL::testing::rtcache::takeSlot;

TEST_CASE("RT 缓存多描述符：逐项独立领号 + 类型对位 + heapSlot 指名 sampled 项") {
	EngineStack engine;
	const auto spec = multiSpec();
	const auto made = engine.cache.ensure("test.multi", kExtentA, kFmt, spec);
	REQUIRE(made.has_value());
	RGL::RtAttachment* rt = *made;

	REQUIRE(rt->descriptors.size() == spec.descriptors.size());
	// ① 逐项：类型 / layout / 子资源范围与规格对位，槽号落在 Runtime 段
	const auto runtimeBegin = engine.resources.segmentBegin(RGL::SlotSegment::Runtime);
	for (size_t i = 0; i < spec.descriptors.size(); ++i) {
		INFO("descriptor index " << i);
		CHECK(rt->descriptors[i].type == spec.descriptors[i].type);
		CHECK(rt->descriptors[i].layout == spec.descriptors[i].layout);
		CHECK(rt->descriptors[i].range == spec.descriptors[i].range);
		CHECK(rt->descriptors[i].heapSlot >= runtimeBegin);
		// ② 描述符指针链指向**本项内部**（vector 一旦 resize 这两条就断 = 交给驱动的是野指针）
		CHECK(rt->descriptors[i].imageDesc.pView == &rt->descriptors[i].viewCreate);
		CHECK(rt->descriptors[i].resourceDesc.data.pImage == &rt->descriptors[i].imageDesc);
		CHECK(rt->descriptors[i].viewCreate.image == rt->image.image);
	}
	// ③ 槽号两两不同（"5 个描述符共用一个槽"是最坏坏法：降采样链会读写同一个视图）
	const auto slots = slotsOf(*rt);
	for (size_t i = 0; i < slots.size(); ++i)
		for (size_t j = i + 1; j < slots.size(); ++j) {
			INFO("slot index pair (" << i << "," << j << ")");
			CHECK(slots[i] != slots[j]);
		}
	// ④ heapSlot(name) 返回的是**采样项**那个槽，而不是第 0 项（采样项在 index 1）
	const auto sampled = engine.cache.heapSlot("test.multi");
	REQUIRE(sampled.has_value());
	CHECK(*sampled == slots[1]);
	CHECK(*sampled != slots[0]);
	CHECK(rt->descriptors[1].type == vk::DescriptorType::eSampledImage);
	// ⑤ 反向：未 ensure 的名字 → nullopt（区分"没建"与"建坏了"）
	CHECK_FALSE(engine.cache.heapSlot("test.never-ensured").has_value());
	CHECK(engine.cache.find("test.never-ensured") == nullptr);
}

TEST_CASE("RT 缓存跨重建：同 name 换 extent → generation 变而每个描述符 heapSlot 不变") {
	EngineStack engine;
	const auto spec = multiSpec();
	const auto first = engine.cache.ensure("test.resize", kExtentA, kFmt, spec);
	REQUIRE(first.has_value());
	const uint64_t gen0 = (*first)->generation;
	const std::vector<uint32_t> slots0 = slotsOf(**first);
	const auto sampled0 = engine.cache.heapSlot("test.resize");
	REQUIRE(sampled0.has_value());

	// 🔴 判据的**非恒真前提**：这次 ensure 必须真的重建（extent 两轴都换了）。
	// 若它其实命中复用，"槽没变"就恒真、M1 变异也不会红——先钉住激励再断言。
	const auto second = engine.cache.ensure("test.resize", kExtentB, kFmt, spec);
	REQUIRE(second.has_value());
	CHECK((*second)->extent == kExtentB);
	CHECK((*second)->generation == gen0 + 1);
	CHECK(*second == *first);  // map 节点地址稳定（消费方持的指针跨重建仍有效）

	// 判据本体：**每个** descriptor 的 heapSlot 跨重建复用（本次迁入的结构性收益来源：
	// 烘进管线条目的槽号因此永久有效。不成立 = 白搬）
	CHECK(slotsOf(**second) == slots0);
	const auto sampled1 = engine.cache.heapSlot("test.resize");
	REQUIRE(sampled1.has_value());
	CHECK(*sampled1 == *sampled0);
	// 复用槽 ≠ 什么都没做：描述符必须已改指**新图**（否则画面读旧 VA）
	for (size_t i = 0; i < (*second)->descriptors.size(); ++i) {
		INFO("descriptor index " << i);
		CHECK((*second)->descriptors[i].viewCreate.image == (*second)->image.image);
		CHECK((*second)->descriptors[i].imageDesc.pView == &(*second)->descriptors[i].viewCreate);
	}

	// 反向半边：同 extent 再 ensure 一次 = 命中，不 bump generation、不换槽
	const auto third = engine.cache.ensure("test.resize", kExtentB, kFmt, spec);
	REQUIRE(third.has_value());
	CHECK((*third)->generation == gen0 + 1);
	CHECK(slotsOf(**third) == slots0);
}

TEST_CASE("RT 缓存预发号：采样项用外部台账的号，缓存不为它多领一个槽") {
	EngineStack engine;
	// 时序坑的单元形态：装配期先拿号（管线条目 bake），图首帧才建。
	// 探针槽 = 台账游标标记：allocSlot 段内单调 ⟹ 两次探针之间拿走的槽数可数出来。
	const uint32_t probeBefore = takeSlot(engine.resources, "test.probe.before");
	const uint32_t preIssued = takeSlot(engine.resources, "test.presample");

	const auto spec = multiSpec(preIssued);  // 5 项里只有 sampled 带预发号
	const auto made = engine.cache.ensure("test.preissued", kExtentA, kFmt, spec);
	REQUIRE(made.has_value());
	const auto sampledSlot = engine.cache.heapSlot("test.preissued");
	REQUIRE(sampledSlot.has_value());
	CHECK(*sampledSlot == preIssued);
	CHECK((*made)->descriptors[1].heapSlot == preIssued);

	// 计数判据：probeAfter − probeBefore = 预发号 1 + 缓存新领 4 + 探针自身 1 = **6**。
	// M3（忽略预发号 → 给采样项也领新号）会让它变成 7；M1（重建重新领号）不影响本用例
	// （这里没有重建），所以两条变异各红各的用例，不是一条万能红。
	const uint32_t probeAfter = takeSlot(engine.resources, "test.probe.after");
	CHECK(probeAfter == probeBefore + 6u);
	// 反向：预发号项的类型仍是采样项（"多领一个槽"最常见的形态是采样项换成新号）
	CHECK((*made)->descriptors[1].type == vk::DescriptorType::eSampledImage);
}

TEST_CASE("RT 缓存规格漂移：多于一个采样项 / 换描述符规格 → 报错且不动已有附件") {
	EngineStack engine;
	// 正向半边：一个采样项 = 合法（否则"报错"这条判据可以靠"永远报错"通过）
	const auto ok = engine.cache.ensure("test.drift", kExtentA, kFmt, multiSpec());
	REQUIRE(ok.has_value());
	const uint64_t gen = (*ok)->generation;
	const auto extent = (*ok)->extent;
	const auto slots = slotsOf(**ok);

	// ① 两个采样项：heapSlot(name) 无法指名"那个采样槽"，必须点名而不是取第一个
	auto twoSampled = multiSpec();
	twoSampled.descriptors[3] = twoSampled.descriptors[1];
	const auto badTwo = engine.cache.ensure("test.drift", kExtentA, kFmt, twoSampled);
	REQUIRE_FALSE(badTwo.has_value());
	CHECK(badTwo.error().describe().find("SAMPLED_IMAGE") != std::string::npos);

	// ② 项数漂移（少一项 = 会让 descriptors vector resize = 指针链悬空的形态）
	auto fewer = multiSpec();
	fewer.descriptors.pop_back();
	const auto badCount = engine.cache.ensure("test.drift", kExtentA, kFmt, fewer);
	REQUIRE_FALSE(badCount.has_value());

	// ③ 同项数换 layout（"下次重建悄悄换布局"的形态）
	auto relaid = multiSpec();
	relaid.descriptors[0].layout = vk::ImageLayout::eShaderReadOnlyOptimal;
	const auto badLayout = engine.cache.ensure("test.drift", kExtentA, kFmt, relaid);
	REQUIRE_FALSE(badLayout.has_value());

	// 报错半边：三次失败都**没有**动已有附件（不留下"图已换、描述符没写"的半态）
	RGL::RtAttachment* rt = engine.cache.find("test.drift");
	REQUIRE(rt != nullptr);
	CHECK(rt->generation == gen);
	CHECK(rt->extent == extent);
	CHECK(slotsOf(*rt) == slots);
}

// ── B2-4③′ 债-124：逐出路径（erase / retire 窗口 / 堆槽归还 / generation 地板）──────

TEST_CASE("RT 缓存 erase：毁图 + 归还自有槽（allocSlot 最小号复用）；预发号槽不归还") {
	EngineStack engine;
	// 预发号一项（外部台账所有）+ 4 个缓存自有项（multiSpec 的 sampled 留空 = 也自有；
	// 用带预发号的规格区分两类归属）
	const uint32_t preIssued = takeSlot(engine.resources, "test.erase.pre");
	const auto spec = multiSpec(preIssued);
	const auto made = engine.cache.ensure("test.erase", kExtentA, kFmt, spec);
	REQUIRE(made.has_value());
	const auto slots = slotsOf(**made);
	const auto erased = engine.cache.erase("test.erase");
	CHECK(erased);
	CHECK(engine.cache.find("test.erase") == nullptr);
	// 反向：再 erase 同一个名字 = false（名不存在，不许静默成功）
	CHECK_FALSE(engine.cache.erase("test.erase"));

	// 🔴 归还判据：allocSlot **最小号优先**复用 ⟹ 下一个领到的槽就是刚还的自有槽里
	// 最小的那个；预发号槽**不在**归还集合里（归属没变，还了就是把别人的槽发两次）。
	const uint32_t smallestOwned = minOf({slots[0], slots[2], slots[3], slots[4]});
	const auto reused = engine.resources.allocSlot(RGL::SlotSegment::Runtime, "test.erase.reuse");
	REQUIRE(reused.has_value());
	CHECK(*reused == smallestOwned);
	// 预发号槽没被归还的机器证据：再领一次**不会**拿到它（它比 smallestOwned 小或大
	// 都不重要——重要的是归还集合里没有它）。取规格首项槽做对照：若实现把预发号也还了，
	// 连续领两次会先领到 preIssued（它更小）或把它混进台账；这里领两次断言都不是它。
	const auto second = engine.resources.allocSlot(RGL::SlotSegment::Runtime, "test.erase.reuse2");
	REQUIRE(second.has_value());
	CHECK(*second != preIssued);
}

TEST_CASE("RT 退役窗口：retire 立刻摘活动表；due 前销毁不落账、重复 retire 炸；due 后还槽") {
	EngineStack engine;
	const auto made = engine.cache.ensure("test.retire", kExtentA, kFmt, multiSpec());
	REQUIRE(made.has_value());
	const uint64_t gen = (*made)->generation;
	const auto slots = slotsOf(**made);

	// ① retire = **立刻**从活动表摘除（下帧 ensure 按新尺寸重建的语义前提），
	//    但 GPU 资源进退役区（本用例不查 GPU 侧，查的是台账行为）。
	REQUIRE(engine.cache.retire("test.retire", 10).has_value());
	CHECK(engine.cache.find("test.retire") == nullptr);
	CHECK(engine.cache.retiredPending("test.retire"));

	// ② 🔴「有 in-flight 用户逐出必须炸」：同名未到期再次 retire = Error（消息点名）。
	const auto dup = engine.cache.retire("test.retire", 20);
	REQUIRE_FALSE(dup.has_value());
	CHECK(dup.error().describe().find("test.retire") != std::string::npos);

	// ③ 未到期收割 = 什么都不交（差一判据在 PendingEvictions 单测钉，这里钉缓存侧）。
	engine.cache.processRetired(9);
	CHECK(engine.cache.retiredPending("test.retire"));

	// ④ 到期收割 = 销毁 + 还槽（最小号复用可观测）。
	engine.cache.processRetired(10);
	CHECK_FALSE(engine.cache.retiredPending("test.retire"));
	const uint32_t smallestOwned = minOf(slots);
	const auto reused = engine.resources.allocSlot(RGL::SlotSegment::Runtime, "test.retire.reuse");
	REQUIRE(reused.has_value());
	CHECK(*reused == smallestOwned);

	// ⑤ generation 地板：同名重建的 generation 必须**严格大于**退役前值（Renderer 的
	// 跨帧快照靠它失配触发布局重置；相撞 = 新图带旧布局值跑，静默错布局）。
	const auto rebuilt = engine.cache.ensure("test.retire", kExtentB, kFmt, multiSpec());
	REQUIRE(rebuilt.has_value());
	CHECK((*rebuilt)->generation > gen);
}

TEST_CASE("RT 退役重建继承描述符链：due 前同名 ensure 复用槽号 + 链缓冲地址不变（消费方指针链不断）") {
	// resize 冒烟实测抓出的洞（M8）：extent 漂移 retire 后，同帧 ensure 新尺寸**不能领新号**——
	// 消费方（light pass 描述符集）指着的指针链活在旧 descriptors vector 的数据缓冲里，
	// 领新号 = 消费方继续采退役区那份旧尺寸图、due 后彻底悬空。真程序症状 = resize 后
	// 整幅画面横向压缩 720/800（light pass 采旧 720 G-buffer）。
	EngineStack engine;
	const auto made = engine.cache.ensure("test.inherit", kExtentA, kFmt, multiSpec());
	REQUIRE(made.has_value());
	const void* bufBefore = (*made)->descriptors.data();  // vector 数据缓冲地址（指针链的真身）
	const auto slotsBefore = slotsOf(**made);

	REQUIRE(engine.cache.retire("test.inherit", 10).has_value());

	// due 前同名重建（真实 resize 就是 retire 与 ensure 同帧）：必须**继承**描述符链。
	const auto rebuilt = engine.cache.ensure("test.inherit", kExtentB, kFmt, multiSpec());
	REQUIRE(rebuilt.has_value());
	// ① 每个描述符 heapSlot 与退役前逐一相等（= 没领新号，消费方 baked 的槽号继续有效）。
	CHECK(slotsOf(**rebuilt) == slotsBefore);
	// ② vector 数据缓冲地址不变（move 转移缓冲）——消费方指针链指向的字节还是那块，
	//    只是内容被 syncDescriptors 原地换成新 view。这条是「无需改写消费方」的结构性前提。
	CHECK((*rebuilt)->descriptors.data() == bufBefore);
	// ③ 新图 extent 已换、generation 递增（布局重置照触发）。
	CHECK((*rebuilt)->extent == kExtentB);

	// ④ 反向：继承的槽**没被还进 free-list**（否则下一个 allocSlot 最小号复用会领回它 =
	//    同一槽发两次）。到期收割只毁旧图，不还已被继承的槽。
	engine.cache.processRetired(10);
	CHECK_FALSE(engine.cache.retiredPending("test.inherit"));
	const auto probe = engine.resources.allocSlot(RGL::SlotSegment::Runtime, "test.inherit.probe");
	REQUIRE(probe.has_value());
	for (const auto s : slotsBefore)
		CHECK(*probe != s);  // 领到的新号不等于任何继承槽 = 它们没被错误归还
}

}  // TEST_SUITE("gpu")
