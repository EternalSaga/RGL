module;
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>  // 被测函数的入参类型（第三方头，走 GMF include）

/**
 * test_burst_protocol —— `/shotburst` 协议判据（rgl/debug_rpc_burst.cppm，rlbase 零设备）
 *
 * 被测 = 「请求体形状」与「绝对帧号锚点裁决」两段纯逻辑（债-89：连拍锚从帧数增量换成
 * 绝对帧号）。它们决定时域判据能不能跨进程逐帧复现，而**失效形态恰好是静默的**：锚点
 * 一旦悄悄退回相对，读数照样能出一组「看起来很干净」的数——所以这一层必须有反向用例。
 *
 * ## 期望值出处（独立于被测实现）
 * 全部期望值 = 手写的协议约定本身（张数区间、字段互斥、锚点三态、五条归因文案），不取
 * 被测码的返回值当期望。文案在此**是**契约（客户端按它归因），所以断言写成「九条反向输入
 * 收敛到这五条、一条不多一条不少」，而不是含糊的「非空」。
 *
 * ## 阳性锚点与反向用例
 *  - 最小合法请求（只有 n/dir）解析通过且 startFrame = 0（**旧行为逐位不变**的载体）；
 *  - 锚点三态各一条：未到 → armed = 锚点、恰在本帧 → armed = 0（立即起跑，还来得及）、
 *    已过 → 非空错误（**绝不静默退回相对**）；
 *  - 反向：n 越界/非整数、dir 空、attachment 与 gbuf 同时给、attachment 非白名单、
 *    start_frame = 0 或负或浮点 → 九条输入全部响亮，且按字段收敛成五条归因文案
 *    （同分支共享文案 = 契约；不同字段共用一条 = 读者无法分辨坏在哪个字段）。
 *
 * ## 变异红线（哪条变异让我红）
 *  `startFrame < frameNow` 改成 `<=` → 「恰在本帧」那条红（那是还能救的最后一帧）；
 *  `armed = startFrame > frameNow ? startFrame : 0` 改成无条件 `= startFrame` → 同帧那条红
 *  （挂一个已过的锚 = 永远等不到首张 = 卡到超时）；锚已过时改成 `armed = 0` 放行 → 反向
 *  用例红（= 静默退回相对，正是本债要根除的形态）。
 */
export module test_burst_protocol;

import rlstd;
import debug_rpc_burst;

