module;
#include <sol/sol.hpp>
#include <glm/glm.hpp>
#include <fmt/format.h>
module scene_config:impl;

import scene_config;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import vkerror;
import rllogger;
import scene_context;

namespace RGL {

// readVec3（内部 helper）+ defaultSceneConfig/loadSceneConfig 定义（2026-09-02
// 编译提速 A 方案）：sol2 实现移出接口 TU；本单元是叶子边。
// Phase 8 批次 ②b 拆债：loadSceneConfig（175 行）按段拆 parse* helper，主函数
// 只剩沙箱建立 + dofile + 段调度；environment 段新增 hdr/hdr_size 可选字段。

/**
 * Lua vec3 读取（可选字段回落默认值，同 camera.cppm loadCameraConfig 惯例）：
 * {x,y,z} → glm::vec3；字段缺失/非表/分量不齐 → fallback。
 * @param root 配置（子）表
 * @param key 字段名
 * @param fallback 缺省值
 * @return 解析值或 fallback
 */
inline glm::vec3 readVec3(sol::table root, const char* key, glm::vec3 fallback) {
	sol::table t = root[key];
	if (!t.valid() || !t.is<sol::table>())
		return fallback;
	sol::optional<float> x = t[1];
	sol::optional<float> y = t[2];
	sol::optional<float> z = t[3];
	if (x && y && z)
		return glm::vec3(*x, *y, *z);
	return fallback;
}

/** 顶层表取字符串字段（缺失/非表 = no-op 回落现值）。 */
void readStr(sol::table table, const char* key, std::string& into) {
	sol::optional<std::string> v = table[key];
	if (v)
		into = *v;
}

/** 顶层表取 float 字段（缺失 = no-op 回落现值）。 */
void readFloat(sol::table table, const char* key, float& into) {
	sol::optional<float> v = table[key];
	if (v)
		into = *v;
}

/** 顶层表取 bool 字段（缺失 = no-op 回落现值）。 */
void readBool(sol::table table, const char* key, bool& into) {
	sol::optional<bool> v = table[key];
	if (v)
		into = *v;
}

/** defaultSceneConfig 定义（接口注释见 scene_config.cppm）：内建管线集 + styles {unlit,pbr} + spin。 */
SceneConfig defaultSceneConfig() {
	SceneConfig cfg;
	cfg.pipelines = {
		{"cube", "generated_shaders/cube.mesh.spv", "generated_shaders/cube.frag.spv"},
		{"model", "generated_shaders/model.mesh.spv", "generated_shaders/model.frag.spv"},
		{"model_pbr", "generated_shaders/model.mesh.spv", "generated_shaders/model_pbr.frag.spv"},
		{"model_phong", "generated_shaders/model.mesh.spv", "generated_shaders/model_phong.frag.spv"},
		{"model_pbr_ibl", "generated_shaders/model.mesh.spv", "generated_shaders/model_pbr_ibl.frag.spv"},
		{"model_vegetation", "generated_shaders/model.mesh.spv", "generated_shaders/model_vegetation.frag.spv"},
		{"model_light", "generated_shaders/model.mesh.spv", "generated_shaders/model_light.frag.spv"},
		{"deferred_geometry", "generated_shaders/model.mesh.spv", "generated_shaders/deferred_geometry.frag.spv"},
		{"deferred_geometry_vegetation", "generated_shaders/model.mesh.spv",
			"generated_shaders/deferred_geometry_vegetation.frag.spv"},
		{"deferred_geometry_light", "generated_shaders/model.mesh.spv",
			"generated_shaders/deferred_geometry_light.frag.spv"},
		{"deferred_geometry_unlit", "generated_shaders/model.mesh.spv",
			"generated_shaders/deferred_geometry_unlit.frag.spv"},
		{"skybox", "generated_shaders/skybox.mesh.spv", "generated_shaders/skybox.frag.spv"},
	};
	cfg.styleMap = {{"unlit", "model"}, {"pbr", "model_pbr"},
		{"vegetation", "model_vegetation"}, {"light", "model_light"}};
	cfg.materialRules = {{"Emissive", "light"}, {"Foliage", "vegetation"}};
	// unlit 必须上榜（2026-10-18）：缺表回落 deferred_geometry = KHR_materials_unlit
	// 材质被静默打光（D1 破口本体，见 deferred_geometry_unlit.frag.slang 头注）。
	cfg.deferredStyleMap = {{"unlit", "deferred_geometry_unlit"},
		{"vegetation", "deferred_geometry_vegetation"},
		{"light", "deferred_geometry_light"}};
	cfg.spin = true;
	return cfg;
}

namespace {

/**
 * 键表 = 「解析器认识哪些键」的单一真相源（diagnostics 的 kGateEcho 同角色，债-11）：
 * ① 未知键 lint 的判据表；② tests 里「读过的键集合 == 键表键集合」双向覆盖率断言的输入。
 * 子表字段用 `表名.字段`；map 段（styles/deferred_styles）键名任意只记表名——债-129
 * 第三刀起这同时是**下钻豁免声明**（未知键 lint 只钻键表声明了 `块.` 点分前缀的块，
 * 裸表名 = 不钻，见 declaresNestedKeys）；数组段（entities/pipelines/lights）按条目
 * 字段记 `表名.字段`（条目内字段名固定）。
 * ⚠ **原生数组**（尺寸自动推导）不是 `std::array<T, N>`——shadow_config_impl 同款教训：
 * N 写错时 std::array 不报错而是零填充。
 */
constexpr std::string_view kSceneKeys[]{
	"model",
	"spin",
	"spin_frame_angle",
	"spin_axis",
	"cube",
	"exposure",
	"ao.strength",
	"taa.feedback",
	"taa.jitter_scale",
	"taa.motion_compensation",
	"taa.base_space",
	"taa.reprojection",
	"taa.diag_reject",
	"taa.input_kernel",
	"taa.clip_mode",
	"taa.clip_soft",
	"entities.type",
	"entities.file",
	"entities.position",
	"entities.rotation",
	"entities.scale",
	"pipelines.name",
	"pipelines.mesh",
	"pipelines.frag",
	"light.direction",
	"light.color",
	"light.intensity",
	"light.ambient",
	"lights.direction",
	"lights.position",
	"lights.color",
	"lights.intensity",
	"lights.range",
	"lights.type",
	"environment.dir",
	"environment.faces",
	"environment.intensity",
	"environment.skybox",
	"environment.hdr",
	"environment.hdr_size",
	"styles",
	"material_styles.pattern",
	"material_styles.style",
	"deferred_styles",
};

/**
 * 键表是否声明了 `块.` 前缀的点分键（= 该块是**固定字段块**，可下钻；债-129 第三刀，
 * runtime_config_impl 同构）。🔴 本 loader 是这条前缀条件存在的理由：scene.lua 的
 * styles/deferred_styles 是**开放 map**（子键 = 用户自起的材质名，键表里没有），
 * 键表只有裸表名 ⟹ false ⟹ 不钻——无条件下钻会把 `styles.pbr` 报成未知键。
 * 数组段（entities/pipelines/material_styles）虽声明了点分前缀，但条目子键是数字
 * 不是字符串键 ⟹ 下钻天然不贡献。
 * @param block 顶层键名（候选块名）
 */
bool declaresNestedKeys(std::string_view block) {
	for (const auto& k : kSceneKeys) {
		const auto dot = k.find('.');
		if (dot != std::string_view::npos && k.substr(0, dot) == block)
			return true;
	}
	return false;
}

/**
 * 「文件里实际出现的键」采集（diagnostics 的 collectPresentKeys 同构，未知键 lint
 * 的输入；present 来自文件本体不来自读取动作）。顶层键照旧记；债-129 第三刀：键表声明了
 * `块.` 点分前缀的块（ao/taa/entities/pipelines/light/lights/environment/material_styles）
 * **下钻一层**，字符串子键以 `块.子键` 追加；开放 map（styles/deferred_styles）不钻
 * （判据见 declaresNestedKeys）。
 * @param root 配置根表
 * @param trace 键轨迹（present 追加目标）
 */
void collectPresentKeys(const sol::table& root, SceneKeyTrace& trace) {
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
 * 单键已知判据（lintUnknownSceneKeys 的判据本体，债-11 + 债-129 第三刀）：**含 `.`**
 * 的键（下钻采集的 `块.子键`）🔴 必须是键表的**精确成员**；**不含 `.`** 的键保持原判据
 * （键表任一键名的「`.` 前缀」等于它即已知——map 段裸表名走这条）。抽成函数是认知
 * 复杂度红线（先例 applyAaKey）。
 * @param key present 里的键名
 */
bool isKnownSceneKey(std::string_view key) {
	const bool dotted = key.find('.') != std::string::npos;
	for (const auto& k : kSceneKeys) {
		const bool hit = dotted ? (k == key)                      // 点分名 = 精确成员
		                        : (k.substr(0, k.find('.')) == key);  // 无 `.` 时 = 全名比对
		if (hit)
			return true;
	}
	return false;
}

/**
 * 未知键 lint（lintUnknownDiagnosticsKeys 同判据，债-11 + 债-129 第三刀）：present 里
 * 每个键过 isKnownSceneKey（顶层 = 前缀判据、点分名 = 精确成员，判据本体见该函数）。
 * 判据来自键表本身（= 解析器键名真相源），不另立名单——覆盖率护栏（测试）保证键表 ⊥
 * 解析器不会漂移，本函数因此只需查一张表。
 * @param trace 解析期键轨迹
 */
void lintUnknownSceneKeys(const SceneKeyTrace& trace) {
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	for (const auto& key : trace.present)
		if (!isKnownSceneKey(key))
			logger->warn("scene: 未知键 '{}'（解析器不读它，拼错？已忽略）", key);
}

/**
 * entities 单条目解析（type/file/position/rotation/scale；错误消息逐字保持拆分前文案）。
 * @param item 条目表
 * @param i 条目序号（1 起，错误信息用）
 * @return 解析出的 SceneEntityDesc；未知 type / model 缺 file / scale≤0 = Error
 */
std::expected<SceneEntityDesc, Error> parseEntityItem(sol::table item, int i) {
	SceneEntityDesc ent;
	readStr(item, "type", ent.type);
	if (ent.type != "model" && ent.type != "cube" && ent.type != "aa_target")
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("scene config: entities[{}] unknown type '{}' (want model|cube|aa_target)",
				i, ent.type)});
	readStr(item, "file", ent.file);
	if (ent.type == "model" && ent.file.empty())
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("scene config: entities[{}] type 'model' missing 'file'", i)});
	ent.position = readVec3(item, "position", ent.position);
	ent.rotation = readVec3(item, "rotation", ent.rotation);
	float scale = ent.scale;
	readFloat(item, "scale", scale);
	ent.scale = scale;
	if (ent.scale <= 0.0f)
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("scene config: entities[{}] scale must be > 0", i)});
	return ent;
}

