export module scene_config;

import rlstd;
import vkerror;
import rllogger;
import scene;

namespace RGL {

// Lua vec3 读取（可选字段回落默认值，同 camera.cppm loadCameraConfig 惯例）：{x,y,z} → glm::vec3
// readVec3 + 配置加载定义随 scene_config_impl.cppm 迁出（本接口只留类型与声明）

// 一条 pipeline 规格：shader 风格的 SPV 路径（由 configs/scene.lua 传入，
// 取代原 vkrenderer.cppm 里写死的 SPV 文件名）。
export struct PipelineSpec {
	std::string name;    // "cube" / "model"
	std::string meshSpv; // mesh shader SPV 相对路径
	std::string fragSpv; // fragment shader SPV 相对路径
};

// 环境贴图（Phase 5.5：scene.lua environment 段，可选缺省 = 不启用）。faces 按
// **Vulkan 层序** 0-5 = (+X,−X,+Y,−Y,+Z,−Z) 排列（素材面顺序定案表 =
// docs/archive/phase5_5_plan.md 素材定案；nightSky 层序 1,3,5,6,2,4 / rainbow 方案 B
// ft,bk,up,dn,rt,lf），完整路径 = dir + "/" + faces[i]。intensity 进 scene data
// @48（env.slang readEnvParams）；skybox = 背景天穹开关（批次 4 消费）。
export struct EnvironmentConfig {
	bool enabled = false;
	std::string dir;
	std::array<std::string, 6> faces;
	float intensity = 1.0f;
	bool skybox = true;
};

// 场景配置：加载哪个模型 + 用哪些 pipeline + 是否场景旋转。
// 沿用 Phase 1 确立的 configs/*.lua 模式（sol2 沙箱 + 字段可选回落默认值）。
export struct SceneConfig {
	std::string modelPath;                 // glTF/glTF-Binary 路径（空 = 不加载模型，仅 cube）
	std::vector<PipelineSpec> pipelines;   // pipeline 规格列表
	std::map<std::string, std::string> styleMap;  // Phase 4 批次 3：风格名→管线规格名
	bool spin = true;                      // 场景整体时间旋转（便于肉眼判断渲染对错）
	bool showCube = true;                  // 是否渲染硬编码 cube（Phase 1 遗留；模型在场时通常关掉）
	Light light;                           // Phase 5 批次 2：定向光（scene.lua light 段，可选回落默认）
	EnvironmentConfig environment;         // Phase 5.5：环境贴图（scene.lua environment 段，可选缺省）
};

// 默认配置：cube + model + model_pbr + model_phong 四条 pipeline（SPV 路径与构建产物对应），
// 无模型，开启旋转。默认 styles：unlit→model / pbr→model_pbr（Phase 4 批次 3，styles 表可选回落；
// Phase 5 批次 3：改 pbr = "model_phong" 即切基础 Phong 路线，零 C++ 改动）。
export SceneConfig defaultSceneConfig();

// 从 Lua 配置初始化场景（字段可选，缺省回落默认值）。
// 文件缺失 → 返回默认 SceneConfig + warn；脚本/类型错误 → std::unexpected。
// 沙箱限制与 camera.cppm / tools/schema_loader 一致：禁用 dofile/loadfile/package/require。
export [[nodiscard]] std::expected<SceneConfig, Error> loadSceneConfig(std::string_view path);

}  // namespace RGL
