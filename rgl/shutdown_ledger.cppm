export module shutdown_ledger;

import rlstd;  // std::vector / std::size_t（rlstd 是全部 STL 名的唯一来源）

namespace RGL {

/**
 * 退出与崩溃**两条路共用**的「诊断账结算」注册表（债-140 第四击 ③）。
 *
 * ## 它要消灭的具体形态（本项目的第三次同类事故）
 * 诊断账（"轮询了多少次 / 审了多少帧"）原先只在 main 的退出收口里打印。而 device lost 时
 * 那条路**执行不到**：收尾的 `waitIdle` 抛 `vk::SystemError`，异常展开直接跳过 `exitDiagnostics`。
 * 后果不是"少一行日志"，而是**读数系统性偏差**——实测第一版跑出来的表里，4 个挂掉的轮次
 * 全部报"审了 0 帧"，于是"审了 2055 帧零不符"这个数恰好排除了正在调查的那些轮。
 *
 * 上批的修法是"在抓取点也调一遍 + 各自记 `printed` 去重"。那是对的，但它把**两件事**摊给
 * 每一笔账：① 记得在两个地方调 ② 记得自己去重。加第三笔账的人只要漏掉任一件，症状就是
 * 上面那条"读数看着很干净"——**不会有人红**。本类把两件都收进来：
 *   · 调用点：两条路各调一次 `run()`，新账**不需要知道有几条路**；
 *   · 去重：`printed` 由本类持有，账主自己不再放标志。
 *
 * ## 加一笔账的唯一入口
 * `main_impl.cppm` 的 `registerDiagnosticReporters()` —— 那里加一行 `add(...)`，别处不改。
 * 刻意**不**做静态检查"某函数是否已登记"：漏登记的那一处恰好是查不到的那一处（写不出判据的
 * 护栏比没有护栏更糟），所以这里靠的是"只有一处可加"这个形状本身。
 *
 * ## 对 reporter 的唯一要求
 * 只准读自己的计数 + 打日志，**不得抛**。本类不做异常隔离：账本里每一笔都是日志，抛 =
 * 记账代码自己有 bug，那种情况应该炸出来，而不是被 `catch(...)` 吞成"这行没印"。
 *
 * 可实例化（不只是单例）是刻意的：`tests/test_shutdown_ledger.cppm` 要能在不污染进程级
 * 账本的前提下测幂等与顺序。
 */
export class ShutdownLedger {
public:
	/** 一笔账的结算入口（无参、无返回值：只准读自己的计数 + 打日志）。 */
	using Reporter = void (*)();

	/**
	 * 登记一笔账（append-only：登记顺序 = 打印顺序，本类不排序、不去重同名）。
	 * 🔴 重复登记**同一函数**会被当成两笔账各印一次 —— 这是调用方的 bug，本类不兜
	 *（兜了就会掩盖"两处都登记了"这种真分家；单测 `run 两次只印一次` 测的是路径重复，
	 * 不是登记重复，两者不许混）。
	 * @param fn 结算函数（必须是自由函数 / 无捕获 lambda 转的函数指针）
	 */
	void add(Reporter fn) { entries.push_back(Entry { .fn = fn }); }

	/**
	 * 结算所有**尚未结算**的账（幂等：已印过的不再印）。
	 * 两条路都调它 —— device-lost 抓取点先调，退出收口后调；谁先到谁结算。
	 */
	void run() {
		for (auto& e : entries) {
			if (e.printed)
				continue;
			e.printed = true;  // 🔴 先置位再调用：调用抛了也不重试（同一份账印两遍比少印更糟）
			e.fn();
		}
	}

	/** @return 已登记的账数（含已结算的；测试用） */
	[[nodiscard]] size_t size() const { return entries.size(); }
	/** @return 还没结算的账数（= 0 表示这条路已经把账本跑空） */
	[[nodiscard]] size_t pending() const {
		size_t n = 0;
		for (const auto& e : entries)
			if (!e.printed)
				++n;
		return n;
	}

private:
	/** 一笔账 + 它的结算标志（`printed` 由本类持有，账主自己不再放标志）。 */
	struct Entry {
		Reporter fn;
		bool printed = false;  ///< 去重标志（由本类持有，账主自己不再放）
	};
	std::vector<Entry> entries;
};

/**
 * 进程级账本（登记点 = `main_impl.cppm` 的 `registerDiagnosticReporters`；
 * 结算点 = device-lost 抓取点 + 退出收口，两处都调 `run()`）。
 * @return 单例引用（函数内 static = 无静态初始化顺序问题）
 */
export inline ShutdownLedger& shutdownLedger() {
	static ShutdownLedger ledger;
	return ledger;
}

}  // namespace RGL
