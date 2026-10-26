module;
#include <sol/sol.hpp>
#include <fmt/format.h>
export module runtime_config:keys;

/**
 * runtime_config:keys —— 「解析器认识哪些键」的键名真相源 + 未知键 lint（本刀自 :impl 拆出）
 *
 * ## 为什么这是一条职责边界（判据 = 输入输出类型 + 消费方）
 *
 * 本分区回答的问题全部是**关于键名本身**的，与"某个键写进哪个结构体字段"无关：
 * - 这个键名解析器认识吗（`isKnownRuntimeKey` / `kRuntimeKeys`）？
 * - 这个块是固定字段块、可以下钻吗（`declaresNestedKeys`）？
 * - 文件里**实际出现**了哪些键（`collectPresentKeys`，与"读没读"相对）？
 * - 出现而不认识的键怎么出声（`lintUnknownRuntimeKeys`）？
 * - 外界怎么拿到这份名单（`runtimeConfigKnownKeys`）？
 *
 * 类型面因此自成一族：进出全是 `std::string_view` / `RuntimeKeyTrace` / `sol::table`，
 * **没有一处**写 `RuntimeConfig` 的字段。而留在 `:impl` 的装配函数（applyRenderScale /
 * readFsrBlock / readSsrBlock / readWetBlock / readCoordinateBlock / parseLlm / readUiFont）
 * 恰好相反：它们的存在理由就是"键 → 字段"，一条键名判据都不写。
 *
 * 消费方也不同：键表同时被**测试**当真相源读（`runtimeConfigKnownKeys()` 与解析器读过的
 * 键集合做双向覆盖率断言、gate_lint 吃它的 `meshlet.` 前缀键），装配函数则零测试直连——
 * 两者同处一文件时，"改一个字段"和"改一份对外承诺的名单"会落在同一个 diff 里。
 *
 * ## 为什么两侧都依赖本分区、而本分区不依赖任何一侧
 *
 * 键表是**叶子**：只 import 主接口拿 `RuntimeKeyTrace`，不认识任何装配函数。`:impl` 与
 * `:failfast` 因此各自独立 `import :keys;`，分区间没有横向耦合。
 *
 * ## 可见性
 *
 * 接口分区。`export` 只给 `:impl`（loadRuntimeConfig）看得见的两个入口；主接口
 * `runtime_config` 不 `export import` 本分区 ⟹ 模块外不可见（等价于拆行之前的文件内可见性）。
 * `kRuntimeKeys` / `declaresNestedKeys` / `isKnownRuntimeKey` 仍在匿名命名空间里，与拆行前一致。
 *
 * ## 搬移性质
 *
 * **纯搬移**：函数体逐行原样（脚本按行区间切片，非重打）。唯一的结构性改动 = 原本同一个
 * 匿名命名空间里的五个实体按"要不要给 :impl 用"分成两组（**组内相对顺序不变**，全部
 * 被调者仍先于调用者定义），两个入口加 `export`。`kRuntimeKeys` 是 `constexpr`，
 * 重排不引入任何静态初始化次序问题。
 */

import runtime_config;  // clang 不给 partition 隐式 import 主接口——必须显式
import rlstd;
import rllogger;