/**
 * entities 段解析（Phase 6 批次 5：场景构成数据驱动列表，非空 = 权威；缺段 =
 * 回落旧 model/cube 字段合成，既有配置零改动等价）。type "model"|"cube"|"aa_target"，
 * transform 全可选（position/rotation 欧拉角度/scale 均匀）。
 * 键轨迹在段入口**无条件**记全五个字段键（解析器全量问一遍 = 覆盖率护栏的输入，
 * 同 shadow applyTuningBlock 的口径；条目内重复读不重复记）。
 * @param root 配置根表
 * @param cfg 目标配置（读 modelPath/showCube 做缺段合成，写 entities）
 * @param trace 键轨迹
 * @return 结构违例（未知 type / model 缺 file / scale≤0）= Error
 */
std::expected<void, Error> parseEntities(sol::table root, SceneConfig& cfg,
	SceneKeyTrace& trace) {
	trace.read.emplace_back("entities.type");
	trace.read.emplace_back("entities.file");
	trace.read.emplace_back("entities.position");
	trace.read.emplace_back("entities.rotation");
	trace.read.emplace_back("entities.scale");
	sol::table entities = root["entities"];
	if (entities.valid() && entities.is<sol::table>()) {
		cfg.entities.clear();
		for (int i = 1; i <= static_cast<int>(entities.size()); ++i) {
			sol::table item = entities[i];
			auto ent = parseEntityItem(item, i);
			if (!ent)
				return std::unexpected(std::move(ent).error());
			cfg.entities.push_back(std::move(*ent));
		}
	}
	// 缺段兼容合成（entities 空 = 旧 model/cube 字段语义，行为逐位不变）
	if (cfg.entities.empty()) {
		if (!cfg.modelPath.empty())
			cfg.entities.push_back(SceneEntityDesc{.type = "model", .file = cfg.modelPath});
		if (cfg.showCube)
			cfg.entities.push_back(SceneEntityDesc{.type = "cube"});
	}
	return {};
}

