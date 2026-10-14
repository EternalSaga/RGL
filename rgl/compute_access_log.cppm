export module compute_access_log;

import rlstd;

// compute dispatch 的**资源访问日志**（**叶子模块**：纯值 + 一次查表、无 Vulkan 类型、无日志、
// 无设备依赖 → 属 rlbase，可在 `just test` 里毫秒级测；先例 = `slot_ledger.cppm`）。
//
// ## 它存在的唯一理由（把「哪些 compute 之间有写后读 hazard」从人工注释变成机器派生）
//
// 图外 compute 各自手发 barrier，而「缺 barrier 反正 VVL 会报」这个指望在本项目**实测不成立**
// （`docs/compute_调研.md` §4：syncval 对 shader 侧 buffer 访问 0 条）。于是「有人给某枚 compute
// 加一个 RW buffer、或让第二枚 compute 开始读同一资源，却没有加对应的同步边」这类错今天**完全
// 静默**。`offgraph_sync` 的 declare-by-emit 只保证**已发的一致**，看不见**没发的**。
//
// 本模块补上缺的那半格：**资源身份**。反射给方向（`compute_bindings_generated` 的
// writable/readable），`makeMappedCompute` 的绑定表给「哪个 binding 喂哪个 descriptor heap 槽」，
// 两者一交就得到「同槽 + 至少一方写」的访问对——**不需要任何人记得申报**。而「这条 hazard 靠哪条
// 边同步」仍是人工的半边，那张表住在 `offgraph_sync`（rlgfx），由它做对账。
//
// ## 🔴 本模块**不**裁决「有没有边」
//
// rlbase 不认识 `RgSyncEdgeId`（分层只向下：日志模块 import 不到 rlgfx 的枚举）。`add()` 只
// **派生** hazard 并返回，覆盖与否由调用方（工厂）拿去查 hazard 表。这条边界同时保证了本模块
// 可在无设备子集里单测——判据本体不依赖任何同步知识。
//
// ## 🔴 判据与执行顺序**无关**（定案，别"顺手加个 phase 字段"）
//
// 同一资源上一写一读（或两写），无论谁先跑都需要 barrier——RAW / WAR / WAW 三种 hazard 都要。
// 所以这里**没有**相位/顺序模型：`OffGraphSchedule` 的相位粒度是「工作项」级，而现有 5 类 hazard
// 里有 4 类发生在同一个工作项内部，相位模型根本排不了它们。`instance` 只是「同一次 add 内的两条
// 记录不是跨 dispatch 访问」的区分位（同实例内自别名是另一类问题，不在本层射程）。
//
// ## 为什么资源身份 = descriptor heap 槽号
//
// `makeMappedCompute` 的映射就是「binding → 槽号 × descriptorSize」，槽号是**引擎里唯一同时被
// 两侧认得的资源标识**（buffer 句柄在 compute 侧根本拿不到，mapping 里也没有）。同槽 = 同资源，
// 这是 heap 语义的定义而不是本模块的约定。

