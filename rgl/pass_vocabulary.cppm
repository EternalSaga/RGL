module;
export module pass_vocabulary;

import rlstd;

// 渲染图字符串词表（**叶子模块**）——2026-10-02 从 state_kit / render_graph /
// frame_draw_list 各处的字面量里抽出（`RgPassDesc::state` 与 `RgPassDesc::draws`
// 两个字符串字段的全部合法值）。
//
// ## 为什么抽成叶子模块
//
// 这两张词表原本散成五处：StateKit::stateFor 的 if 链、执行器校验里的
// `kKnownStates{6}` 字面量、render_graph 的 `kFullscreenPassStates`、
// frame_draw_list 的 isKnownDrawSource、两份 render_graph.lua 的头注释。**加一个
// 状态要改五处**，而这条债不是理论风险——批 11.3-1d 加 `izb_mask` 时只改了
// stateFor 一处，其余四处全部落后（执行器词表没写理由地不含它、单测状态表用例
// 少一名、两份 Lua 头注释与 AGENTS 的"九段"口径双双变谎言行），当时还在
// docs/hfts_izb_plan.md 留了"落地时同步更新口径，勿忘"的实现期待办。
//
// 本模块是纯 constexpr 数据（名字 + 类别，**零 Vulkan 类型**），故可以住在
// rlbase 并被四层任意方向的消费者拿到（同 gbuffer_contract 的先例）。
//
// ## 三条红线（改动本表前先读）
//
// 1. **两档校验不可并成一张扁平表**：执行器按 `pass.kind` 分区校验 state——
//    drawMesh 认 scope=meshPass、drawLight 认 scope=fullscreenPass。并集校验会让
//    `kind=drawLight, state="opaque"` 通过图校验，而 drawLight 分支随后按 state 名
//    去 registry 取预建全屏条目 → 放行 = 晚得多的崩溃或静默不画。
// 2. **`internal` 档不走图**：`izb_mask` 由执行器硬编码录制路径直接
//    `stateFor("izb_mask")`（vkrenderer_passrec），不是一张图上的 pass，故两档
//    图校验都不认它——这是**有意排除**，不是漏登记，别顺手加进 meshPass 档。
// 3. **本表与实现侧必须同名同数**：StateKit 用 `stateNamesCoverAll` 做
//    static_assert（加名字忘了实现 = 编译不过）。段名侧的"表 ↔ segmentSpan 的
//    if 链"无法编译期绑，由 tests/test_render_graph.cppm 钉住。