namespace RGL {

namespace {

/**
 * 键表 = 「解析器认识哪些键」的单一真相源（diagnostics 的 kGateEcho 同角色，债-11）：
 * ① 未知键 lint 的判据表；② tests 里「读过的键集合 == 键表键集合」双向覆盖率断言的输入。
 * 嵌套块字段用 `表名.字段`；数组形字段（headless_size/win_size 的 [1][2]）无键名只记
 * 表名；llm_debug 有布尔/表两形态（表内字段与 llm_shot 同名）只记一个键——债-129 第三刀
 * 起未知键 lint 对**本表声明了 `块.` 点分前缀**的块下钻一层（fsr/ssr/wet/meshlet/llm_shot），
 * llm_debug 的表形态因键表没有 `llm_debug.` 声明 ⟹ 仍在射程外（diagnostics 同边界）。
 * ⚠ **原生数组**（尺寸自动推导）不是 `std::array<T, N>`——shadow_config_impl 同款教训。
 */
constexpr std::string_view kRuntimeKeys[]{
	"headless",
	"headless_size",
	"win_size",
	"render_scale",
	"aa",
	"fsr.temporal",
	"fsr.jitter",
	"ssr.enabled",
	"ssr.max_distance_radii",
	"ssr.thickness_px",
	"ssr.bias_px",
	"ssr.rough_full",
	"ssr.rough_cutoff",
	"ssr.blur_scale",
	"wet.strength",
	"wet.dark",
	"wet.rough_lo",
	"wet.rough_hi",
	"wet.ripple_amp",
	"wet.puddle_freq_radii",
	"meshlet.assets",
	"meshlet.mode",
	"meshlet.instrument",
	"meshlet.cull",
	"meshlet.shadow_lod",
	"meshlet.shadow_lod_fallback",
	"meshlet.mdi_shadow",
	"meshlet.mdi_shadow_direct",
	"meshlet.shadow_proxy",
	"meshlet.coplanar",
	"coordinate.rebase",
	"show_after",
	"present_mode",
	"ui_font",
	"ui_font_size",
	"llm_debug",
	"llm_shot.delay_frames",
	"llm_shot.file",
	"llm_shot.size",
};

/**
 * 键表是否声明了 `块.` 前缀的点分键（= 该块是**固定字段块**，可下钻；债-129 第三刀）。
 * 开放 map 段（scene 的 deferred_styles：子键是用户自起的材质名，键表里没有）
 * 键表只有裸表名 ⟹ false ⟹ 不钻——「护栏喊错一次，以后所有 warn 都没人信」，规则
 * 自配置：键表没声明 `deferred_styles.` 就不报 `deferred_styles.xxx`。
 * @param block 顶层键名（候选块名）
 */
bool declaresNestedKeys(std::string_view block) {
	for (const auto& k : kRuntimeKeys) {
		const auto dot = k.find('.');
		if (dot != std::string_view::npos && k.substr(0, dot) == block)
			return true;
	}
	return false;
}

/**
 * 单键已知判据（lintUnknownRuntimeKeys 的判据本体，债-11 + 债-129 第三刀）：
 * **含 `.`** 的键（下钻采集的 `块.子键`）🔴 必须是键表的**精确成员**——块内拼错不再被
 * 顶层前缀判据吞掉；**不含 `.`** 的键（顶层名）保持原判据——键表任一键名的「`.` 前缀」
 * 等于它即已知（表名 = 前缀命中，不喊）。抽成函数是认知复杂度红线（先例 applyAaKey）。
 * @param key present 里的键名
 */
bool isKnownRuntimeKey(std::string_view key) {
	const bool dotted = key.find('.') != std::string::npos;
	for (const auto& k : kRuntimeKeys) {
		const bool hit = dotted ? (k == key)                      // 点分名 = 精确成员
		                        : (k.substr(0, k.find('.')) == key);  // 无 `.` 时 = 全名比对
		if (hit)
			return true;
	}
	return false;
}


}  // namespace

/**
 * 「文件里实际出现的键」采集（diagnostics 的 collectPresentKeys 同构，未知键 lint
 * 的输入；present 来自文件本体不来自读取动作）。顶层键照旧记；债-129 第三刀：键表声明了
 * `块.` 点分前缀的块**下钻一层**，子键以 `块.子键` 追加——块内拼错的键此前完全静默
 * （拍板 A「meshlet.enable 改名 28 处」的硬前置护栏，旧名按拍板原文引）。数组形子表（headless_size 的
 * [1][2]）子键是数字不是字符串键 ⟹ 天然不贡献；非表值跳过；只钻一层不递归。
 * @param root 配置根表
 * @param trace 键轨迹（present 追加目标）
 */
export void collectPresentKeys(const sol::table& root, RuntimeKeyTrace& trace) {
	for (auto&& kv : root) {
		sol::optional<std::string> k = kv.first.as<sol::optional<std::string>>();
		if (!k)
			continue;
		trace.present.push_back(*k);
		if (!declaresNestedKeys(*k) || kv.second.get_type() != sol::type::table)
			continue;
		const sol::table sub = kv.second.as<sol::table>();
		for (auto&& child : sub)
			if (sol::optional<std::string> ck = child.first.as<sol::optional<std::string>>())
				trace.present.push_back(fmt::format("{}.{}", *k, *ck));
	}
}

/**
 * 未知键 lint（lintUnknownDiagnosticsKeys 同判据，债-11 + 债-129 第三刀）：present 里
 * 每个键过 isKnownRuntimeKey（顶层 = 前缀判据、点分名 = 精确成员，判据本体见该函数）。
 * 判据来自键表本身（= 解析器键名真相源），不另立名单——覆盖率护栏（测试）保证键表 ⊥
 * 解析器不会漂移，本函数因此只需查一张表。
 * @param trace 解析期键轨迹
 */
export void lintUnknownRuntimeKeys(const RuntimeKeyTrace& trace) {
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	for (const auto& key : trace.present)
		if (!isKnownRuntimeKey(key))
			logger->warn("runtime: 未知键 '{}'（解析器不读它，拼错？已忽略）", key);
}

/** runtimeConfigKnownKeys 定义（接口注释见 runtime_config.cppm）。 */
std::vector<std::string> runtimeConfigKnownKeys() {
	std::vector<std::string> keys;
	keys.reserve(std::size(kRuntimeKeys));
	for (const auto& k : kRuntimeKeys)
		keys.emplace_back(k);
	return keys;
}

}  // namespace RGL