namespace RGL {

/**
 * 一个 dispatch 实例里的一个绑定（`ComputeAccessLog::add` 的入参形态）。
 *
 * 与 `RgComputeBindingRecord` 的差别只有 `instance`/`shader`：那两项由日志填（实例号是日志的
 * 发号器事实，调用方给不出、也不该给）。
 */
export struct RgComputeBindingDecl {
	std::string_view param;  ///< shader 里的 binding 变量名（如 "head"）
	uint32_t slot = 0;       ///< descriptor heap 槽号（🔴 资源身份 = 同槽即同资源）
	bool writable = false;   ///< 反射方向：会写（readWrite / write 类）
	bool readable = false;   ///< 反射方向：会读（read / readWrite）
};

/**
 * 一条已登记的绑定记录 = 「第 `instance` 个 dispatch 实例经 binding `param` 以某方向碰槽 `slot`」。
 *
 * 🔴 **名字是 `std::string` 而不是 `string_view`**：shader 键由 `computeShaderKey(spvPath)`
 * **派生**（返回局部 `std::string`），日志的寿命比那次 `add()` 长 ⟹ 存 view 就是悬空。第一版
 * 按 view 存，IZB 开档当场炸出「未申报的 hazard」+ 一条乱码消息（比较用的名字已被回收，
 * 规范化排序也跟着错）。日志必须拥有它记的东西，这条没有折中余地；条目只有个位数，
 * 且只在 shader object 创建期产生，分配成本为零。
 */
export struct RgComputeBindingRecord {
	uint32_t instance = 0;   ///< dispatch 实例号（同一枚 shader 的多个实例各一个，如 downsample 的每级）
	std::string shader;      ///< shader 文件名（"izb_create.comp.slang"）
	std::string param;       ///< binding 变量名（"head"）
	uint32_t slot = 0;       ///< 🔴 资源身份 = descriptor heap 槽号（同槽 = 同资源）
	bool writable = false;   ///< 反射方向：会写
	bool readable = false;   ///< 反射方向：会读
};

/**
 * 一条 hazard **类**：两侧 `(shader, param)` 的**无序**对（+ 首次派生时看到的槽号）。
 *
 * 为什么要「类」这一层：同一对 `(shader, param)` 会被多实例重复派生——EVSSM 的 downsample 链
 * 有 `mipLevels − 2` 对相邻级，每一对都派生出同一条 `(mipDst, mipSrc)` hazard。日志与报错都按类，
 * 否则一条链刷 6 行同样的话（而"刷屏"教人跳过日志）。
 *
 * 两侧按 `(shader, param)` 字典序**规范化**存放 ⟹ 类的身份与发现顺序无关，去重是一次相等比较。
 * `slot` 只是**首次派生**那一对的槽号（同类内它可能随实例变——downsample 每级换 mip 槽），
 * 用途是把报错落到具体现场，不参与身份判定。
 */
export struct RgComputeHazardClass {
	std::string shaderA;  ///< 规范化后的前一侧 shader
	std::string paramA;   ///< 前一侧 binding 名
	std::string shaderB;  ///< 后一侧 shader
	std::string paramB;   ///< 后一侧 binding 名
	uint32_t slot = 0;    ///< 首次派生时该资源所在的 heap 槽号（仅归因用）
};

/**
 * 两条记录是否构成 hazard（判据本体，四条**逐条**实现，缺一条就漏一类错）。
 *
 * ① `a.instance != b.instance` —— 同实例内的自别名不是本层要报的那类（一个 dispatch 内部
 *    读写同一资源是 shader 自己的事，barrier 帮不上）。
 * ② `a.slot == b.slot` —— 资源身份相同。
 * ③ 两侧都不是 `none`（`!writable && !readable`）—— `none` = 「不是资源访问型参数」
 *    （CB / push constant），既不算读也不算写，判据里直接跳过。
 * ④ `a.writable || b.writable` —— 至少一方写。两读不需要同步。
 *
 * 🔴 四条之间**没有**顺序/相位条件，理由见文件头（RAW/WAR/WAW 都要 barrier）。
 * @param a 一条记录
 * @param b 另一条记录
 * @return true = 这一对访问之间存在需要 barrier 的 hazard
 */
export [[nodiscard]] inline bool isComputeHazard(
	const RgComputeBindingRecord& a, const RgComputeBindingRecord& b) {
	return a.instance != b.instance
		&& a.slot == b.slot
		&& (a.writable || a.readable)
		&& (b.writable || b.readable)
		&& (a.writable || b.writable);
}

/**
 * 访问日志（append-only + hazard 派生）。
 *
 * 宿主：`Resources`（它本来就持有那座 descriptor heap，「哪个槽被哪个 dispatch 碰过」是它的话题）。
 * 生命周期：只在 **shader object 创建期**追加（EVSSM / IZB 是首帧懒构造 ⟹ 对账也在那一刻），
 * 没有每帧路径，也没有清空口——有清空口就等于允许"每帧重算"，那是本刀明确不要的形态。
 *
 * 线程模型：非线程安全（与 `Resources::allocSlot` 同前提：shader object 创建是单线程）。
 */
export class ComputeAccessLog {
    public:
	/**
	 * 追加一个 dispatch 实例的若干绑定，并派生它们与**既有记录**之间的 hazard。
	 *
	 * 只返回**本次新增**的 hazard 类（按类去重：已派生过的类不再返回）。这不是省流量——
	 * 「按类去重」正是"一条 mip 链只报一次"的实现处，也是调用方日志不会刷屏的原因。
	 * @param shader  shader 文件名（"izb_create.comp.slang"，= 接口表的键）
	 * @param bindings 该实例的 (binding 名, 槽号, 方向) 列表
	 * @return 本次新增的 hazard 类（顺序 = 首次派生顺序）；无新增则空
	 */
	std::vector<RgComputeHazardClass> add(std::string_view shader,
		std::span<const RgComputeBindingDecl> bindings) {
		std::vector<RgComputeHazardClass> fresh;
		const uint32_t instance = nextInstance++;
		// 先记下边界：新记录只与**既有**记录配对。判据 ① 本来就会排掉同实例内的配对，
		// 但把"新"与"旧"划出来让这件事在代码里看得见，而不是靠读者推 instance 的相等性。
		const size_t prior = records.size();
		records.reserve(prior + bindings.size());
		for (const auto& b : bindings)
			// 🔴 显式 std::string(...)：聚合初始化的成员是**拷贝初始化**，走不了 string→string 的
			// explicit 构造（实测 clang 22 直接报 no viable conversion）。
			records.push_back(RgComputeBindingRecord{instance, std::string(shader),
				std::string(b.param), b.slot, b.writable, b.readable});
		for (size_t i = prior; i < records.size(); ++i) {
			for (size_t j = 0; j < prior; ++j) {
				if (!isComputeHazard(records[i], records[j]))
					continue;
				auto cls = makeClass(records[i], records[j]);
				if (std::ranges::any_of(classes,
					[&](const RgComputeHazardClass& c) { return sameClass(c, cls); }))
					continue;  // 同类已被更早的实例派生过（downsample 链的第 2..N−1 级）
				classes.push_back(cls);
				fresh.push_back(cls);
			}
		}
		return fresh;
	}

