module;
#include <sol/sol.hpp>
#include <fmt/format.h>
export module runtime_config:failfast;

/**
 * runtime_config:failfast —— **配置笔误不静默回落**那一族的键装配（本刀自 :impl 拆出）
 *
 * ## 为什么这是一条职责边界（判据 = 错误处理契约；这条缝是代码里本来就写好的）
 *
 * 本分区的每个函数都是同一个签名形状
 * `[[nodiscard]] static std::optional<std::string> f(...)`：**成功返回空、失败返回错误
 * 消息**，由调用方 fail-fast 成启动失败。文件里本来就有一条把它们成组的注释
 * （`applyFailFastKeys` 头注：「把「解析失败返回错误消息」的键串成一条……新增 fail-fast
 * 键 = 在此加一行」）——本刀只是把那条注释指向一个文件，让"在此加一行"有个明确落点。
 *
 * 与留在 `:impl` 的宽容装配是**相反**的契约，且两边都写明了为什么相反：
 * - 本分区：未知值 / 类型不对 = 启动失败点名键（`aa` / `present_mode` / `meshlet.*`）。
 *   理由 = 这些键决定"跑的是哪条路"，静默回落会让对照臂根本没跑而判据全绿。
 *   `readStrictBoolKey` / `readStrictIntKey` 因此也住这里：它们的存在理由就是这条纪律
 *   （错误消息里硬写着 `meshlet.`，不是通用机制，不该冒充通用取值层）。
 * - `:impl`：缺块 / 缺键 = 保持结构体默认，越界钳制 + warn，`ssr` / `wet` 两块甚至写明
 *   "原样透传，一条 warn 都不发"（校验会堵死合法消融档）。
 *
 * 把两种**刻意相反**的契约放在同一个文件里，正是"顺手给 ssr 也补一句 clamp"最容易发生的
 * 地方；分开之后各自的头注就是那条契约的告示牌。
 *
 * ## 可见性
 *
 * 接口分区。只有 `loadRuntimeConfig` 直接调的两处（链头 `applyFailFastKeys` 与
 * `applyPresentModeKey`）需要 `export`，其余保持文件内 `static`——与拆行前一致。主接口不
 * `export import` 本分区 ⟹ 模块外不可见。
 *
 * ## 搬移性质
 *
 * **纯搬移**：函数体逐行原样（脚本按行区间切片；两段区间内部顺序不变，`applyAaKey` 仍先于
 * `applyFailFastKeys`、meshlet 族内部顺序原样）。两个链头定义去掉 `static`、加 `export`
 * （内部链接 → 模块链接，运行期零差别，否则跨分区不可见）。本分区不 import `:keys`——
 * 它不需要键名判据。
 */

import runtime_config;  // clang 不给 partition 隐式 import 主接口——必须显式
import rlstd;

