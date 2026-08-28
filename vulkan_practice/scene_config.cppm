module;
#include <sol/sol.hpp>
export module scene_config;

import rlstd;
import vkerror;
import rllogger;

namespace RGL {

// 一条 pipeline 规格：shader 风格的 SPV 路径（由 configs/scene.lua 传入，
// 取代原 vkrenderer.cppm 里写死的 SPV 文件名）。
export struct PipelineSpec {
	std::string name;    // "cube" / "model"
	std::string meshSpv; // mesh shader SPV 相对路径
	std::string fragSpv; // fragment shader SPV 相对路径
};

// 场景配置：加载哪个模型 + 用哪些 pipeline + 是否场景旋转。
// 沿用 Phase 1 确立的 configs/*.lua 模式（sol2 沙箱 + 字段可选回落默认值）。
export struct SceneConfig {
	std::string modelPath;                 // glTF/glTF-Binary 路径（空 = 不加载模型，仅 cube）
	std::vector<PipelineSpec> pipelines;   // pipeline 规格列表
	std::map<std::string, std::string> styleMap;  // Phase 4 批次 3：风格名→管线规格名
	bool spin = true;                      // 场景整体时间旋转（便于肉眼判断渲染对错）
	bool showCube = true;                  // 是否渲染硬编码 cube（Phase 1 遗留；模型在场时通常关掉）
};

// 默认配置：cube + model + model_pbr 三条 pipeline（SPV 路径与构建产物对应），无模型，开启旋转。
// 默认 styles：unlit→model / pbr→model_pbr（Phase 4 批次 3，styles 表可选回落）。
export SceneConfig defaultSceneConfig() {
	SceneConfig cfg;
	cfg.pipelines = {
		{"cube", "generated_shaders/cube.mesh.spv", "generated_shaders/cube.frag.spv"},
		{"model", "generated_shaders/model.mesh.spv", "generated_shaders/model.frag.spv"},
		{"model_pbr", "generated_shaders/model.mesh.spv", "generated_shaders/model_pbr.frag.spv"},
	};
	cfg.styleMap = {{"unlit", "model"}, {"pbr", "model_pbr"}};
	cfg.spin = true;
	return cfg;
}

// 从 Lua 配置初始化场景（字段可选，缺省回落默认值）。
// 文件缺失 → 返回默认 SceneConfig + warn；脚本/类型错误 → std::unexpected。
// 沙箱限制与 camera.cppm / tools/schema_loader 一致：禁用 dofile/loadfile/package/require。
export [[nodiscard]] std::expected<SceneConfig, Error> loadSceneConfig(std::string_view path) {
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
			std::string("scene config (") + std::string(path) + "): " + msg});
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
						"scene config: pipeline[" + std::to_string(i) + "] missing 'name'"});
				}
				cfg.pipelines.push_back(std::move(spec));
			}
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
			std::string("scene config (") + std::string(path) + "): " + e.what()});
	}
}

}  // namespace RGL