	/** @return 全部记录（按登记顺序），供诊断与单测断言。 */
	[[nodiscard]] const std::vector<RgComputeBindingRecord>& entries() const {
		return records;
	}

	/** @return 已派生出的全部 hazard 类（按首次派生顺序，已按类去重）。 */
	[[nodiscard]] const std::vector<RgComputeHazardClass>& hazardClasses() const {
		return classes;
	}

	/** @return 已登记的 dispatch 实例数（= 下一个实例会拿到的号）。 */
	[[nodiscard]] uint32_t instanceCount() const {
		return nextInstance;
	}

    private:
	/**
	 * 由两条记录造出规范化的 hazard 类。
	 * @param a 一条记录
	 * @param b 另一条记录
	 * @return 两侧按 (shader, param) 字典序排好的一对
	 */
	[[nodiscard]] static RgComputeHazardClass makeClass(
		const RgComputeBindingRecord& a, const RgComputeBindingRecord& b) {
		// (shader, param) 的字典序。🔴 用 compare() 而不是 `<`：名字类型的关系运算符是
		// hidden friend / std 里的约束模板，经 rlstd 的 `import` 面 ADL 找不到（实测 clang 22 +
		// libstdc++ 16 报 invalid operands），而 compare() 是成员函数、随类型本身可达。
		int cmp = a.shader.compare(b.shader);
		if (cmp == 0)
			cmp = a.param.compare(b.param);
		const bool aFirst = cmp < 0;
		const auto& first = aFirst ? a : b;
		const auto& second = aFirst ? b : a;
		return RgComputeHazardClass{first.shader, first.param, second.shader, second.param,
			first.slot};
	}

	/**
	 * 两个 hazard 类是否同一类（**无序**比较：只认两侧 (shader, param) 这个集合）。
	 * @param a 一个类
	 * @param b 另一个类
	 * @return true = 同一类
	 */
	[[nodiscard]] static bool sameClass(
		const RgComputeHazardClass& a, const RgComputeHazardClass& b) {
		return a.shaderA == b.shaderA && a.paramA == b.paramA
			&& a.shaderB == b.shaderB && a.paramB == b.paramB;
	}

	std::vector<RgComputeBindingRecord> records;  ///< 追加式记录表（永不重排/删除）
	std::vector<RgComputeHazardClass> classes;    ///< 已派生的 hazard 类（按首次派生顺序，去重后）
	uint32_t nextInstance = 0;                    ///< dispatch 实例发号器
};

}  // namespace RGL