/**
 * pipelines 段解析（条目名 → SPV 路径规格表，非空覆盖默认六条）。
 * 键轨迹在段入口无条件记全三个字段键（同 parseEntities 口径）。
 * @param root 配置根表
 * @param cfg 目标配置（写 pipelines）
 * @param trace 键轨迹
 * @return 缺 name = Error
 */
std::expected<void, Error> parsePipelines(sol::table root, SceneConfig& cfg,
	SceneKeyTrace& trace) {
	trace.read.emplace_back("pipelines.name");
	trace.read.emplace_back("pipelines.mesh");
	trace.read.emplace_back("pipelines.frag");
	sol::table pipelines = root["pipelines"];
	if (!(pipelines.valid() && pipelines.is<sol::table>()))
		return {};
	cfg.pipelines.clear();
	for (int i = 1; i <= static_cast<int>(pipelines.size()); ++i) {
		sol::table item = pipelines[i];
		PipelineSpec spec;
		readStr(item, "name", spec.name);
		readStr(item, "mesh", spec.meshSpv);
		readStr(item, "frag", spec.fragSpv);
		if (spec.name.empty())
			return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
				fmt::format("scene config: pipeline[{}] missing 'name'", i)});
		cfg.pipelines.push_back(std::move(spec));
	}
	return {};
}

