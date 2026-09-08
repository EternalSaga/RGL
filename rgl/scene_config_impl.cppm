module;
#include <sol/sol.hpp>
#include <glm/glm.hpp>
#include <fmt/format.h>
module scene_config:impl;

import scene_config;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import vkerror;
import rllogger;
import scene;

namespace RGL {

// readVec3（内部 helper）+ defaultSceneConfig/loadSceneConfig 定义（2026-09-02
// 编译提速 A 方案）：sol2 实现移出接口 TU；本单元是叶子边。

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

SceneConfig defaultSceneConfig() {
	SceneConfig cfg;
	cfg.pipelines = {
		{"cube", "generated_shaders/cube.mesh.spv", "generated_shaders/cube.frag.spv"},
		{"model", "generated_shaders/model.mesh.spv", "generated_shaders/model.frag.spv"},
		{"model_pbr", "generated_shaders/model.mesh.spv", "generated_shaders/model_pbr.frag.spv"},
		{"model_phong", "generated_shaders/model.mesh.spv", "generated_shaders/model_phong.frag.spv"},
		{"model_pbr_ibl", "generated_shaders/model.mesh.spv", "generated_shaders/model_pbr_ibl.frag.spv"},
		{"skybox", "generated_shaders/skybox.mesh.spv", "generated_shaders/skybox.frag.spv"},
	};
	cfg.styleMap = {{"unlit", "model"}, {"pbr", "model_pbr"}};
	cfg.spin = true;
	return cfg;
}

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

		sol::optional<std::string> model = root["model"];
		if (model)
			cfg.modelPath = *model;

		sol::optional<bool> spin = root["spin"];
		if (spin)
			cfg.spin = *spin;

		sol::optional<bool> showCube = root["cube"];
		if (showCube)
			cfg.showCube = *showCube;

		// Phase 6 批次 5：entities 段（场景构成数据驱动列表，非空 = 权威；缺段 =
		// 回落旧 model/cube 字段合成，既有配置零改动等价）。type "model"|"cube"，
		// transform 全可选（position/rotation 欧拉角度/scale 均匀）
		sol::table entities = root["entities"];
		if (entities.valid() && entities.is<sol::table>()) {
			cfg.entities.clear();
			for (int i = 1; i <= static_cast<int>(entities.size()); ++i) {
				sol::table item = entities[i];
				SceneEntityDesc ent;
				sol::optional<std::string> type = item["type"];
				if (type)
					ent.type = *type;
				if (ent.type != "model" && ent.type != "cube")
					return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
						fmt::format("scene config: entities[{}] unknown type '{}' (want model|cube)",
							i, ent.type)});
				sol::optional<std::string> file = item["file"];
				if (file)
					ent.file = *file;
				if (ent.type == "model" && ent.file.empty())
					return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
						fmt::format("scene config: entities[{}] type 'model' missing 'file'", i)});
				ent.position = readVec3(item, "position", ent.position);
				ent.rotation = readVec3(item, "rotation", ent.rotation);
				sol::optional<float> scale = item["scale"];
				if (scale)
					ent.scale = *scale;
				if (ent.scale <= 0.0f)
					return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
						fmt::format("scene config: entities[{}] scale must be > 0", i)});
				cfg.entities.push_back(std::move(ent));
			}
		}
		// 缺段兼容合成（entities 空 = 旧 model/cube 字段语义，行为逐位不变）
		if (cfg.entities.empty()) {
			if (!cfg.modelPath.empty())
				cfg.entities.push_back(SceneEntityDesc{.type = "model", .file = cfg.modelPath});
			if (cfg.showCube)
				cfg.entities.push_back(SceneEntityDesc{.type = "cube"});
		}

		sol::table pipelines = root["pipelines"];
		if (pipelines.valid() && pipelines.is<sol::table>()) {
			cfg.pipelines.clear();
			for (int i = 1; i <= static_cast<int>(pipelines.size()); ++i) {
				sol::table item = pipelines[i];
				PipelineSpec spec;
				sol::optional<std::string> name = item["name"];
				if (name)
					spec.name = *name;
				sol::optional<std::string> mesh = item["mesh"];
				if (mesh)
					spec.meshSpv = *mesh;
				sol::optional<std::string> frag = item["frag"];
				if (frag)
					spec.fragSpv = *frag;
				if (spec.name.empty()) {
					return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
						fmt::format("scene config: pipeline[{}] missing 'name'", i)});
				}
				cfg.pipelines.push_back(std::move(spec));
			}
		}

		// Phase 5 批次 2：light 段（定向光，全字段可选回落 Light 默认值）
		sol::table light = root["light"];
		if (light.valid() && light.is<sol::table>()) {
			cfg.light.direction = readVec3(light, "direction", cfg.light.direction);
			cfg.light.color = readVec3(light, "color", cfg.light.color);
			sol::optional<float> intensity = light["intensity"];
			if (intensity)
				cfg.light.intensity = *intensity;
			cfg.light.ambient = readVec3(light, "ambient", cfg.light.ambient);
		}

		// Phase 7 D3：lights 列表（多灯，延迟 light pass 遍历；缺段回落 [light] 单灯）。
		// 逐字段可选回落 Light 默认。type=0 定向（direction）/ 1 点光（position+range）。
		// ambient 是场景共享项（仅 lights[0] 进 scene data @36），故 per-light 不解析 ambient。
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
				sol::optional<float> intensity = lt["intensity"];
				if (intensity)
					L.intensity = *intensity;
				sol::optional<float> range = lt["range"];
				if (range)
					L.range = *range;
				sol::optional<int> type = lt["type"];
				if (type)
					L.type = static_cast<uint32_t>(*type);
				cfg.lights.push_back(L);
			}
		}
		if (cfg.lights.empty())
			cfg.lights.push_back(cfg.light);  // 回落：旧单灯 light 段合成主光（lights[0]）

		// Phase 5.5：environment 段（环境 cubemap，缺段 = 不启用；faces 按 Vulkan 层序
		// 0-5 排列，见 EnvironmentConfig 注释）
		sol::table envTable = root["environment"];
		if (envTable.valid() && envTable.is<sol::table>()) {
			cfg.environment.enabled = true;
			sol::optional<std::string> dir = envTable["dir"];
			if (dir)
				cfg.environment.dir = *dir;
			sol::table faces = envTable["faces"];
			if (faces.valid() && faces.is<sol::table>()) {
				for (int i = 1; i <= 6; ++i) {
					sol::optional<std::string> face = faces[i];
					if (face)
						cfg.environment.faces[i - 1] = *face;
				}
			}
			sol::optional<float> intensity = envTable["intensity"];
			if (intensity)
				cfg.environment.intensity = *intensity;
			sol::optional<bool> skybox = envTable["skybox"];
			if (skybox)
				cfg.environment.skybox = *skybox;
		}

		// Phase 4 批次 3：styles 表（风格名→管线规格名，可选回落默认 {unlit,pbr}）
		sol::table styles = root["styles"];
		if (styles.valid() && styles.is<sol::table>()) {
			cfg.styleMap.clear();
			for (auto [key, value] : styles) {
				auto styleName = key.as<std::string>();
				auto pipelineName = value.as<std::string>();
				cfg.styleMap[styleName] = pipelineName;
			}
		}

		return cfg;
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("scene config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
