module;
#include <doctest/doctest.h>
#include <initializer_list>  // modules 环境坑：initializer_list 不从 import 带进来（⑥ 的 vector 列表初始化要）

// SlotLedger（sampler heap 申报制领槽）契约测试 —— 被测 = rgl/slot_ledger.cppm（rlbase，
// 纯逻辑无设备依赖，`just test` 毫秒级）。
//
// ## 为什么值得一条单测（2026-10-04 env sampler 槽相撞事故）
//
// sampler heap 用户槽原本由每个使用者自算 `reservedSlots + N`：`Environment::ensureSkySampler`
// 与 `SamplerLibrary::ensurePointSampler` 双双算出 `+1`（两处注释都自称"共享 sampler 之后
// 第二槽"，谁都没错），而两个 ensure* 的幂等守卫让**后写者永久赢** ⟹ env 镜面项拿到 AO 的
// NEAREST + maxLod=0 sampler ⟹ GGX 预滤波 mip 链从未被采样（判据：`env.mipRange` 扫 0..20
// 画面**逐字节不变**，而同一次 memcpy 里的 `env.intensity` 活得好好的）。
//
// 那条 bug 在渲染侧的签名很重（IBL 镜面项全错、粗糙金属偏暗），但在**代码侧**的签名只有
// "两个整数相等"——没有任何编译期/运行期机制会发现。本测试钉的就是那三个本该存在的不变式。
//
// ## 契约来源（出处独立于实现：模块头注释 §三条不变式 + VmaBuffer.cppm:169 reserved 语义）
//   · 同一 owner 重复领 → 恒返回同一槽（幂等，保住调用方撤掉守卫后的行为）；
//   · 不同 owner → 槽号两两不同（**护栏本体**：相撞在结构上不可能）；
//   · 超容量 → Error 且**不消耗槽号**、既有申报不受影响（先查界后发号）；
//   · 基址 = heap reservedSlots：驱动保留区 [0, reserved) 声明为不可用（VmaBuffer.cppm:157-159
//     "不可使用"），领进去 = 覆写驱动数据；
//   · 分配顺序 = 首次申报顺序（确定性契约：槽号会烘进管线 envSamplerHeapOffset）。
//
// ## 变异红线（哪个变异让我红；答不出 = 摆设）
//   · 幂等分支删掉（不再查 claims，直接发新槽）        → ① 红、③ 红（失败后既有申报漂移）
//   · 幂等分支改成"恒返回首槽"（忽略 owner 不同）      → ② 红（**正是本次事故的行为**）
//   · 容量检查从 `index >= slotCount` 松成 `>`（越界一格）→ ③ 红（返回 slot == base+capacity）
//   · 先发号后查界（push_back 在检查之前）             → ③ 红（used() 变 3、失败消耗槽号）
//   · 忽略 base（恒从 0 发号）                        → ④ 红（NVIDIA reserved=3024 领进保留区）
//   · owner 比较换成前缀 / 大小写不敏感                → ⑤ 红（"ao" 误命中 "aoPoint"）
//   · 台账换成哈希表（迭代序不定）                     → ⑥ 红（同序列重放表不一致）
//   · capacity=0 时仍发号                             → ⑦ 红
export module test_slot_ledger;

import rlstd;
import slot_ledger;
import vkerror;

