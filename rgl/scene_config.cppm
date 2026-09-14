module;
#include <glm/glm.hpp>  // SceneEntityDesc 成员 glm::vec3 需名可见（同 render_list.cppm 先例）
export module scene_config;

import rlstd;
import vkerror;
import rllogger;
import scene_context;

namespace RGL {

// Lua vec3 读取（可选字段回落默认值，同 camera.cppm loadCameraConfig 惯例）：{x,y,z} → glm::vec3
// readVec3 + 配置加载定义随 scene_config_impl.cppm 迁出（本接口只留类型与声明）

/**
 * 一条 pipeline 规格：shader 风格的 SPV 路径（由 configs/scene.lua 传入，
 * 取代原 vkrenderer.cppm 里写死的 SPV 文件名）。
 */
export struct PipelineSpec {
	std::string name;    // "cube" / "model"
	std::string meshSpv; // mesh shader SPV 相对路径
	std::string fragSpv; // fragment shader SPV 相对路径
};

/**
 * 环境贴图（Phase 5.5：scene.lua environment 段，可选缺省 = 不启用）。faces 按
 * **Vulkan 层序** 0-5 = (+X,−X,+Y,−Y,+Z,−Z) 排列（素材面顺序定案表 =
 * docs/archive/phase5_5_plan.md 素材定案；nightSky 层序 1,3,5,6,2,4 / rainbow 方案 B
 * ft,bk,up,dn,rt,lf），完整路径 = dir + "/" + faces[i]。intensity 进 scene data
 * @48（env.slang readEnvParams）；skybox = 背景天穹开关（批次 4 消费）。
 *
 * Phase 8 批次 ②b：`hdr` 非空 = 走 Radiance .hdr 环境路径（image_codec::decodeHdr →
 * CPU 等距柱状→cubemap 重投影 → RGBA16F 上传），**优先于** dir/faces 六面 PNG 路径；
 * .hdr 文件缺失时回落 PNG 路径（素材 gitignore，干净 checkout 仍可运行）。
 * hdrSize = 重投影面边长（2 的幂，默认 2048）。
 */
export struct EnvironmentConfig {
	bool enabled = false;
	std::string dir;
	std::array<std::string, 6> faces;
	float intensity = 1.0f;
	bool skybox = true;
	std::string hdr;      // Phase 8 ②b：.hdr 环境路径（非空优先于 faces）
	uint32_t hdrSize = 2048;  // 重投影面边长（2 的幂）
};

/**
 * 场景 entity 声明（Phase 6 批次 5，scene.lua entities 段）："场景里有什么"的
 * 数据驱动列表——每项注册为场景实体（画不画 = entity 有无，ECS 规范 3）。
 * type = "model"（file = glTF 路径）| "cube"（内建几何，无需 file，见 builtin_geometry）。
 * transform（仅 model/cube 通用）：position 世界平移 + rotation 欧拉角（度，
 * 应用序 X→Y→Z）+ scale 均匀缩放（约束同 Phase 5 已知近似②：world 法线 mat3
 * 直乘假定均匀缩放）。缺省 = identity（模型原始位姿）。
 */
export struct SceneEntityDesc {
	std::string type = "model";
	std::string file;
	glm::vec3 position{0.0f};
	glm::vec3 rotation{0.0f};
	float scale = 1.0f;
};

/**
 * 场景配置：加载哪些模型 + 用哪些 pipeline + 是否场景旋转。
 * 沿用 Phase 1 确立的 configs/*.lua 模式（sol2 沙箱 + 字段可选回落默认值）。
 */
export struct SceneConfig {
	std::string modelPath;                 // 旧单模型字段（entities 缺省时的兼容合成源，空 = 无模型）
	std::vector<SceneEntityDesc> entities; // Phase 6 批次 5：entity 列表（非空 = 权威场景构成）
	std::vector<PipelineSpec> pipelines;   // pipeline 规格列表
	std::map<std::string, std::string> styleMap;  // Phase 4 批次 3：风格名→管线规格名
	// Phase 8 批次 F：材质名连连看规则表（有序，首个命中生效）——材质名含 pattern → 风格键。
	// forward 经 styleMap、deferred 经 deferredStyleMap 落到各自规格名；空 = 全走 pbr。
	std::vector<std::pair<std::string, std::string>> materialRules;
	std::map<std::string, std::string> deferredStyleMap;  // 风格名→deferred 规格名（缺省风格 = deferred_geometry）
	bool spin = true;                      // 场景整体时间旋转（便于肉眼判断渲染对错）
	bool showCube = true;                  // 是否渲染硬编码 cube（Phase 1 遗留；模型在场时通常关掉）
	Light light;                           // Phase 5 批次 2：定向光（scene.lua light 段，可选回落默认）
	std::vector<Light> lights;             // Phase 7 D3：多灯（scene.lua lights 列表；空则 loadSceneConfig 回落合成 [light]）
	EnvironmentConfig environment;         // Phase 5.5：环境贴图（scene.lua environment 段，可选缺省）
	float exposure = 1.0f;                 // Phase 8 收尾：tonemap 曝光（scene.lua 顶层 exposure，ACES 前线性倍率，1.0 = 直通）
};

/**
 * 默认配置：cube + model + model_pbr + model_phong（+model_pbr_ibl + skybox）
 * 六条内建 pipeline（SPV 路径与构建产物对应），无模型，开启旋转。
 * 默认 styles：unlit→model / pbr→model_pbr（Phase 4 批次 3，styles 表可选回落；
 * Phase 5 批次 3：改 pbr = "model_phong" 即切基础 Phong 路线，零 C++ 改动）。
 * @return 默认 SceneConfig（值）
 */
export SceneConfig defaultSceneConfig();

/**
 * 从 Lua 配置（configs/scene.lua）初始化场景（字段可选，缺省回落默认值）。
 * 文件缺失 → 返回默认 SceneConfig + warn；脚本/类型错误 → std::unexpected。
 * 沙箱限制与 camera.cppm / tools/schema_loader 一致：禁用 dofile/loadfile/package/require。
 * @param path 配置文件路径
 * @return 解析好的 SceneConfig；失败返回 ConfigLoadFailed
 */
export [[nodiscard]] std::expected<SceneConfig, Error> loadSceneConfig(std::string_view path);

}  // namespace RGL