namespace RGL {

/**
 * `pass.state` 状态名的归属类别——决定它被哪条图校验承认为合法（见文件头红线 1）。
 * `internal` 档的存在本身是结论不是兜底：它把"这个状态不走图"写进数据，
 * 而不是靠执行器词表少一条来隐含表达。
 */
export enum class RgStateScope : uint8_t {
	meshPass = 0,    ///< drawMesh pass 的合法 state（执行 FrameDrawList 段）
	fullscreenPass,  ///< drawLight pass 的合法 state（固定全屏条目，state 名 = 条目名）
	internal,        ///< 不走图：执行器硬编码录制路径直接 stateFor（红线 2）
};

/** pass.state 词表条目：状态名 + 归属类别。 */
export struct RgStateName {
	std::string_view name;
	RgStateScope scope = RgStateScope::internal;
};

/**
 * pass.state 全量状态名词表（**单一真相源**，10 条）——键集合 = StateKit::stateFor
 * 能解析的名字集合（static_assert 绑定）；按 scope 派生出图校验用的两个子集。
 *
 * 加一个状态 = 本表加一行 + StateKit 加成员与实现（漏实现编译不过）；
 * Lua 配置侧与文档不再各自抄名单（写错的启动报错会列出合法值）。
 */
export inline constexpr std::array<RgStateName, 10> kAllPassStates{{
	{"opaque", RgStateScope::meshPass},
	{"skybox", RgStateScope::meshPass},
	{"transparent", RgStateScope::meshPass},
	{"picking", RgStateScope::meshPass},       // 批 10.5：id pass 重放 opaque 段
	{"highlight", RgStateScope::meshPass},     // 批 10.7：高亮 pass 按材质过滤重放
	{"shadow", RgStateScope::meshPass},        // 批 11.2：depth-only 阴影 pass
	{"light", RgStateScope::fullscreenPass},
	{"ao", RgStateScope::fullscreenPass},      // 批 11.4：SSAO 全屏终图
	{"tonemap", RgStateScope::fullscreenPass},
	{"izb_mask", RgStateScope::internal},      // 批 11.3-1d：图外硬编码录制（红线 2）
}};

// 派生工具（非导出实现细节）。两点注意：
// · 模板参数不用 `std::size_t N`——rlstd 不带 std::size_t（项目惯例 uint32_t 计数）；
// · 子集长度必须是**模板实参**（namesInScope<Scope>()）：数组长度要常量表达式，
//   函数形参不算（实测报 "non-type template argument is not a constant expression"）。
namespace detail {

/** 某类别的条目数（派生子集的长度）。 */
consteval uint32_t countScope(RgStateScope scope) {
	uint32_t n = 0;
	for (const auto& e : kAllPassStates)
		if (e.scope == scope)
			++n;
	return n;
}

/** 取某类别的状态名子集（顺序 = kAllPassStates 声明序）。 */
template <RgStateScope Scope>
consteval auto namesInScope() {
	std::array<std::string_view, countScope(Scope)> out{};
	for (uint32_t i = 0, j = 0; i < kAllPassStates.size(); ++i)
		if (kAllPassStates[i].scope == Scope)
			out[j++] = kAllPassStates[i].name;
	return out;
}

}  // namespace detail

/**
 * deferred 光照 pass 的 state 名（fullscreenPass 档里唯一带"图事实语义"的一个：
 * 「图里有没有 deferred 光照 pass」= 有没有 `drawLight` + 本名，见
 * render_graph::graphHasDeferredLight）。IZB / EVSSM / dump_gbuf 三类消费方判据
 * 都读它，故从字面量升为具名常量（比较侧用它，构造侧仍写字面量以便阅读）。
 */
export inline constexpr std::string_view kDeferredLightState = "light";

/**
 * drawMesh pass 的合法 state 子集（派生自 kAllPassStates，scope = meshPass）——
 * 执行器 validateRenderGraphForExecutor 校验 drawMesh pass 的 state 用本表。
 */
export inline constexpr auto kMeshPassStates = detail::namesInScope<RgStateScope::meshPass>();

/**
 * drawLight pass 的合法 state 子集（派生自 kAllPassStates，scope = fullscreenPass）。
 * state 名同时是 ShaderRegistry 固定全屏条目名（"light" = deferred 逐灯 GGX，
 * "ao" = 屏幕空间环境光遮蔽，"tonemap" = HDR→LDR ACES）——执行器构造期按名取条目。
 */
export inline constexpr auto kFullscreenPassStates =
	detail::namesInScope<RgStateScope::fullscreenPass>();

/**
 * 状态名是否被 StateKit 承认（全量集合，含 `internal` 档）——
 * = StateKit::stateFor 的键集合，**不是**图校验用的判据（图校验按档判）。
 * @param name pass.state 字符串
 * @return true = 词表内
 */
export constexpr bool isKnownStateName(std::string_view name) {
	for (const auto& e : kAllPassStates)
		if (e.name == name)
			return true;
	return false;
}

/**
 * drawMesh pass 的 state 合法性判据（scope = meshPass 档成员）。
 * @param name pass.state 字符串
 * @return true = 可作为 drawMesh pass 的状态名
 */
export constexpr bool isMeshPassState(std::string_view name) {
	for (const auto n : kMeshPassStates)
		if (n == name)
			return true;
	return false;
}

/**
 * drawLight pass 的 state 合法性判据（scope = fullscreenPass 档成员）。
 * @param name pass.state 字符串
 * @return true = 可作为 drawLight pass 的状态名（同时是 registry 固定条目名）
 */
export constexpr bool isFullscreenPassState(std::string_view name) {
	for (const auto n : kFullscreenPassStates)
		if (n == name)
			return true;
	return false;
}

// 具名常量与词表的绑定（改名时**编译不过**，而不是留下一个恒 false 的静默死判据）。
static_assert(isKnownStateName(kDeferredLightState)
		&& isFullscreenPassState(kDeferredLightState),
	"kDeferredLightState 必须是 kAllPassStates 里的 fullscreenPass 档状态名");

// 同名同数校验（StateKit 用它做 static_assert）。三个函数各管一段，合成一条判据；
// 用 `export { }` 包一块是因为 doc 检查的"可跳过 template 引入行"只认行首
// `template`，而 `export template` 同一行写法会漏判。
export {

/**
 * 实现侧每个名字都在词表内（不判"是否覆盖全表"）。
 * @param entries 实现侧表（元素含 name 字段）
 * @return true = 无词表外的名字
 */
template <class Entries>
consteval bool allNamesKnown(const Entries& entries) {
	for (const auto& e : entries)
		if (!isKnownStateName(e.name))
			return false;  // 实现侧有词表没有的名字
	return true;
}

/**
 * 词表某个名字在实现侧**恰好登记一次**（缺 0 次 = 死名，重复 = 一个名字两份存储）。
 * @param entries 实现侧表（元素含 name 字段）
 * @param name 词表里的状态名
 * @return true = 命中恰一次
 */
template <class Entries>
consteval bool coveredOnce(const Entries& entries, std::string_view name) {
	uint32_t hits = 0;
	for (const auto& e : entries)
		if (e.name == name)
			++hits;
	return hits == 1;
}

/**
 * 实现侧表与本词表的**同名同数**校验（StateKit::kStateSlots 用它做 static_assert）：
 * entries 的 name 集合必须与 kAllPassStates 完全相同——缺名、多名、重复名都判不过。
 * 模板鸭子类型：元素只需有 `.name`（std::string_view）。
 * @param entries 实现侧表（元素含 name 字段，长度任意）
 * @return true = 两侧集合相等
 */
template <class Entries>
consteval bool stateNamesCoverAll(const Entries& entries) {
	if (entries.size() != kAllPassStates.size() || !allNamesKnown(entries))
		return false;
	for (const auto& s : kAllPassStates)
		if (!coveredOnce(entries, s.name))
			return false;  // 词表名缺实现，或实现侧重复登记
	return true;
}

}  // export

/** 绘制段名（`pass.draws`）的匹配方式——阴影段名带可变分块号，只能前缀匹配。 */
export enum class RgDrawSourceMatch : uint8_t {
	exact,  ///< 精确相等
	prefix, ///< 前缀匹配（"shadow" 承 "shadow" / "shadow2" / "shadow13"）
};

/** pass.draws 词表条目：段名 + 匹配方式。 */
export struct RgDrawSourcePattern {
	std::string_view name;
	RgDrawSourceMatch match = RgDrawSourceMatch::exact;
};

/**
 * pass.draws 绘制段名词表（**单一真相源**）——消费方 = isKnownDrawSource（图校验）
 * 与文档口径。阴影段名 = "shadow" + 分块号（scene_context::shadowDrawSource），
 * 分块号可变（kActiveShadowCascades 1→4），故用**前缀匹配**：写死字面量会在切
 * 级联时漏校验。
 *
 * ⚠️ 本表只回答"这个段名合法吗"；"段名 → 哪一段 draw 序列"是
 * frame_draw_list::segmentSpan 的 if 链（含兜底分支），两者不共享机制——漂移由
 * tests/test_render_graph.cppm 的覆盖用例拦（红线 3）。
 */
export inline constexpr std::array<RgDrawSourcePattern, 4> kAllDrawSources{{
	{"opaque", RgDrawSourceMatch::exact},
	{"skybox", RgDrawSourceMatch::exact},
	{"transparent", RgDrawSourceMatch::exact},
	{"shadow", RgDrawSourceMatch::prefix},
}};

/**
 * 执行器可识别的 drawSource 段名判定（图校验入口，逐条按 match 方式比对）。
 *
 * 未知段名若不拦下来，segmentSpan 会走兜底分支返回 **transparent 段**
 * = 静默多画一遍透明体（无报错的错渲染），故这条判据必须在图校验里跑。
 * @param s pass.draws 字符串
 * @return true = 词表内（prefix 条目按前缀算命中）
 */
export constexpr bool isKnownDrawSource(std::string_view s) {
	for (const auto& p : kAllDrawSources) {
		if (p.match == RgDrawSourceMatch::prefix ? s.starts_with(p.name) : s == p.name)
			return true;
	}
	return false;
}

}  // namespace RGL