/**
 * light 单灯段 + lights 多灯列表段解析（Phase 5 批次 2 / Phase 7 D3）。
 * lights 缺段回落合成 [light] 单灯（lights[0] = 主光）；per-light 不解析 ambient
 *（场景共享项，仅 lights[0] 进 scene data @36）。type=0 定向 / 1 点光。
 * ambient 语义：light 段是唯一可配源——lights 段被采用时 lights[0].ambient 也取
 * light 段值（否则 lights 段一存在，light.ambient 就成死配置、静默回落组件默认）。
 * 键轨迹在段入口无条件记全 light 四键 + lights 六键（同 parseEntities 口径）。
 * @param root 配置根表
 * @param cfg 目标配置（写 light/lights）
 * @param trace 键轨迹
 */
void parseLights(sol::table root, SceneConfig& cfg, SceneKeyTrace& trace) {
	trace.read.emplace_back("light.direction");
	trace.read.emplace_back("light.color");
	trace.read.emplace_back("light.intensity");
	trace.read.emplace_back("light.ambient");
	trace.read.emplace_back("lights.direction");
	trace.read.emplace_back("lights.position");
	trace.read.emplace_back("lights.color");
	trace.read.emplace_back("lights.intensity");
	trace.read.emplace_back("lights.range");
	trace.read.emplace_back("lights.type");
	sol::table light = root["light"];
	if (light.valid() && light.is<sol::table>()) {
		cfg.light.direction = readVec3(light, "direction", cfg.light.direction);
		cfg.light.color = readVec3(light, "color", cfg.light.color);
		readFloat(light, "intensity", cfg.light.intensity);
		cfg.light.ambient = readVec3(light, "ambient", cfg.light.ambient);
	}
	sol::table lights = root["lights"];
	if (lights.valid() && lights.is<sol::table>()) {
		for (uint32_t i = 1;; ++i) {
			sol::table lt = lights[i];
			if (!lt.valid() || !lt.is<sol::table>())
				break;
			Light L;
			L.direction = readVec3(lt, "direction", L.direction);
			L.position = readVec3(lt, "position", L.position);
			L.color = readVec3(lt, "color", L.color);
			readFloat(lt, "intensity", L.intensity);
			readFloat(lt, "range", L.range);
			float type = static_cast<float>(L.type);
			readFloat(lt, "type", type);
			L.type = static_cast<uint32_t>(type);
			cfg.lights.push_back(L);
		}
	}
	if (cfg.lights.empty()) {
		cfg.lights.push_back(cfg.light);  // 回落：旧单灯 light 段合成主光（lights[0]）
		return;
	}
	cfg.lights[0].ambient = cfg.light.ambient;
	// ambient = 场景共享项（仅 lights[0] 进 scene data @36），light 段保持唯一可配源；
	// per-light 不解析 ambient（契约见函数 doc）
	if (auto* logger = RLLogger::getInstance())
		logger->info("scene: lights 段生效（{} 灯），light 段仅作 ambient 源（per-light ambient 不解析）",
			cfg.lights.size());
}