namespace RGL {

/**
 * `aa` 键解析（批 14.6 抗锯齿三态开关）：字符串 → `AaMode`。
 *
 * 缺键 = 保持默认 off（出厂态 = 红线 A）；未知值 = 返回错误消息由调用方 fail-fast
 * （present_mode 的既有纪律：配置笔误不静默回落——静默回落的失效形态 = 你以为开着
 * 抗锯齿在量图，其实跑在关档上，而图看起来"只是糊了一点"）。
 * 抽成函数 = 认知复杂度红线（先例 applyRenderScale），也为了让 tests 能单独打它。
 * @param root runtime.lua 顶层表
 * @param cfg  写入目标（仅在键存在且合法时被写）
 * @param path 仅用于错误消息（与 present_mode 同口径带配置文件路径）
 * @param trace 键轨迹（缺键也记 = 解析器读过它，覆盖率护栏的输入）
 * @return nullopt = 合法（含缺键）；有值 = 错误消息
 */
[[nodiscard]] static std::optional<std::string> applyAaKey(const sol::table& root,
	RuntimeConfig& cfg, std::string_view path, RuntimeKeyTrace& trace) {
	trace.read.emplace_back("aa");
	sol::optional<std::string> raw = root["aa"];
	if (!raw)
		return std::nullopt;
	if (*raw == "off")
		cfg.aa = AaMode::off;
	else if (*raw == "fsr") {
		// fsr 态**隐含打开**速度场（FSR 的 motionVectors 输入就是那张图）。债-129 S2.5-C：
		// velocity_field lua 键已撤并，velocityField 是**纯派生件**——本行是全项目唯一置重点
		// （消费侧对派生关系无感知）；off/taa 态不碰它 ⟹ 不会凭空冒出 velocity pass。
		cfg.aa = AaMode::fsr;
		cfg.velocityField = true;
	} else if (*raw == "taa")
		cfg.aa = AaMode::taa;
	else
		return fmt::format(
			"runtime config ({}): aa must be \"off\", \"fsr\" or \"taa\", got \"{}\"",
			path, *raw);
	return std::nullopt;
}

/**
 * `meshlet.coplanar` 三态字符串键解析（★0.5-b2 共面双臂；形态先例 = `applyAaKey`）。
 *
 * 缺键 = 保持默认 `off`（= 不碰动态状态 = 门关/off 动态状态序列逐 bit 不变，A1 构造性
 * 保证）。**未知值 = 返回错误消息由调用方 fail-fast**，不静默回落（present_mode/aa 的既有
 * 纪律，本批尤其致命：主判据 = 四臂截图哈希全同，笔误值静默回落成 off 会让 equal/ne 两臂
 * 根本没跑而四臂照样全绿 = 整批假绿）。
 * @param root meshlet 块表（与 readMeshletBlock 同层）
 * @param cfg  写入目标（仅在键存在且合法时被写）
 * @param path 仅用于错误消息（与 aa/present_mode 同口径带配置文件路径）
 * @return nullopt = 合法（含缺键）；有值 = 错误消息
 */
[[nodiscard]] static std::optional<std::string> applyMeshletCoplanarKey(const sol::table& root,
	RuntimeConfig& cfg, std::string_view path) {
	sol::optional<std::string> raw = root["coplanar"];
	if (!raw)
		return std::nullopt;
	if (*raw == "off")
		cfg.meshletCoplanar = MeshletCoplanarMode::off;
	else if (*raw == "equal")
		cfg.meshletCoplanar = MeshletCoplanarMode::equal;
	else if (*raw == "ne")
		cfg.meshletCoplanar = MeshletCoplanarMode::ne;
	else
		return fmt::format(
			"runtime config ({}): meshlet.coplanar must be \"off\", \"equal\" or \"ne\", got \"{}\"",
			path, *raw);
	return std::nullopt;
}

/**
 * 严格布尔键读取（★1-a1ii-β-1 起 **meshlet 块专用**；与 `readBoolKey` 的唯一差别 =
 * **值不是布尔时立即报错**，而不是静默当键缺失）。
 *
 * 为什么必须换（实测，不是推测）：`sol::optional<bool> v = root[key]` 在值是字符串或数字时
 * **静默给 nullopt** —— `meshlet.cull = "yes"` 与 `meshlet.cull = 1` 两条都按「键不存在」
 * 跑完整程，errors=0 warnings=0、日志零行。对 meshlet 块这是最坏的失效形态：β 的主判据是
 * 「截图哈希对基线」，键被静默吃掉 ⟹ 实际跑的是旧路径而判据全绿。`present_mode` / `aa` /
 * `meshlet.coplanar` 的「笔误不静默回落」纪律同向。
 * 缺键仍 = 保持结构体默认（门关 / off 护栏不动）。旧块（fsr/ssr/wet）不改——零回归面，
 * 严格化整张配置表是另一批的事。
 * @return nullopt = 缺键或类型正确；有值 = 错误消息（调用方 fail-fast）
 */
[[nodiscard]] static std::optional<std::string> readStrictBoolKey(const sol::table& root,
	const char* key, bool& dst, std::string_view path) {
	sol::object value = root[key];
	const auto type = value.get_type();
	if (type == sol::type::lua_nil)
		return std::nullopt;  // 缺键 = 出厂默认（门关护栏）
	if (type != sol::type::boolean)
		return fmt::format("runtime config ({}): meshlet.{} must be a boolean, got {}",
			path, key, sol::type_name(root.lua_state(), type));
	dst = value.as<bool>();
	return std::nullopt;
}

/**
 * 严格整数键读取（★18-B1 起 **meshlet 块专用**；与 `readStrictBoolKey` 同一条纪律，只是
 * 目标类型是 `int32_t`）：**类型不对立即报错点名键**，🔴 **不许**静默当键缺失。
 *
 * 理由不重抄——见 `readStrictBoolKey` 注释里那条实测（`sol::optional<T>` 在值类型不匹配时
 * **静默给 nullopt**，键被吃掉而 errors=0 warnings=0、日志零行 ⟹ 截图哈希判据整批假绿）。
 * 本键的失效形态同样致命：`shadow_lod = "5"` 被静默吃掉 = 跑的是关档，而"门开了、条目建了、
 * 图没变"会被读成「LOD 不影响图像」。
 * Lua 的 number 全是 double，故整数判据 = 「是 number 且无小数部分」；`5.0` 放行（Lua 里
 * `5` 与 `5.0` 是同一个值），`5.5` 报错而不是悄悄截断成 5。
 * @return nullopt = 缺键或类型正确；有值 = 错误消息（调用方 fail-fast）
 */
[[nodiscard]] static std::optional<std::string> readStrictIntKey(const sol::table& root,
	const char* key, int32_t& dst, std::string_view path) {
	sol::object value = root[key];
	const auto type = value.get_type();
	if (type == sol::type::lua_nil)
		return std::nullopt;  // 缺键 = 保持结构体默认（-1 = 本档关）
	if (type != sol::type::number)
		return fmt::format("runtime config ({}): meshlet.{} must be an integer, got {}",
			path, key, sol::type_name(root.lua_state(), type));
	const double raw = value.as<double>();
	// 整数判据 = 无小数部分；范围判据 = int32_t 可表示（越界不截断，报错——截断出来的
	// 是个语义完全不同的数，比"启动失败"难查得多）。
	constexpr double lo = static_cast<double>(std::numeric_limits<int32_t>::min());
	constexpr double hi = static_cast<double>(std::numeric_limits<int32_t>::max());
	if (raw != std::floor(raw) || raw < lo || raw > hi)
		return fmt::format(
			"runtime config ({}): meshlet.{} must be an integer (got {})", path, key, raw);
	dst = static_cast<int32_t>(raw);
	return std::nullopt;
}

/**
 * `meshlet.mode` 三态字符串键解析（债-129 S2 O1-1.2 = 原 `overlay` + `switch` 两布尔合并，
 * `aa` 三态先例复刻；形态先例 = `applyMeshletCoplanarKey` / `applyMeshletInstrumentKey`）。
 *
 * 缺键 = 保持默认 `off`（= 原两布尔全 false = 出厂不叠加不取代，债-105「不白付」+ P5 逐位
 * 不变护栏的构造性前提）。**未知值 = 返回错误消息由调用方 fail-fast 且点名全部合法值**，
 * 不静默回落（`aa` / `meshlet.coplanar` 的既有纪律：选路位被静默回落 = 取代臂根本没跑而
 * 截图判据全绿 = 整批假绿）。🔴 配置串 `"switch"` 对应枚举值 `inplaceSwitch`（`switch` 是
 * C++ 关键字，只有枚举值名让位，用户写的东西不让位）。
 * 🔴 旧两布尔键 `overlay` / `switch` **无兼容双读**（本仓硬切红线）——残留旧键由未知键护栏
 * 出声（`runtime: 未知键 'meshlet.overlay'`，tests 变异红线钉死）；两布尔的「同真」互斥态
 * 从此**写不出来**（合并的存在理由，实测证据 = /tmp/opencode/o3b_premerge_anchors.txt 第四行）。
 * @param root meshlet 块表（与 readMeshletBlock 同层）
 * @param cfg  写入目标（仅在键存在且合法时被写）
 * @param path 仅用于错误消息（与 aa/present_mode 同口径带配置文件路径）
 * @return nullopt = 合法（含缺键）；有值 = 错误消息
 */
[[nodiscard]] static std::optional<std::string> applyMeshletModeKey(
	const sol::table& root, RuntimeConfig& cfg, std::string_view path) {
	sol::optional<std::string> raw = root["mode"];
	if (!raw)
		return std::nullopt;
	if (*raw == "off")
		cfg.meshletMode = MeshletMode::off;
	else if (*raw == "overlay")
		cfg.meshletMode = MeshletMode::overlay;
	else if (*raw == "switch")
		cfg.meshletMode = MeshletMode::inplaceSwitch;
	else
		return fmt::format(
			"runtime config ({}): meshlet.mode must be \"off\", \"overlay\" or \"switch\", "
			"got \"{}\"",
			path, *raw);
	return std::nullopt;
}

/**
 * `meshlet.instrument` 三态字符串键解析（债-129 S2 O1-1.1 = 原 `debug` + `tint` 两布尔
 * 合并，`aa` 三态先例复刻；形态先例 = `applyMeshletCoplanarKey`）。
 *
 * 缺键 = 保持默认 `none`（= 正常叠加档 = 原两布尔全 false）。**未知值 = 返回错误消息由
 * 调用方 fail-fast 且点名全部合法值**，不静默回落（`aa` / `meshlet.coplanar` 的既有纪律：
 * 仪器位的笔误值静默回落成 none = 「阳性对照臂根本没跑而截图判据全绿」= 整批假绿）。
 * 🔴 旧两布尔键 `debug` / `tint` **无兼容双读**（本仓硬切红线）——残留旧键由未知键护栏
 * 出声（`runtime: 未知键 'meshlet.debug'`，tests 变异红线钉死）。
 * @param root meshlet 块表（与 readMeshletBlock 同层）
 * @param cfg  写入目标（仅在键存在且合法时被写）
 * @param path 仅用于错误消息（与 aa/present_mode 同口径带配置文件路径）
 * @return nullopt = 合法（含缺键）；有值 = 错误消息
 */
[[nodiscard]] static std::optional<std::string> applyMeshletInstrumentKey(
	const sol::table& root, RuntimeConfig& cfg, std::string_view path) {
	sol::optional<std::string> raw = root["instrument"];
	if (!raw)
		return std::nullopt;
	if (*raw == "none")
		cfg.meshletInstrument = MeshletInstrument::none;
	else if (*raw == "offset")
		cfg.meshletInstrument = MeshletInstrument::offset;
	else if (*raw == "tint")
		cfg.meshletInstrument = MeshletInstrument::tint;
	else
		return fmt::format(
			"runtime config ({}): meshlet.instrument must be \"none\", \"offset\" or \"tint\", "
			"got \"{}\"",
			path, *raw);
	return std::nullopt;
}

/**
 * `meshlet.shadow_lod_fallback` 字符串键解析（★18-B1；形态先例 = `applyMeshletCoplanarKey`）。
 *
 * 缺键 = 保持默认 `deepest`。**未知值 = 返回错误消息由调用方 fail-fast**，不静默回落
 * （`meshlet.coplanar` 的理由在此更硬：§18.2 实测 D=7 下 deepest = 3.32% 而 l0 = 34.97%
 * 三角，笔误值静默回落成默认会让「成本判据」整批读到错的臂）。
 * ⚠ 覆盖面与先例同边界：只有**字符串**值进得了本判据，数字值被 sol 的 `optional<string>`
 * 读成 nullopt = 走缺键保默认（实机对照 `meshlet.coplanar = 0` 同形）。整数键的严格类型面
 * 在 `readStrictIntKey`（走 `get_type()`，另一条代码形状），两者不是一回事，别混着指望。
 * @param root meshlet 块表（与 readMeshletBlock 同层）
 * @param cfg  写入目标（仅在键存在且合法时被写）
 * @param path 仅用于错误消息（与 aa/present_mode 同口径带配置文件路径）
 * @return nullopt = 合法（含缺键）；有值 = 错误消息
 */
[[nodiscard]] static std::optional<std::string> applyMeshletShadowLodFallbackKey(
	const sol::table& root, RuntimeConfig& cfg, std::string_view path) {
	sol::optional<std::string> raw = root["shadow_lod_fallback"];
	if (!raw)
		return std::nullopt;
	if (*raw == "deepest")
		cfg.meshletShadowLodFallback = MeshletLodFallbackMode::deepest;
	else if (*raw == "l0")
		cfg.meshletShadowLodFallback = MeshletLodFallbackMode::l0;
	else
		return fmt::format("runtime config ({}): meshlet.shadow_lod_fallback must be "
			"\"deepest\" or \"l0\", got \"{}\"",
			path, *raw);
	return std::nullopt;
}

/**
 * `meshlet.mdi_shadow_direct` 三态字符串键解析（债-140 §27 直派模拟，gfx1151 workaround，
 * 上游 Mesa#16670；形态先例 = `applyMeshletShadowLodFallbackKey`，含其 ⚠ 覆盖面边界：
 * 只有字符串值进得了判据，非字符串 = 缺键保默认）。
 * 值域 = "auto"（默认）/ "true" / "false"；未知值 = 启动 fail-fast（笔误静默回落 = 把
 * workaround 读丢，比崩更糟）。语义展开 → RuntimeConfig::meshletMdiShadowDirect 注释。
 */
[[nodiscard]] static std::optional<std::string> applyMdiShadowDirectEmuKey(
	const sol::table& root, RuntimeConfig& cfg, std::string_view path) {
	sol::optional<std::string> raw = root["mdi_shadow_direct"];
	if (!raw)
		return std::nullopt;
	if (*raw == "auto")
		cfg.meshletMdiShadowDirect = MdiShadowDirectEmu::Auto;
	else if (*raw == "true")
		cfg.meshletMdiShadowDirect = MdiShadowDirectEmu::On;
	else if (*raw == "false")
		cfg.meshletMdiShadowDirect = MdiShadowDirectEmu::Off;
	else
		return fmt::format("runtime config ({}): meshlet.mdi_shadow_direct must be "
			"\"auto\", \"true\" or \"false\", got \"{}\"",
			path, *raw);
	return std::nullopt;
}

/**
 * `meshlet` 嵌套块解析（★0-b1 簇路径**资源门**（assets：决定条目烘不烘、4 段簇 buffer 与描述符槽
 * 在不在；不是块内 8 键的总门——生效前置不含它的只有 mode 的 `"overlay"` 档，三条语义的权威展开
 * → RuntimeConfig::meshletAssets 注释）+ ★1-a1ii-α/β-1 选路档（mode，债-129 S2 由 overlay/switch
 * 两布尔合并）+ ★1-a1ii-α/★0.5-b2 仪器档（instrument，债-129 S2 由 debug/tint 两布尔合并）+
 * ★0.5-b2 coplanar + ★1-a2-i cull + ★18-B1 shadow_lod 阴影段选级 +
 * ★M-0 mdi_shadow 真 MDI 档）：
 * `meshlet = { assets, mode, instrument, coplanar, cull, shadow_lod,
 * shadow_lod_fallback, mdi_shadow, mdi_shadow_direct, shadow_proxy }`。
 * 缺块 / 缺键 = 保持结构体默认（🔴 ★18-C 起 `meshletAssets = true`；`meshletShadowLod` 曾 5、
 * N10 起 7 且 `meshletShadowProxy = true`（2026-10-20 用户拍板，依据 → 数 8）
 * = 出厂即开簇路径与阴影远级联 LOD，2026-10-12 用户拍板。P5「门关 = 逐位不变」**仍是硬护栏**，
 * 只是门关态现在要显式 `assets = false` + `shadow_lod = -1` + `shadow_proxy = false`（N10 起
 * 三脚；实测精确复现旧锚 54e40665…）。
 * 逐键默认值唯一真相源 = RuntimeConfig 成员初始化，本行只列其余形态：`meshletMode = off` =
 * 不叠加不取代（债-129 S2 合并原 overlay/switch 两布尔，「同真」互斥态写不出来；债-105 起
 * 出厂不再白付 1.59ms）；`meshletInstrument = none` =
 * 正常叠加档（不选仪器变体，债-129 S2 合并原 debug/tint 两布尔）；`meshletCoplanar = off` =
 * 不碰动态状态；`shadow_lod_fallback = deepest`；
 * `meshletMdiShadow = false` = 不走 MDI（★M-1b 起本键**有消费者**：键开则烘 MDI 条目并回填，
 * 宿主条件 → 字段注释）。
 * 形态先例 = readFsrBlock（嵌套块，无扁平键双读兼容）；coplanar / shadow_lod_fallback /
 * instrument / mode 四枚三态键先例 = applyAaKey（未知值 fail-fast，错误经返回值上抛给调用方）。
 * 四枚布尔走 readStrictBoolKey、shadow_lod 走 readStrictIntKey（类型错 = 启动失败，理由 →
 * 各自注释）。
 * 债-106 已挂号：gate_lint 的 `Ctx` 现含 `RuntimeConfig` 与它的出厂默认，整块 8 枚键逐枚
 * 申报（规则表 → `gate_lint_impl.cppm` 的 meshlet 段；判据出处 = 各字段注释 + `wireMeshlet*`
 * 消费者）。★18-C 的就近 WARNING（`warnIfShadowLodOrphaned`）与叠加仪器的就近 warn
 * （`warnMeshletVariantInstruments`）判据是纯配置组合，已随迁移删除（同一根因两条喊 =
 * 影子真相源）；其余装配层 warn 依赖装配期事实（条目数 / 载体在不在），保留。
 * @param root runtime.lua 顶层表
 * @param cfg  写入目标
 * @param path 仅用于错误消息
 * @param trace 键轨迹（十枚键在块入口**无条件**记全 = 覆盖率护栏输入；五枚三态字符串键
 *              由本函数记，不进 applyMeshlet*Key 内部记——每键恰好记一次）
 * @return nullopt = 合法；有值 = 首个错误消息（调用方 fail-fast）
 */
[[nodiscard]] static std::optional<std::string> readMeshletBlock(const sol::table& root,
	RuntimeConfig& cfg, std::string_view path, RuntimeKeyTrace& trace) {
	trace.read.emplace_back("meshlet.assets");
	trace.read.emplace_back("meshlet.mode");
	trace.read.emplace_back("meshlet.instrument");
	trace.read.emplace_back("meshlet.cull");
	trace.read.emplace_back("meshlet.shadow_lod");
	trace.read.emplace_back("meshlet.shadow_lod_fallback");
	trace.read.emplace_back("meshlet.mdi_shadow");
	trace.read.emplace_back("meshlet.mdi_shadow_direct");
	trace.read.emplace_back("meshlet.shadow_proxy");
	trace.read.emplace_back("meshlet.coplanar");
	sol::table meshlet = root["meshlet"];
	if (!meshlet.valid() || !meshlet.is<sol::table>())
		return std::nullopt;
	// 顺序 = 键在 configs/runtime.lua 里的出现序；任一失败即返回（fail-fast，不收集后合并）
	if (auto err = readStrictBoolKey(meshlet, "assets", cfg.meshletAssets, path))
		return err;
	// 债-129 S2 选路三态键（off/overlay/switch = 原 overlay/switch 两布尔合并，互斥态写不出来；
	// 债-105 α 影子叠加段 = "overlay" 档，出厂 "off" ⟹ 出厂不再白付 +1.59ms）
	if (auto err = applyMeshletModeKey(meshlet, cfg, path))
		return err;
	// 债-129 S2 仪器位三态键（none/offset/tint = 原 debug/tint 两布尔合并，让位态写不出来）
	if (auto err = applyMeshletInstrumentKey(meshlet, cfg, path))
		return err;
	// ★1-a2-i 逐簇视锥剔除
	if (auto err = readStrictBoolKey(meshlet, "cull", cfg.meshletCull, path))
		return err;
	// ★18-B1 阴影段簇路径选级（-1 = 关 / 0 = 阳性对照 L0 臂 / N≥1 = uniform 选级 D=N）
	if (auto err = readStrictIntKey(meshlet, "shadow_lod", cfg.meshletShadowLod, path))
		return err;
	// ★18-B1 回落策略（三态字符串键，未知值 fail-fast）
	if (auto err = applyMeshletShadowLodFallbackKey(meshlet, cfg, path))
		return err;
	// ★M-0（§19.2 D2）真 MDI 阴影段档（本批零消费者 = 读到也不改变任何一帧行为）
	if (auto err = readStrictBoolKey(meshlet, "mdi_shadow", cfg.meshletMdiShadow, path))
		return err;
	// 债-140 §27 直派模拟三态键（gfx1151 workaround，宿主 = mdi_shadow 的宿主；语义 → 字段注释）
	if (auto err = applyMdiShadowDirectEmuKey(meshlet, cfg, path))
		return err;
	// N10 P2 阴影代理钉段表档（🔴 P2 本批零消费者：建表 + 打日志，接消费点 = P3）
	if (auto err = readStrictBoolKey(meshlet, "shadow_proxy", cfg.meshletShadowProxy, path))
		return err;
	return applyMeshletCoplanarKey(meshlet, cfg, path);
}

/**
 * fail-fast 键链（★0.5-b2，认知复杂度预算）：把「解析失败返回错误消息」的键串成一条，
 * `loadRuntimeConfig` 只留一个 if+return（两处各一个曾把认知复杂度顶到 16>15）。
 * 新增 fail-fast 键 = 在此加一行，不再往 loadRuntimeConfig 里添 if。
 * 顺序契约：applyAaKey 必须先于 readFsrBlock（aa=fsr 隐含打开速度场在其内一行）；
 * readMeshletBlock 只读 meshlet 块、与其他键零耦合，链尾即可。
 * @param root runtime.lua 顶层表
 * @param cfg  写入目标
 * @param path 仅用于错误消息
 * @param trace 键轨迹（透传给链上各键）
 * @return nullopt = 全部合法；有值 = 首个错误消息
 */
export [[nodiscard]] std::optional<std::string> applyFailFastKeys(const sol::table& root,
	RuntimeConfig& cfg, std::string_view path, RuntimeKeyTrace& trace) {
	if (auto err = applyAaKey(root, cfg, path, trace))
		return err;
	return readMeshletBlock(root, cfg, path, trace);  // ★0-b1 簇路径资源门（非总门，语义 → readMeshletBlock 头注）+ ★0.5-b2 coplanar 三态键
}

/**
 * `present_mode` 键解析（批 9.3 present 模式偏好；抽函数 = loadRuntimeConfig 认知
 * 复杂度红线，先例 applyAaKey）。缺键 = 保持默认 immediate；未知值 = 返回错误消息由
 * 调用方 fail-fast（与类型错误同级，不静默回落）。
 * @param root runtime.lua 顶层表
 * @param cfg  写入目标（仅在键存在且合法时被写）
 * @param path 仅用于错误消息（与 aa 同口径带配置文件路径）
 * @param trace 键轨迹（缺键也记 = 解析器读过它，覆盖率护栏的输入）
 * @return nullopt = 合法（含缺键）；有值 = 错误消息
 */
export [[nodiscard]] std::optional<std::string> applyPresentModeKey(const sol::table& root,
	RuntimeConfig& cfg, std::string_view path, RuntimeKeyTrace& trace) {
	trace.read.emplace_back("present_mode");
	sol::optional<std::string> presentMode = root["present_mode"];
	if (!presentMode)
		return std::nullopt;
	if (*presentMode == "immediate")
		cfg.presentMode = PresentModePref::immediate;
	else if (*presentMode == "fifo")
		cfg.presentMode = PresentModePref::fifo;
	else
		return fmt::format(
			"runtime config ({}): present_mode must be \"immediate\" or \"fifo\", got \"{}\"",
			path, *presentMode);
	return std::nullopt;
}

}  // namespace RGL