namespace RGL {

namespace {
/// 最小合法请求体（其余用例在此基础上改字段 = 每次只注入一个失效）。
nlohmann::json minimalRequest() {
	return { { "n", 8 }, { "dir", "screenshots" } };
}
}  // namespace

TEST_CASE("burst 协议：最小请求通过 + 逐字段套用（未传 prefix = 不改，现行为）") {
	const nlohmann::json body = minimalRequest();
	BurstRequest req;
	CHECK(parseBurstRequest(body, req).empty());
	CHECK(req.n == 8u);
	CHECK(req.dir == "screenshots");
	CHECK(req.gbuf == -1);            // 未传 = 后缓冲
	CHECK(req.attachment.empty());    // 未传 = swapchain 路
	CHECK(!req.prefix.has_value());   // 未传 = 调用侧保留上一次的（现行为）
	CHECK(req.startFrame == 0u);      // 未传 = 无锚 = 拆分前逐位不变

	// 全字段：prefix / gbuf / start_frame 一起给
	const nlohmann::json full = { { "n", 32 },
		{ "dir", "screenshots" },
		{ "prefix", "fl_c" },
		{ "gbuf", 2 },
		{ "start_frame", 528 } };
	BurstRequest r2;
	CHECK(parseBurstRequest(full, r2).empty());
	CHECK(r2.n == 32u);
	CHECK(r2.prefix.has_value());
	CHECK(*r2.prefix == "fl_c");
	CHECK(r2.gbuf == 2);
	CHECK(r2.startFrame == 528u);

	// attachment 白名单两档各自通过（taa_out = temporal-motion 的拒收掩膜连拍）
	const nlohmann::json att = { { "n", 4 }, { "dir", "d" }, { "attachment", "taa_out" } };
	BurstRequest r3;
	CHECK(parseBurstRequest(att, r3).empty());
	CHECK(r3.attachment == "taa_out");
}

TEST_CASE("burst 协议：形状反向九条全部响亮 + 归因按字段收敛（同分支共享文案是契约）") {
	const std::vector<nlohmann::json> bad = {
		{ { "n", 0 }, { "dir", "d" } },                                        // n 下界
		{ { "n", 1025 }, { "dir", "d" } },                                     // n 上界
		{ { "n", "8" }, { "dir", "d" } },                                      // n 非整数
		{ { "n", 4 }, { "dir", "" } },                                         // dir 空
		{ { "n", 4 }, { "dir", "d" }, { "attachment", "gbuf" }, { "gbuf", 1 } },  // 互斥
		{ { "n", 4 }, { "dir", "d" }, { "attachment", "depth" } },             // 非白名单
		{ { "n", 4 }, { "dir", "d" }, { "start_frame", 0 } },                  // 锚 = 0
		{ { "n", 4 }, { "dir", "d" }, { "start_frame", -5 } },                 // 锚为负
		{ { "n", 4 }, { "dir", "d" }, { "start_frame", 1.5 } },                // 锚非整数
	};
	std::vector<std::string> msgs;
	msgs.reserve(bad.size());
	for (const auto& body: bad) {
		BurstRequest req;
		const std::string err = parseBurstRequest(body, req);
		CHECK(err.empty() == false);  // 每条都必须响亮（静默通过 = 坏请求进装配态）
		msgs.push_back(err);
	}
	// 归因粒度 = **字段**：同一校验分支的多条输入共享一条文案是设计如此（客户端按文案归因，
	// 文案也是契约），但不同字段必须给不同串——九条输入按字段收敛成五条，一条不多一条不少。
	// 少一条 = 某个分支的输入被静默放行；多一条 = 同字段给了两种说法（读者无法枚举）。
	const std::vector<std::string> expect = {
		"'n' must be an integer in [1, 1024]",
		"'dir' must be a non-empty path",
		"'attachment' and 'gbuf' are mutually exclusive",
		"'attachment' only supports swapchain | taa_out",
		"'start_frame' must be an integer in [1, 2^32)",
	};
	for (const auto& e: expect)
		CHECK(std::find(msgs.begin(), msgs.end(), e) != msgs.end());  // 每条归因都被命中过
	size_t distinct = 0;
	for (size_t i = 0; i < msgs.size(); ++i)
		if (std::find(msgs.begin(), msgs.begin() + static_cast<long>(i), msgs[i])
			== msgs.begin() + static_cast<long>(i))
			++distinct;
	CHECK(distinct == expect.size());
}

TEST_CASE("burst 协议：绝对帧号锚点三态（未到=等 / 恰在本帧=立即 / 已过=报错不退回相对）") {
	// ① 锚未到：armed = 锚点（帧首钩子里等到它才登记首张）
	uint32_t armed = 0;
	CHECK(resolveBurstAnchor(257, 528, armed).empty());
	CHECK(armed == 528u);
	// ② 恰在本帧：还来得及（请求在帧首 drain，本帧 drawFrame 未跑）⟹ 立即起跑，
	//    且**不能**挂一个锚——挂了就是等一个已经过去的帧，永远等不到。
	CHECK(resolveBurstAnchor(528, 528, armed).empty());
	CHECK(armed == 0u);
	// ③ 无锚（旧行为）：恒立即起跑
	CHECK(resolveBurstAnchor(528, 0, armed).empty());
	CHECK(armed == 0u);
	// ④ 锚已过：必须报错，且**不写任何状态**（早退）——静默改成「立即起跑」= 退回相对锚，
	//    正是本债要根除的形态，所以这里用哨兵值钉住「出错时 armed 原样不动」。
	armed = 0xA5A5u;
	const std::string err = resolveBurstAnchor(600, 528, armed);
	CHECK(err.empty() == false);
	CHECK(err.find("528") != std::string::npos);   // 指名被过的锚点
	CHECK(err.find("600") != std::string::npos);   // 指名当前帧（读者能判断差多少）
	CHECK(armed == 0xA5A5u);
}

}  // namespace RGL