/**
 * environment 段解析（Phase 5.5；缺段 = 不启用）。faces 按 Vulkan 层序 0-5 排列。
 * Phase 8 ②b 新增 hdr（.hdr 环境路径，非空优先于 faces）+ hdr_size（重投影面边长，
 * 必须 2 的幂，默认 2048）。键轨迹在段入口无条件记全六个字段键（同 parseEntities 口径，
 * 记录先于一切早退分支）。
 * @param root 配置根表
 * @param cfg 目标配置（写 environment）
 * @param trace 键轨迹
 * @return hdr_size 非 2 的幂 = Error
 */
std::expected<void, Error> parseEnvironment(sol::table root, SceneConfig& cfg,
	SceneKeyTrace& trace) {
	trace.read.emplace_back("environment.dir");
	trace.read.emplace_back("environment.faces");
	trace.read.emplace_back("environment.intensity");
	trace.read.emplace_back("environment.skybox");
	trace.read.emplace_back("environment.hdr");
	trace.read.emplace_back("environment.hdr_size");
	sol::table envTable = root["environment"];
	if (!(envTable.valid() && envTable.is<sol::table>()))
		return {};
	cfg.environment.enabled = true;
	readStr(envTable, "dir", cfg.environment.dir);
	sol::table faces = envTable["faces"];
	if (faces.valid() && faces.is<sol::table>()) {
		for (int i = 1; i <= 6; ++i) {
			sol::optional<std::string> face = faces[i];
			if (face)
				cfg.environment.faces[i - 1] = *face;
		}
	}
	readFloat(envTable, "intensity", cfg.environment.intensity);
	readBool(envTable, "skybox", cfg.environment.skybox);
	readStr(envTable, "hdr", cfg.environment.hdr);
	if (!envTable["hdr_size"].valid())
		return {};
	sol::optional<float> hdrSize = envTable["hdr_size"];
	if (!hdrSize)
		return {};
	cfg.environment.hdrSize = static_cast<uint32_t>(*hdrSize);
	const uint32_t s = cfg.environment.hdrSize;
	if (s == 0 || (s & (s - 1)) != 0)
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("scene config: environment.hdr_size {} 须为 2 的幂", s)});
	return {};
}

/** styles 表解析（风格名→管线规格名，Phase 4 批次 3；非空覆盖默认 {unlit,pbr}）。
 *  map 段键名任意 ⟹ 键轨迹只记表名（与 lint 的「`.` 前缀」判据一致）。 */
void parseStyles(sol::table root, SceneConfig& cfg, SceneKeyTrace& trace) {
	trace.read.emplace_back("styles");
	sol::table styles = root["styles"];
	if (!(styles.valid() && styles.is<sol::table>()))
		return;
	cfg.styleMap.clear();
	for (auto [key, value] : styles)
		cfg.styleMap[key.as<std::string>()] = value.as<std::string>();
}