namespace RGL {

TEST_CASE("SlotLedger ①：同一 owner 重复领恒返回同一槽（幂等，不消耗新槽）") {
	SlotLedger ledger(0u, 8u);

	const auto first = ledger.claimSlot("sampler.skyEnv");
	REQUIRE(first.has_value());
	const auto again = ledger.claimSlot("sampler.skyEnv");
	REQUIRE(again.has_value());

	// 阳性锚点：若幂等从未生效（每次都发新槽），again 会是 1 ≠ 0 ⟹ 本条红。
	CHECK(*first == 0u);
	CHECK(*again == *first);
	// 幂等不得推进台账：重复领不是"占第二个槽"。
	CHECK(ledger.used() == 1u);

	// 幂等在容量已满时依然生效（否则"重复领"会被误报成"容量不足"——那是把幂等
	// 与分配混成一件事，调用方 ensure* 的守卫撤掉后就会变成启动期崩溃）。
	SlotLedger full(0u, 1u);
	REQUIRE(full.claimSlot("sampler.shared").has_value());
	const auto fullAgain = full.claimSlot("sampler.shared");
	REQUIRE(fullAgain.has_value());
	CHECK(*fullAgain == 0u);
}

TEST_CASE("SlotLedger ②：不同 owner 永不拿到同一个槽（事故回归本体）") {
	// 三个归属者 = 现行引擎的真实三个 sampler（名字逐字取自 sampler_library / environment）。
	SlotLedger ledger(0u, 8u);

	const auto shared = ledger.claimSlot("sampler.shared");
	const auto point = ledger.claimSlot("sampler.aoPoint");
	const auto sky = ledger.claimSlot("sampler.skyEnv");

	// 反向用例：三次都必须成功（若"报错"其实是"全错"，下面的相等性断言毫无意义）。
	REQUIRE(shared.has_value());
	REQUIRE(point.has_value());
	REQUIRE(sky.has_value());

	// ★ 事故签名：sky 与 point 曾各自算出 reservedSlots + 1。这条就是那条 bug 的墓碑。
	CHECK(*sky != *point);
	CHECK(*shared != *point);
	CHECK(*shared != *sky);

	// 首申报顺序 = 递增槽号（确定性：不是哈希序、不是地址序）。
	CHECK(*shared == 0u);
	CHECK(*point == 1u);
	CHECK(*sky == 2u);
	CHECK(ledger.used() == 3u);
}

TEST_CASE("SlotLedger ③：超容量返回 Error 且不静默越界、不消耗槽号、既有申报不受影响") {
	SlotLedger ledger(4u, 2u);  // 可领 [4, 6)

	// 反向用例：容量内的两次必须成功。
	const auto a = ledger.claimSlot("a");
	const auto b = ledger.claimSlot("b");
	REQUIRE(a.has_value());
	REQUIRE(b.has_value());
	CHECK(*a == 4u);
	CHECK(*b == 5u);

	// 第三次越界：必须**响**（返回 Error），而不是把 6 发出去。
	const auto c = ledger.claimSlot("c");
	CHECK_FALSE(c.has_value());
	CHECK(c.error().code == ErrorCode::DescriptorSlotOutOfBounds);
	CHECK(c.error().message.find("c") != std::string::npos);  // 报错要点名是谁领不到

	// 失败不得消耗槽号（"先发号后查界"的变异在这里红：used() 会变成 3）。
	CHECK(ledger.used() == 2u);
	CHECK(ledger.capacity() == 2u);

	// 失败不得污染既有申报（幂等在失败之后仍然成立）。
	const auto aAgain = ledger.claimSlot("a");
	REQUIRE(aAgain.has_value());
	CHECK(*aAgain == 4u);

	// 任何时刻都没有发过越界槽号。
	for (const auto& claim : ledger.entries())
		CHECK(claim.slot < ledger.base() + ledger.capacity());
}

TEST_CASE("SlotLedger ④：基址 = heap reservedSlots（驱动保留区不可领）") {
	// 3024 = NVIDIA descriptor heap 的实测 reservedSlots（VmaBuffer.cppm:158 注释）；
	// RADV/Intel = 0。两种驱动都得覆盖，否则"忘了加 base"这种错只在其中一台机器上暴露。
	SlotLedger nvidia(3024u, 8u);
	SlotLedger radv(0u, 8u);

	const auto n = nvidia.claimSlot("sampler.shared");
	REQUIRE(n.has_value());
	// 阳性锚点：忽略 base 的实现会返回 0 —— 在 NVIDIA 上那是驱动保留区，写进去 = 覆写驱动数据。
	CHECK(*n == 3024u);
	CHECK(nvidia.base() == 3024u);

	const auto r = radv.claimSlot("sampler.shared");
	REQUIRE(r.has_value());
	CHECK(*r == 0u);

	// 两个台账互不干扰（各自独立的申报记录，不共享全局状态）。
	CHECK(nvidia.used() == 1u);
	CHECK(radv.used() == 1u);
	CHECK(*n != *r);
}

TEST_CASE("SlotLedger ⑤：owner 整串比较（前缀 / 大小写宽松匹配都算不同归属者）") {
	SlotLedger ledger(0u, 8u);
	const auto ao = ledger.claimSlot("ao");
	const auto aoPoint = ledger.claimSlot("aoPoint");
	const auto upper = ledger.claimSlot("AO");
	REQUIRE(ao.has_value());
	REQUIRE(aoPoint.has_value());
	REQUIRE(upper.has_value());

	// 阳性锚点：若 owner 比较被写成前缀匹配或大小写不敏感，后两次会误命中第一次的记录
	// 并返回同一槽 —— 那就是"两个归属者同槽"的另一种复发路径。
	CHECK(*ao != *aoPoint);
	CHECK(*ao != *upper);
	CHECK(*aoPoint != *upper);
	CHECK(ledger.used() == 3u);
}

TEST_CASE("SlotLedger ⑥：同序列重放得到同一张表（确定性契约 + 可观测性）") {
	const std::vector<std::string> order = {"sampler.shared", "sampler.skyEnv", "sampler.aoPoint"};

	SlotLedger x(0u, 8u);
	SlotLedger y(0u, 8u);
	for (const auto& owner : order) {
		auto sx = x.claimSlot(owner);
		auto sy = y.claimSlot(owner);
		REQUIRE(sx.has_value());
		REQUIRE(sy.has_value());
		CHECK(*sx == *sy);
	}
	// 逐项比对全表（不只是"长度相同"）：槽号会烘进管线（envSamplerHeapOffset），
	// 跨运行必须逐位一致。哈希序 / 指针序的实现在这里就会红。
	REQUIRE(x.entries().size() == y.entries().size());
	for (size_t i = 0; i < x.entries().size(); ++i) {
		CHECK(x.entries()[i].owner == y.entries()[i].owner);
		CHECK(x.entries()[i].slot == y.entries()[i].slot);
	}
	// 台账按发放顺序排列（不是字典序 —— 字典序会让"新增归属者"悄悄改掉别人的槽号）。
	CHECK(x.entries()[0].owner == "sampler.shared");
	CHECK(x.entries()[1].owner == "sampler.skyEnv");
	CHECK(x.entries()[2].owner == "sampler.aoPoint");

	// describe() 是"顺序被改动"唯一的现场证据（画面不变、VVL 不响，只有这张表能 diff）。
	CHECK(x.describe() == "sampler.shared→0 sampler.skyEnv→1 sampler.aoPoint→2");
	CHECK(SlotLedger(0u, 8u).describe() == "(none)");
}

TEST_CASE("SlotLedger ⑦：capacity = 0 边界（一次都领不到，且不发号）") {
	SlotLedger ledger(0u, 0u);
	const auto got = ledger.claimSlot("sampler.shared");
	CHECK_FALSE(got.has_value());
	CHECK(got.error().code == ErrorCode::DescriptorSlotOutOfBounds);
	CHECK(ledger.used() == 0u);
	CHECK(ledger.entries().empty());
}

}  // namespace RGL
