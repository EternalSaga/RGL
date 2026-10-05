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

/** 顶层表取 uint32 字段（缺失/负数 = no-op 回落现值；lua 无负数下界，越界负值直接忽略）。 */
void readU32(sol::table table, const char* key, uint32_t& into) {
	sol::optional<int64_t> v = table[key];
	if (v && *v >= 0)
		into = static_cast<uint32_t>(*v);
}

/** defaultSceneConfig 定义（接口注释见 scene_config.cppm）：六条内建 pipeline + styles {unlit,pbr} + spin。 */
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
		{"skybox", "generated_shaders/skybox.mesh.spv", "generated_shaders/skybox.frag.spv"},
	};
	cfg.styleMap = {{"unlit", "model"}, {"pbr", "model_pbr"},
		{"vegetation", "model_vegetation"}, {"light", "model_light"}};
	cfg.materialRules = {{"Emissive", "light"}, {"Foliage", "vegetation"}};
	cfg.deferredStyleMap = {{"vegetation", "deferred_geometry_vegetation"},
		{"light", "deferred_geometry_light"}};
	cfg.spin = true;
	return cfg;
}

namespace {

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
 * @param root 配置根表
 * @param cfg 目标配置（读 modelPath/showCube 做缺段合成，写 entities）
 * @return 结构违例（未知 type / model 缺 file / scale≤0）= Error
 */
std::expected<void, Error> parseEntities(sol::table root, SceneConfig& cfg) {
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
 * @param root 配置根表
 * @param cfg 目标配置（写 pipelines）
 * @return 缺 name = Error
 */
std::expected<void, Error> parsePipelines(sol::table root, SceneConfig& cfg) {
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
 * @param root 配置根表
 * @param cfg 目标配置（写 light/lights）
 */
void parseLights(sol::table root, SceneConfig& cfg) {
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
 * 必须 2 的幂，默认 2048）。
 * @param root 配置根表
 * @param cfg 目标配置（写 environment）
 * @return hdr_size 非 2 的幂 = Error
 */
std::expected<void, Error> parseEnvironment(sol::table root, SceneConfig& cfg) {
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

/** styles 表解析（风格名→管线规格名，Phase 4 批次 3；非空覆盖默认 {unlit,pbr}）。 */
void parseStyles(sol::table root, SceneConfig& cfg) {
	sol::table styles = root["styles"];
	if (!(styles.valid() && styles.is<sol::table>()))
		return;
	cfg.styleMap.clear();
	for (auto [key, value] : styles)
		cfg.styleMap[key.as<std::string>()] = value.as<std::string>();
}

/** material_styles 解析（Phase 8 批次 F，材质名连连看：数组 { pattern=..., style=... }，
 * 有序首个命中；非空覆盖默认）。 */
void parseMaterialStyles(sol::table root, SceneConfig& cfg) {
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
 * 未列出的风格在 deferred 下回落 deferred_geometry 本体）。 */
void parseDeferredStyles(sol::table root, SceneConfig& cfg) {
	sol::table styles = root["deferred_styles"];
	if (!(styles.valid() && styles.is<sol::table>()))
		return;
	cfg.deferredStyleMap.clear();
	for (auto [key, value] : styles)
		cfg.deferredStyleMap[key.as<std::string>()] = value.as<std::string>();
}

}  // namespace

/** loadSceneConfig 定义（接口注释见 scene_config.cppm）：沙箱 dofile + 段调度（entities/pipelines/lights/environment/styles 各 parse* helper，Phase ②b 拆债）；缺文件回落默认。 */
[[nodiscard]] std::expected<SceneConfig, Error> loadSceneConfig(std::string_view path) {
	auto* logger = RLLogger::getInstance();

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

		readStr(root, "model", cfg.modelPath);
		readBool(root, "spin", cfg.spin);
		// 帧驱动 spin（诊断激励门，默认 0 = 关 = 逐位今天；判据本体 = frameDrivenSpinAngle）
		readFloat(root, "spin_frame_angle", cfg.spinFrameAngle);
		cfg.spinAxis = readVec3(root, "spin_axis", cfg.spinAxis);
		readBool(root, "cube", cfg.showCube);
		readFloat(root, "exposure", cfg.exposure);  // Phase 8 收尾：tonemap 曝光（可选，1.0 = 直通）
		// Phase 11 批 11.4：SSAO 强度（可选段 ao.strength，0 = 逐位旁路）
		if (sol::table ao = root["ao"]; ao.valid() && ao.is<sol::table>())
			readFloat(ao, "strength", cfg.aoStrength);
		// 批 12.6 TAA（可选段 taa；缺段 = 全默认即「关」）：enabled 决定图形态，
		// 其余是每帧运行参数（feedback / jitterScale / motionCompensation / baseSpace）
		if (sol::table taa = root["taa"]; taa.valid() && taa.is<sol::table>()) {
			readBool(taa, "enabled", cfg.taaEnabled);
			readFloat(taa, "feedback", cfg.taaFeedback);
			readFloat(taa, "jitter_scale", cfg.taaJitterScale);
			readBool(taa, "motion_compensation", cfg.taaMotionCompensation);
			// 批 13.2 基空间输出（键名对齐引擎侧 lua 惯例：snake_case）
			readBool(taa, "base_space", cfg.taaBaseSpace);
			// 批 13.7 静态机位历史权重提升（static boost；总门默认关 = 逐位今天行为）。
			// ⚠ static_feedback 是**本帧权重**（同 feedback 语义）：调小 = 更信历史。
			readBool(taa, "static_boost", cfg.taaStaticBoost);
			readFloat(taa, "static_feedback", cfg.taaStaticFeedback);
			readU32(taa, "static_ramp", cfg.taaStaticRamp);
			// 重投影消融门（默认 true = 逐位今天；关 = 历史按同基位置采样，见字段注释）
			readBool(taa, "reprojection", cfg.taaReprojection);
			// 历史拒收诊断门（默认关；开 = 拒收码编进 taa_out alpha，RGB 逐位不变）
			readBool(taa, "diag_reject", cfg.taaDiagReject);
			// 输入侧重建核选择（批 13.8；默认 0 = 双线性 = 逐位今天；1 = CR 4×4，2 = Lanczos3）
			readFloat(taa, "input_kernel", cfg.taaInputKernel);
		}

		if (auto r = parseEntities(root, cfg); !r)
			return std::unexpected(r.error());
		if (auto r = parsePipelines(root, cfg); !r)
			return std::unexpected(r.error());
		parseLights(root, cfg);
		if (auto r = parseEnvironment(root, cfg); !r)
			return std::unexpected(r.error());
		parseStyles(root, cfg);
		parseMaterialStyles(root, cfg);
		parseDeferredStyles(root, cfg);

		return cfg;
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("scene config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
