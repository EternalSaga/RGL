module;
#include <doctest/doctest.h>

/**
 * test_shutdown_ledger —— 诊断账注册表（债-140 第四击 ③）的机制面。
 *
 * 被测对象 = `rgl/shutdown_ledger.cppm` 的 `ShutdownLedger`（纯逻辑、零设备 ⟹ 跑在
 * `just test` 子集里）。这个类存在的唯一理由是**读数偏差**：诊断账原先只在 main 的退出
 * 收口打印，而 device lost 时那条路执行不到（收尾 `waitIdle` 抛异常直接跳过），于是
 * "审了 2055 帧零不符"这个数**恰好排除了正在调查的那些轮**——表里 4 个挂的轮次全报 0。
 * 所以本文件测的不是"能不能印"，而是三条**会让读数骗人**的性质：
 *
 *   ① 幂等：两条路（device-lost 抓取点 / 退出收口）先后各调一次 `run()`，只结算一次。
 *      少了这条 → 同一笔账印两遍，读日志的人以为是两次不同的运行。
 *   ② 顺序稳定：打印顺序 = 登记顺序。少了这条 → 归因时"哪笔账先出"不可依赖。
 *      （本项目的日志判据大量靠 grep 取首条，顺序漂了判据就漂了。）
 *   ③ 空表不印、不炸。少了这条 → 门关态（一笔账都没登记）会多出输出，
 *      而"门关零出声"是三锚之外的另一条硬验收。
 *
 * 另测 append-only：`run()` 之后登记的账，下一次 `run()` 要结算，已结算的不重印。
 * 这条对应真实场景 = 启动早期崩（账还没登记全）后，退出收口再跑一次把新账补上。
 *
 * ## 变异红线声明（改在源码哪一行，照抄即可重放）
 *   Ⓐ `run()` 里删掉 `if (e.printed) continue;` → 红「两条路只结算一次」
 *   Ⓑ `run()` 里把 `e.printed = true` 挪到 `e.fn()` **之后** → 红「reporter 抛了也不重印」
 *      （本文件用一条会抛的 reporter 钉它：置位在后 → 抛之前没置位 → 第二次 run 重印）
 *   Ⓒ `add()` 改成插到表头（`insert(begin)`）→ 红「顺序 = 登记顺序」
 *   Ⓓ `pending()` 改成返回 `entries.size()` → 红「run 后 pending 归零」
 */
export module test_shutdown_ledger;

import rlstd;
import shutdown_ledger;

namespace RGL {
namespace {

/* 三组计数器：reporter 是函数指针，只能写文件作用域的可变状态。
   每个用例开头 reset（本构建的 doctest 默认顺序执行，无并发）。 */
int hitsA = 0;
int hitsB = 0;
std::vector<int> order;
int throwHits = 0;

void resetCounters() {
	hitsA = hitsB = throwHits = 0;
	order.clear();
}

void reportA() { ++hitsA; order.push_back(1); }
void reportB() { ++hitsB; order.push_back(2); }
/** 首次结算时抛一次的 reporter（验 Ⓑ：置位必须在调用之前，否则抛过之后会重印）。 */
void reportThrow() {
	++throwHits;
	if (throwHits == 1)
		throw std::runtime_error("boom");
}

}  // namespace

TEST_CASE("ShutdownLedger 空表：run() 不炸、不结算、pending 恒 0") {
	resetCounters();
	ShutdownLedger ledger;
	CHECK(ledger.size() == 0u);
	CHECK(ledger.pending() == 0u);
	ledger.run();  // 门关态（一笔账都没登记）必须安静且活着
	ledger.run();
	CHECK(order.empty());
	CHECK(ledger.size() == 0u);
}

TEST_CASE("ShutdownLedger 幂等：两条路先后各 run() 一次，每笔账只结算一次") {
	resetCounters();
	ShutdownLedger ledger;
	ledger.add(reportA);
	ledger.add(reportB);
	CHECK(ledger.pending() == 2u);
	ledger.run();  // = device-lost 抓取点先到
	CHECK(hitsA == 1);
	CHECK(hitsB == 1);
	CHECK(ledger.pending() == 0u);  // 账本已跑空
	ledger.run();  // = 退出收口后到（异常没展开过去、正常跑完的情况）
	CHECK_MESSAGE(hitsA == 1, "第二次 run 重印了 A —— 读日志的人会以为是两次运行各审了一遍");
	CHECK(hitsB == 1);
	CHECK(order.size() == 2u);
}

TEST_CASE("ShutdownLedger 顺序：打印顺序 == 登记顺序（日志判据大量靠 grep 取首条）") {
	resetCounters();
	ShutdownLedger ledger;
	ledger.add(reportB);  // 故意先登记 B
	ledger.add(reportA);
	ledger.run();
	REQUIRE(order.size() == 2u);
	CHECK_MESSAGE(order[0] == 2, "先登记的 B 没先印 = 顺序不再等于登记顺序（变异红线 Ⓒ）");
	CHECK(order[1] == 1);
}

TEST_CASE("ShutdownLedger append-only：run 之后登记的新账下次结算，已结算的不重印") {
	resetCounters();
	ShutdownLedger ledger;
	ledger.add(reportA);
	ledger.run();
	CHECK(hitsA == 1);
	ledger.add(reportB);  // 场景 = 启动早期崩（账没登记全），退出收口再补一次
	CHECK(ledger.pending() == 1u);
	CHECK(ledger.size() == 2u);
	ledger.run();
	CHECK_MESSAGE(hitsB == 1, "run 之后登记的账必须被下一次 run 结算（否则新加的账永远不出声）");
	CHECK(hitsA == 1);
}

TEST_CASE("ShutdownLedger 去重标志先于调用置位：reporter 抛了也不会重印") {
	resetCounters();
	ShutdownLedger ledger;
	ledger.add(reportThrow);
	// reporter 抛了 —— 本类不隔离异常（契约见类注释：抛 = 记账代码自己有 bug，炸出来比吞掉好），
	// 所以这里由测试接住，只验"置位时机"这一条判据。
	CHECK_THROWS_AS(ledger.run(), std::runtime_error);
	CHECK(throwHits == 1);
	CHECK(ledger.pending() == 0u);  // 🔴 已置位：抛过也算结算过
	ledger.run();                    // 第二次不应再调
	CHECK_MESSAGE(throwHits == 1, "置位若在调用之后，抛过一次会重印（变异红线 Ⓑ）");
}

}  // namespace RGL