/** material_styles 解析（Phase 8 批次 F，材质名连连看：数组 { pattern=..., style=... }，
 * 有序首个命中；非空覆盖默认）。键轨迹在段入口无条件记 pattern/style 两键。 */
void parseMaterialStyles(sol::table root, SceneConfig& cfg, SceneKeyTrace& trace) {
	trace.read.emplace_back("material_styles.pattern");
	trace.read.emplace_back("material_styles.style");
	sol::table rules = root["material_styles"];
	if (!(rules.valid() && rules.is<sol::table>()))
		return;
	cfg.materialRules.clear();
	for (int i = 1; i <= static_cast<int>(rules.size()); ++i) {
		sol::table item = rules[i];
		if (!(item.valid() && item.is<sol::table>()))
			continue;
		sol::optional<std::string> pattern = item["pattern"];
		sol::optional<std::string> style = item["style"];
		if (pattern && style)
			cfg.materialRules.emplace_back(*pattern, *style);
	}
}

/** deferred_styles 解析（Phase 8 批次 F，风格名→deferred 规格名；非空覆盖默认。
 * 未列出的风格在 deferred 下回落 deferred_geometry 本体）。map 段只记表名。 */
void parseDeferredStyles(sol::table root, SceneConfig& cfg, SceneKeyTrace& trace) {
	trace.read.emplace_back("deferred_styles");
	sol::table styles = root["deferred_styles"];
	if (!(styles.valid() && styles.is<sol::table>()))
		return;
	cfg.deferredStyleMap.clear();
	for (auto [key, value] : styles)
		cfg.deferredStyleMap[key.as<std::string>()] = value.as<std::string>();
}

}  // namespace

/** loadSceneConfig 定义（接口注释见 scene_config.cppm）：沙箱 dofile + 段调度（entities/pipelines/lights/environment/styles 各 parse* helper，Phase ②b 拆债）；缺文件回落默认。键轨迹 + 未知键 lint 同 diagnostics（债-11）。 */
[[nodiscard]] std::expected<SceneConfig, Error> loadSceneConfig(
	std::string_view path, SceneKeyTrace* trace) {
	auto* logger = RLLogger::getInstance();
	// 键轨迹**总是**记录（未知键 lint 在启动期就要工作，main 传 nullptr 也不例外）；
	// 调用方给了出参就带出去，没给就落在本地局部量（diagnostics 同口径）。
	SceneKeyTrace fallback;
	if (!trace)
		trace = &fallback;

	{
		std::string pathStr{std::string(path)};
		std::ifstream probe{pathStr};
		if (!probe.is_open()) {
			if (logger)
				logger->warn("Scene config not found ({}), using defaults", std::string(path));
			return defaultSceneConfig();
		}
	}

	sol::state lua;
	lua.open_libraries(sol::lib::base, sol::lib::table);
	lua["dofile"] = sol::nil;
	lua["loadfile"] = sol::nil;
	lua["package"] = sol::nil;
	lua["require"] = sol::nil;

	auto result = lua.do_file(std::string(path).c_str());
	if (!result.valid()) {
		sol::optional<sol::error> err = result.get<sol::optional<sol::error>>();
		std::string msg = err ? err->what() : std::string("unknown lua error");
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("scene config ({}): {}", path, msg)});
	}

	try {
		// 同 camera.cppm：此版本 sol2 的 stack_proxy→sol::table 隐式转换有坑，用 .as<>
		sol::table root = result[0].as<sol::table>();
		SceneConfig cfg = defaultSceneConfig();
		collectPresentKeys(root, *trace);

		trace->read.emplace_back("model");
		readStr(root, "model", cfg.modelPath);
		trace->read.emplace_back("spin");
		readBool(root, "spin", cfg.spin);
		// 帧驱动 spin（诊断激励门，默认 0 = 关 = 逐位今天；判据本体 = frameDrivenSpinAngle）
		trace->read.emplace_back("spin_frame_angle");
		readFloat(root, "spin_frame_angle", cfg.spinFrameAngle);
		trace->read.emplace_back("spin_axis");
		cfg.spinAxis = readVec3(root, "spin_axis", cfg.spinAxis);
		trace->read.emplace_back("cube");
		readBool(root, "cube", cfg.showCube);
		trace->read.emplace_back("exposure");
		readFloat(root, "exposure", cfg.exposure);  // Phase 8 收尾：tonemap 曝光（可选，1.0 = 直通）
		// Phase 11 批 11.4：SSAO 强度（可选段 ao.strength，0 = 逐位旁路）
		trace->read.emplace_back("ao.strength");
		if (sol::table ao = root["ao"]; ao.valid() && ao.is<sol::table>())
			readFloat(ao, "strength", cfg.aoStrength);
		// 批 12.6 TAA（可选段 taa）：批 14.6 起本段**只有调参**（feedback / jitterScale /
		// motionCompensation / baseSpace / 各消融门）；原 `enabled` 图形态开关已删，
		// 「TAA 在不在图里」由 runtime.lua 的 aa 三态推导（RuntimeConfig::aaWantsTaa）。
		trace->read.emplace_back("taa.feedback");
		trace->read.emplace_back("taa.jitter_scale");
		trace->read.emplace_back("taa.motion_compensation");
		trace->read.emplace_back("taa.base_space");
		trace->read.emplace_back("taa.reprojection");
		trace->read.emplace_back("taa.diag_reject");
		trace->read.emplace_back("taa.input_kernel");
		trace->read.emplace_back("taa.clip_mode");
		trace->read.emplace_back("taa.clip_soft");
		if (sol::table taa = root["taa"]; taa.valid() && taa.is<sol::table>()) {
			readFloat(taa, "feedback", cfg.taaFeedback);
			readFloat(taa, "jitter_scale", cfg.taaJitterScale);
			readBool(taa, "motion_compensation", cfg.taaMotionCompensation);
			// 批 13.2 基空间输出（键名对齐引擎侧 lua 惯例：snake_case）
			readBool(taa, "base_space", cfg.taaBaseSpace);
			// 重投影消融门（默认 true = 逐位今天；关 = 历史按同基位置采样，见字段注释）
			readBool(taa, "reprojection", cfg.taaReprojection);
			// 历史拒收诊断门（默认关；开 = 拒收码编进 taa_out alpha，RGB 逐位不变）
			readBool(taa, "diag_reject", cfg.taaDiagReject);
			// 输入侧重建核选择（批 13.8；默认 0 = 双线性 = 逐位今天；1 = CR 4×4，2 = Lanczos3）
			readFloat(taa, "input_kernel", cfg.taaInputKernel);
			// 历史裁剪消融门（批 13.11；默认 0 = 现行 = 逐位今天）。λ 只在 mode=4 时被读。
			readFloat(taa, "clip_mode", cfg.taaClipMode);
			readFloat(taa, "clip_soft", cfg.taaClipSoft);
		}

		if (auto r = parseEntities(root, cfg, *trace); !r)
			return std::unexpected(r.error());
		if (auto r = parsePipelines(root, cfg, *trace); !r)
			return std::unexpected(r.error());
		parseLights(root, cfg, *trace);
		if (auto r = parseEnvironment(root, cfg, *trace); !r)
			return std::unexpected(r.error());
		parseStyles(root, cfg, *trace);
		parseMaterialStyles(root, cfg, *trace);
		parseDeferredStyles(root, cfg, *trace);

		// 未知键 lint（债-11：scene.lua 里拼错的键此前完全静默）
		lintUnknownSceneKeys(*trace);

		return cfg;
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("scene config ({}): {}", path, e.what())});
	}
}

/** sceneConfigKnownKeys 定义（接口注释见 scene_config.cppm）。 */
std::vector<std::string> sceneConfigKnownKeys() {
	std::vector<std::string> keys;
	keys.reserve(std::size(kSceneKeys));
	for (const auto& k : kSceneKeys)
		keys.emplace_back(k);
	return keys;
}

}  // namespace RGL
