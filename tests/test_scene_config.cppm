module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <filesystem>  // 临时 lua 落盘（AGENTS.md GMF 例外清单成员）

// 四份配置 loader 的解析契约（批 12.1 · B1）——被测对象 = scene_config /
// camera_config / runtime_config / diagnostics（rlcore 成员，sol2 沙箱 + 缺省回落）。
//
// ## 判据来源纪律
//
// 每条"回落/夹取"断言都先读了实现单元源码再写（scene_config_impl /
// camera_config_impl / runtime_config_impl / diagnostics_impl），与直觉不符处在
// 注释里显式标"**实际行为**"。反向用例（合法输入必须解析出非默认值）与正向回落
// 成对出现——否则"loader 什么都不解析"也能全绿。
//
// ## 与 dump_gbuf 闸门的联动
//
// diagnostics 的 dump_gbuf 越界夹取按 kGBufferChannelCount（gbuffer_contract 表长）
// 判界。本文件把**边界两侧**（count−1 保留 / count 夹到 −1）钉在常量上：G-buffer
// 加通道时表长变、闸门跟着动，而 lua 里写死的 4/5 会逼着改动者同步本测试。
//
// ## 临时文件策略
//
// loader 只吃文件路径（dofile），测试用 temp_directory_path 落最小 lua、RAII 删除。
// 不依赖 CWD（rgl 主程序要求 CWD=build/rgl/，测试不继承该约束）。
export module test_scene_config;

import rlstd;
import test_support;
import scene_config;
import camera_config;
import camera;  // 本文件直接调 cameraForward/cameraView（camera_config 是 import 非 export import）
import runtime_config;
import diagnostics;
import gbuffer_contract;
import vkerror;

namespace RGL {
namespace {

using testing::expectNear;

/** 临时 lua 配置（构造写盘、析构删除）。tag 全局唯一 = 文件名互不踩踏。 */
class TempLua {
  public:
	TempLua(std::string_view tag, std::string_view body)
	: path_(std::filesystem::temp_directory_path() / ("rgl_cfg_" + std::string(tag) + ".lua")) {
		std::ofstream out(path_);
		out << body;
	}
	TempLua(const TempLua&) = delete;
	TempLua& operator=(const TempLua&) = delete;
	~TempLua() {
		std::error_code ec;
		std::filesystem::remove(path_, ec);
	}
	/** 传给 loader 的路径字符串（生命周期随本对象）。 */
	std::string pathStr() const { return path_.string(); }

  private:
	std::filesystem::path path_;
};

/** 断言 vec3 三分量近似（配置解析常用）。 */
void expectVec(const glm::vec3& got, const glm::vec3& want, double absTol = 1e-5) {
	for (int k = 0; k < 3; ++k)
		expectNear(got[k], want[k], 0.0, absTol);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// scene_config
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("scene_config：缺文件回落默认（warn 不报错）——默认 = 11 管线 + 4 styles + 空 entities") {
	// 缺文件路径：temp 目录下不存在的名
	std::string missing = (std::filesystem::temp_directory_path() / "rgl_cfg_no_such_scene.lua").string();
	auto r = loadSceneConfig(missing);
	REQUIRE(r.has_value());
	// 默认画像（defaultSceneConfig，scene_config_impl.cppm:62-86）。
	// **实际行为**：缺文件路径**不经 parseEntities**，entities 保持**空**——
	// 与"存在但空的 lua"不同（下一条），这个不对称是既有语义，钉住防"顺手统一"。
	CHECK(r->pipelines.size() == 11);
	CHECK(r->styleMap.size() == 4);
	CHECK(r->materialRules.size() == 2);
	CHECK(r->deferredStyleMap.size() == 2);
	CHECK(r->entities.empty());
	CHECK(r->spin == true);
	CHECK(r->showCube == true);
	CHECK(r->modelPath.empty());
	expectNear(r->exposure, 1.0, 0.0, 1e-6);
	expectNear(r->aoStrength, 0.0, 0.0, 1e-6);
	CHECK(r->environment.enabled == false);
}

TEST_CASE("scene_config：空表 return {} → 各字段 = 默认值，且 entities 合成 [cube]、lights 回落 [light]") {
	TempLua f("sc_empty", "return {}\n");
	auto r = loadSceneConfig(f.pathStr());
	REQUIRE(r.has_value());
	// 顶层标量回落
	CHECK(r->spin == true);
	CHECK(r->showCube == true);
	expectNear(r->exposure, 1.0, 0.0, 1e-6);
	expectNear(r->aoStrength, 0.0, 0.0, 1e-6);
	// 管线/styles = 默认集（parsePipelines 缺段不覆盖）
	CHECK(r->pipelines.size() == 11);
	CHECK(r->styleMap.size() == 4);
	CHECK(r->styleMap.at("pbr") == "model_pbr");
	// **实际行为**（parseEntities 缺段合成，scene_config_impl.cppm:138-144）：
	// entities 空 + showCube 默认 true → 合成 [cube]。"空配置 ≠ 空场景"，空配置 =
	// 一个旋转的 cube。这条不对称同样被上一条钉着（缺文件 = 真空）。
	REQUIRE(r->entities.size() == 1);
	CHECK(r->entities[0].type == "cube");
	CHECK(r->entities[0].file.empty());
	// **实际行为**（parseLights 回落，:208-210）：lights 缺段/空段 → 合成 [light]
	REQUIRE(r->lights.size() == 1);
	expectVec(r->lights[0].direction, r->light.direction);
	expectNear(r->lights[0].intensity, 3.0, 0.0, 1e-6);
}

TEST_CASE("scene_config：显式 exposure = 2.5 被解析（回落断言的正向对照，批 12.3d · B1）") {
	// 实文（scene_config_impl.cppm:336）：readFloat(root, "exposure", cfg.exposure)——
	// Phase 8 收尾的 tonemap 曝光，可选字段，1.0 = 直通。前面两条只钉过"缺省回落 1.0"
	// （:92/:104），本条钉"显式值真的被写进来"：解析 key 名若漂移，cfg.exposure 保持
	// 默认 1.0，本断言红（变异红线：key "exposure" 改成别的 → 红）。
	TempLua f("sc_exposure", "return { exposure = 2.5 }\n");
	auto r = loadSceneConfig(f.pathStr());
	REQUIRE(r.has_value());
	expectNear(r->exposure, 2.5, 0.0, 1e-6);  // 2.5 float 精确可表示，只给绝对阈
	// 阳性对照的反方向：exposure 命中后 loader 并未提前返回——同表其它字段仍走各自契约
	// （缺段的回落合成照常：aoStrength 默认 0、spin 默认 true、entities 合成 [cube]）。
	expectNear(r->aoStrength, 0.0, 0.0, 1e-6);
	CHECK(r->spin == true);
	CHECK(r->entities.size() == 1);
}

TEST_CASE("scene_config：entities 列表解析（type/file/position/rotation/scale，rotation 度原样透传）") {
	TempLua f("sc_entities", R"lua(
return {
  entities = {
    { type = "cube" },
    { type = "model", file = "x.glb", position = { 1, 2, 3 },
      rotation = { 10, 20, 30 }, scale = 2 },
  },
}
)lua");
	auto r = loadSceneConfig(f.pathStr());
	REQUIRE(r.has_value());
	REQUIRE(r->entities.size() == 2);
	// cube 是退化路径：不带 file 不得报错（parseEntityItem 只对 type=="model" 查 file）
	CHECK(r->entities[0].type == "cube");
	CHECK(r->entities[0].file.empty());
	expectNear(r->entities[0].scale, 1.0, 0.0, 0.0);  // scale 缺省 = 1
	CHECK(r->entities[1].type == "model");
	CHECK(r->entities[1].file == "x.glb");
	expectVec(r->entities[1].position, glm::vec3{1.0f, 2.0f, 3.0f});
	// **rotation 是度**：loader 契约 = 原样透传（不转弧度）。若哪天 loader 里"顺手"
	// 转了弧度，这里 10 vs 0.1745 立刻红。应用序 X→Y→Z 的组装在 main_scene.cppm:265-267
	// （非 rlcore，装配层职责），loader 层可测的就是"度不被动"。
	expectNear(r->entities[1].rotation.x, 10.0, 0.0, 1e-5);
	expectNear(r->entities[1].rotation.y, 20.0, 0.0, 1e-5);
	expectNear(r->entities[1].rotation.z, 30.0, 0.0, 1e-5);
	expectNear(r->entities[1].scale, 2.0, 0.0, 1e-6);
	// 反向锚点：entities 在场时**权威**——缺段合成不得追加 [cube]（showCube 默认 true）
	CHECK(r->entities.size() == 2);
}

TEST_CASE("scene_config：entities 错误路径三连（未知 type 报错 / model 缺 file 报错 / scale≤0 报错）") {
	// 实际行为（读 parseEntityItem 源码，scene_config_impl.cppm:96-116）：
	// **未知 type = 报错退出**（不是回落默认）；model 缺 file = 报错；scale≤0 = 报错。
	{
		TempLua f("sc_badtype", R"lua(return { entities = { { type = "sphere" } } })lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().code == ErrorCode::ConfigLoadFailed);
		INFO(r.error().describe());
		CHECK(r.error().message.find("unknown type") != std::string::npos);
	}
	{
		TempLua f("sc_nofile", R"lua(return { entities = { { type = "model" } } })lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().message.find("missing 'file'") != std::string::npos);
	}
	{
		TempLua f("sc_scales0", R"lua(return { entities = { { type = "cube", scale = 0 } } })lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().message.find("scale") != std::string::npos);
	}
	// 反向对照：type 缺省按 "model" 处理（SceneEntityDesc.type 默认值）→ 无 file 报错
	{
		TempLua f("sc_notype", R"lua(return { entities = { { position = { 0, 0, 0 } } } })lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().message.find("missing 'file'") != std::string::npos);
	}
	// 阳性对照：同样三条改成合法值即通过（错误分支不是恒真分支）
	TempLua ok("sc_err_ok", R"lua(return { entities = { { type = "cube" } } })lua");
	CHECK(loadSceneConfig(ok.pathStr()).has_value());
}

TEST_CASE("scene_config：entities 缺段的旧字段合成（model= → [model]；cube=false 不出 cube）") {
	{  // 旧 model 字段（无 entities 段）→ 合成 [model, cube]（showCube 默认 true）
		TempLua f("sc_compat1", R"lua(return { model = "old.glb" })lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		REQUIRE(r->entities.size() == 2);
		CHECK(r->entities[0].type == "model");
		CHECK(r->entities[0].file == "old.glb");
		CHECK(r->entities[1].type == "cube");
	}
	{  // cube=false → 只剩 [model]
		TempLua f("sc_compat2", R"lua(return { model = "old.glb", cube = false })lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		REQUIRE(r->entities.size() == 1);
		CHECK(r->entities[0].type == "model");
	}
	{  // **实际行为**：entities = {} 显式空表 + cube=false + 无 model → 真空场景
		TempLua f("sc_compat3", R"lua(return { entities = {}, cube = false })lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->entities.empty());
	}
}

TEST_CASE("scene_config：lights 段（空段/缺段回落 [light]；两段并存时 lights[0].ambient 取 light 段）") {
	{  // lights = {} 空数组 → 与缺段同：回落合成 [light]（parseLights:190-210）
		TempLua f("sc_lights_empty", R"lua(return { lights = {} })lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		REQUIRE(r->lights.size() == 1);
		expectVec(r->lights[0].direction, glm::vec3{0.35f, -1.0f, 0.25f});
	}
	{  // light 段字段 → cfg.light，且被 lights 回落合成继承
		TempLua f("sc_light1",
			R"lua(return { light = { direction = { 0, -1, 0 }, color = { 1, 0.5, 0.25 }, intensity = 7 } })lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		expectVec(r->light.direction, glm::vec3{0.0f, -1.0f, 0.0f});
		expectVec(r->light.color, glm::vec3{1.0f, 0.5f, 0.25f});
		expectNear(r->light.intensity, 7.0, 0.0, 1e-6);
		REQUIRE(r->lights.size() == 1);
		expectVec(r->lights[0].direction, glm::vec3{0.0f, -1.0f, 0.0f});
		expectNear(r->lights[0].intensity, 7.0, 0.0, 1e-6);
	}
	{  // lights 段生效：2 灯（定向 + 点光）；**实际行为**：lights[0].ambient 被改写为
	   // light 段 ambient（场景共享项唯一可配源），lights[1].ambient 保持组件默认
	   // （per-light ambient 不解析——契约原文见 parseLights 函数头）。
		TempLua f("sc_lights2", R"lua(
return {
  light = { ambient = { 0.5, 0.5, 0.5 } },
  lights = {
    { direction = { 0, -1, 0 }, intensity = 2 },
    { position = { 1, 2, 3 }, intensity = 5, range = 8, type = 1 },
  },
}
)lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		REQUIRE(r->lights.size() == 2);
		expectNear(r->lights[0].intensity, 2.0, 0.0, 1e-6);
		expectVec(r->lights[0].ambient, glm::vec3{0.5f, 0.5f, 0.5f});  // light 段覆盖
		expectVec(r->lights[1].ambient, glm::vec3{0.03f, 0.03f, 0.035f});  // 组件默认
		CHECK(r->lights[1].type == 1u);
		expectVec(r->lights[1].position, glm::vec3{1.0f, 2.0f, 3.0f});
		expectNear(r->lights[1].range, 8.0, 0.0, 1e-6);
	}
}

TEST_CASE("scene_config：environment 段（缺段不启用；faces 层序；hdr_size 非 2 的幂报错）") {
	{  // 缺段 = 不启用（enabled=false，dir 空）
		TempLua f("sc_env_none", "return {}\n");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->environment.enabled == false);
		CHECK(r->environment.dir.empty());
		expectNear(r->environment.hdrSize, 2048.0, 0.0, 0.0);  // 默认面边长（uint32 精确）
	}
	{  // **实际行为**：environment 段存在即 enabled=true（无显式 enabled 字段）
		TempLua f("sc_env_on", R"lua(
return { environment = { dir = "sky", faces = { "px", "nx", "py", "ny", "pz", "nz" },
  intensity = 0.5, skybox = false, hdr_size = 1024 } }
)lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->environment.enabled == true);
		CHECK(r->environment.dir == "sky");
		// faces 按 Vulkan 层序 0-5（+X,−X,+Y,−Y,+Z,−Z）逐位透传
		CHECK(r->environment.faces[0] == "px");
		CHECK(r->environment.faces[5] == "nz");
		expectNear(r->environment.intensity, 0.5, 0.0, 1e-6);
		CHECK(r->environment.skybox == false);
		CHECK(r->environment.hdrSize == 1024u);
	}
	{  // hdr_size 非 2 的幂 = 报错（parseEnvironment:252-254）
		TempLua f("sc_env_pow2", R"lua(return { environment = { hdr_size = 100 } })lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().message.find("2") != std::string::npos);  // "须为 2 的幂"
	}
}

TEST_CASE("scene_config：pipelines/styles 非空覆盖默认；pipeline 缺 name 报错；语法错误报错") {
	{
		TempLua f("sc_pipe",
			R"lua(return { pipelines = { { name = "mine", mesh = "m.spv", frag = "f.spv" } },
  styles = { pbr = "mine" } })lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		REQUIRE(r->pipelines.size() == 1);  // 覆盖而非追加
		CHECK(r->pipelines[0].name == "mine");
		CHECK(r->pipelines[0].meshSpv == "m.spv");
		CHECK(r->pipelines[0].fragSpv == "f.spv");
		REQUIRE(r->styleMap.size() == 1);
		CHECK(r->styleMap.at("pbr") == "mine");
	}
	{
		TempLua f("sc_pipe_noname", R"lua(return { pipelines = { { mesh = "m.spv" } } })lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().message.find("name") != std::string::npos);
	}
	{  // Lua 语法错误 → do_file 失败 → ConfigLoadFailed（不是崩、不是回落）
		TempLua f("sc_syntax", "return {{{\n");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().code == ErrorCode::ConfigLoadFailed);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// camera_config
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("camera_config：空表回落默认相机（fov=45°弧度、trackball、keys=WASD 的 SDL3 ASCII 码）") {
	TempLua f("cc_empty", "return {}\n");
	auto r = loadCameraConfig(f.pathStr());
	REQUIRE(r.has_value());
	const Camera& c = r->camera;
	CHECK(c.mode == CameraMode::Trackball);
	expectVec(c.position, glm::vec3{0.0f, 0.0f, 3.0f});
	expectVec(c.target, glm::vec3{0.0f, 0.0f, 0.0f});
	// fov 内部恒为弧度：默认 45°（camera.cppm:49），loader 只在 fovDegrees 字段做度→弧度
	expectNear(c.fov, 45.0 * 3.14159265358979323846e0 / 180.0, 1e-6, 1e-6);
	expectNear(c.nearPlane, 0.1, 1e-5, 1e-6);
	expectNear(c.farPlane, 10.0, 1e-5, 1e-6);
	// SDL3 字母键码 = 小写 ASCII（**实测**：默认 forward=119='w'；diagnostics_impl.cppm:4
	// 与 camera_config_impl 注释写"= ASCII 大写"不确，但全链算术都以 SDLK_A 为基、自洽
	// 无功能问题——本断言钉实际值：W=119 S=115 A=97 D=100，键码若整体漂移即红）
	CHECK(c.keys.forward == 119u);
	CHECK(c.keys.back == 115u);
	CHECK(c.keys.left == 97u);
	CHECK(c.keys.right == 100u);
}

TEST_CASE("camera_config：fovDegrees 是度（60°→1.0472 rad）；near/far/模式/两模式参数逐字段生效") {
	TempLua f("cc_scalars", R"lua(
return { mode = "fps", fovDegrees = 60, near = 0.05, far = 250,
  trackball = { sensitivity = 0.01, zoom = 0.07 },
  fps = { speed = 9.5, sensitivity = 0.0021 } }
)lua");
	auto r = loadCameraConfig(f.pathStr());
	REQUIRE(r.has_value());
	const Camera& c = r->camera;
	CHECK(c.mode == CameraMode::Fps);
	// 度→弧度：60° = 1.0471976；若 loader 被改成"直接当弧度"，fov 会是 60，红
	expectNear(c.fov, 60.0 * 3.14159265358979323846e0 / 180.0, 1e-6, 1e-6);
	expectNear(c.nearPlane, 0.05, 1e-5, 1e-6);
	expectNear(c.farPlane, 250.0, 1e-5, 1e-6);
	expectNear(c.trackballSensitivity, 0.01, 1e-5, 1e-6);
	expectNear(c.zoomSpeed, 0.07, 1e-5, 1e-6);
	expectNear(c.fpsSpeed, 9.5, 1e-5, 1e-6);
	expectNear(c.fpsSensitivity, 0.0021, 1e-5, 1e-6);
}

TEST_CASE("camera_config：position/target 反推朝向——forward ≈ normalize(target−position)（与 cameraAimAt 同约定）") {
	// readCameraPlacement（camera_config_impl.cppm:117-142）与 cameraAimAt 是**同一
	// 反推约定**（文件头注释自述）。本组把该约定钉成可执行断言：加载后的相机经
	// cameraForward / cameraView 读回，必须真的看向 target；且携带 up 与 forward
	// 正交（world-up 对齐、无 roll）。
	TempLua f("cc_place", "return { position = { 0, 5, 5 }, target = { 0, 0, 0 }, mode = \"trackball\" }\n");
	auto r = loadCameraConfig(f.pathStr());
	REQUIRE(r.has_value());
	const Camera& c = r->camera;
	const glm::vec3 want = glm::normalize(glm::vec3{0.0f, -5.0f, -5.0f});
	for (int k = 0; k < 3; ++k)
		expectNear(cameraForward(c)[k], want[k], 1e-5, 1e-5);
	// pitch = asin(want.y) = −45°；yaw = atan2(−0, 0.7071) = 0
	expectNear(c.pitch, -45.0 * 3.14159265358979323846e0 / 180.0, 1e-5, 1e-5);
	expectNear(c.yaw, 0.0, 0.0, 1e-6);
	// up 正交（world-up 对齐）
	expectNear(glm::dot(cameraForward(c), c.up), 0.0, 0.0, 1e-5);
	// 终局判据：cameraView 把 target 投到视轴（0,0,−dist）
	const glm::vec4 q = cameraView(c) * glm::vec4(c.target, 1.0f);
	expectNear(q.x, 0.0, 0.0, 1e-3);
	expectNear(q.y, 0.0, 0.0, 1e-3);
	expectNear(q.z, -glm::length(c.target - c.position), 1e-4, 1e-3);
	// 反向对照：只有 position 没有 target 时回落默认 target(0,0,0)——仍看向原点
	TempLua g("cc_place2", "return { position = { 2, 2, 2 } }\n");
	auto r2 = loadCameraConfig(g.pathStr());
	REQUIRE(r2.has_value());
	const glm::vec3 want2 = glm::normalize(-r2->camera.position);
	for (int k = 0; k < 3; ++k)
		expectNear(cameraForward(r2->camera)[k], want2[k], 1e-5, 1e-5);
}

TEST_CASE("camera_config：keys 段可解析（认的名字生效、未写字段保默认）；未知键名报错不崩") {
	{  // forward 改 Up（SDL3 特殊键码 ≠ 字母基），其余三键保默认（SDL3 SDLK_A='a'，
	   // 见上一 TEST_CASE 注释：W=119 S=115 A=97 D=100）
		TempLua f("cc_keys", R"lua(return { keys = { forward = "Up" } })lua");
		auto r = loadCameraConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->camera.keys.forward != 119u);  // 真的换了键
		CHECK(r->camera.keys.back == 115u);
		CHECK(r->camera.keys.left == 97u);
		CHECK(r->camera.keys.right == 100u);
	}
	{  // 小写单字母 = 同大写（keyNameToSdlKey 先 toupper 再 SDLK_A+偏移）："q"→113、"1"→49
		TempLua f("cc_keys2", R"lua(return { keys = { forward = "q", back = "1" } })lua");
		auto r = loadCameraConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->camera.keys.forward == 113u);  // SDLK_A('a'=97) + 16
		CHECK(r->camera.keys.back == 49u);      // SDLK_0('0'=48) + 1
	}
	{  // 未知键名 → 报错（**实际行为**：不是"忽略该键"，是整体 ConfigLoadFailed）
		TempLua f("cc_keys_bad", R"lua(return { keys = { forward = "Bogus" } })lua");
		auto r = loadCameraConfig(f.pathStr());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().message.find("unknown key name") != std::string::npos);
	}
}

TEST_CASE("camera_config：mode 非法串报错；未知顶层键忽略不崩；缺文件回落默认") {
	{
		TempLua f("cc_mode", R"lua(return { mode = "drunk" })lua");
		auto r = loadCameraConfig(f.pathStr());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().message.find("unknown mode") != std::string::npos);
	}
	{  // 未知**顶层**键：loader 只按已知键取值，多余键无害（不崩、不报错），
	   // 同文件的已知键照常解析（阳性对照防"直接返回默认"的假绿）
		TempLua f("cc_bogus", "return { bogus = 42, also_bad = { 1, 2 }, fovDegrees = 30 }\n");
		auto r = loadCameraConfig(f.pathStr());
		REQUIRE(r.has_value());
		expectNear(r->camera.fov, 30.0 * 3.14159265358979323846e0 / 180.0, 1e-6, 1e-6);
	}
	{
		std::string missing = (std::filesystem::temp_directory_path() / "rgl_cfg_no_such_cam.lua").string();
		auto r = loadCameraConfig(missing);
		REQUIRE(r.has_value());  // 缺文件 = 默认 + warn（不报错）
		expectNear(r->camera.fov, 45.0 * 3.14159265358979323846e0 / 180.0, 1e-6, 1e-6);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// runtime_config
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("runtime_config：空表全默认（交互窗 800×600、不截图、presentMode=immediate、uiFontSize=18）") {
	TempLua f("rt_empty", "return {}\n");
	auto r = loadRuntimeConfig(f.pathStr());
	REQUIRE(r.has_value());
	CHECK(r->headless == false);
	CHECK(r->headlessWidth == 1280u);
	CHECK(r->headlessHeight == 720u);
	CHECK(r->winWidth == 800u);
	CHECK(r->winHeight == 600u);
	CHECK(r->showAfter == -1);
	CHECK(r->presentMode == PresentModePref::immediate);
	CHECK(r->llm.enabled == false);
	CHECK(r->llm.delayFrames == 0u);
	expectNear(r->uiFontSize, 18.0, 0.0, 0.0);
	// 缺文件同样回落（不报错）
	std::string missing = (std::filesystem::temp_directory_path() / "rgl_cfg_no_such_rt.lua").string();
	CHECK(loadRuntimeConfig(missing).has_value());
}

TEST_CASE("runtime_config：全字段解析（size 表 / show_after / fifo / llm 新旧格式 / ui_font_size 非正回落）") {
	{
		TempLua f("rt_full", R"lua(
return { headless = true, headless_size = { 1920, 1080 }, win_size = { 640, 480 },
  show_after = 120, present_mode = "fifo",
  llm_debug = true,
  llm_shot = { file = "out.png", size = { 800, 600 }, delay_frames = 7 },
  ui_font = "MiSans", ui_font_size = 22 }
)lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->headless == true);
		CHECK(r->headlessWidth == 1920u);
		CHECK(r->headlessHeight == 1080u);
		CHECK(r->winWidth == 640u);
		CHECK(r->winHeight == 480u);
		CHECK(r->showAfter == 120);
		CHECK(r->presentMode == PresentModePref::fifo);
		CHECK(r->llm.enabled == true);  // llm_debug 布尔 = 纯开关
		CHECK(r->llm.file == "out.png");
		CHECK(r->llm.width == 800u);
		CHECK(r->llm.height == 600u);
		CHECK(r->llm.delayFrames == 7u);
		CHECK(r->uiFont == "MiSans");
		expectNear(r->uiFontSize, 22.0, 0.0, 0.0);
	}
	{  // 向后兼容：llm_debug = table（旧格式）= 启用 + 细字段
		TempLua f("rt_llmold",
			R"lua(return { llm_debug = { file = "x.png", size = { 320, 240 }, delay_frames = 3 } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->llm.enabled == true);
		CHECK(r->llm.file == "x.png");
		CHECK(r->llm.width == 320u);
		CHECK(r->llm.delayFrames == 3u);
	}
	{  // **实际行为**（readUiFont）：ui_font_size ≤ 0 = 笔误，回落 18（不 fail-fast）
		TempLua f("rt_fontneg", "return { ui_font_size = -3 }\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		expectNear(r->uiFontSize, 18.0, 0.0, 0.0);
	}
	{  // present_mode 未知值 = fail-fast（配置笔误不静默回落，实现注释原文同级对待）
		TempLua f("rt_pm_bad", R"lua(return { present_mode = "triple_buffer" })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().message.find("present_mode") != std::string::npos);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// diagnostics
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("diagnostics：空表 = 全默认（影子开、其余关；texelCanary=MAX、dumpGbuf=−1）") {
	TempLua f("dg_empty", "return {}\n");
	auto r = loadDiagnosticsConfig(f.pathStr());
	REQUIRE(r.has_value());
	CHECK(r->texelCanary == ~uint64_t{0});
	CHECK(r->dumpGbuf == -1);
	CHECK(r->frameStats == 0u);
	CHECK(r->profiler == 0u);
	CHECK(r->textureForceSize == 0u);
	CHECK(r->rpcPort == 8788u);
	CHECK(r->rpc == false);
	CHECK(r->imgui == false);
	CHECK(r->shadowPass == true);  // 5a-9 起默认开（diagnostics.cppm:35）
	// 缺文件同样全默认 + warn（不报错）
	std::string missing = (std::filesystem::temp_directory_path() / "rgl_cfg_no_such_diag.lua").string();
	auto m = loadDiagnosticsConfig(missing);
	REQUIRE(m.has_value());
	CHECK(m->texelCanary == ~uint64_t{0});
	CHECK(m->shadowPass == true);
}

TEST_CASE("diagnostics：dump_gbuf 越界闸门按 kGBufferChannelCount 判界（越界=warn+关闭，界内保留）") {
	// 闸门实现（diagnostics_impl.cppm:172-178）：> kMaxGbufChannel 夹到 −1。
	// 表长契约 = 5（本测试先钉表长，lua 里的 4/5 两个数才有意义）。
	CHECK(kGBufferChannelCount == 5);
	{  // 界内（0 与 count−1=4）原值保留——闸门不是"全夹"
		TempLua f("dg_gbuf3", "return { dump_gbuf = 3 }\n");
		auto r = loadDiagnosticsConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->dumpGbuf == 3);
		TempLua g("dg_gbuf4", "return { dump_gbuf = 4 }\n");
		auto r4 = loadDiagnosticsConfig(g.pathStr());
		REQUIRE(r4.has_value());
		CHECK(r4->dumpGbuf == kGBufferChannelCount - 1);  // 边界值恰好放行
	}
	{  // 越界：= count（off-by-one 哨兵）与 99 都夹到 −1（关）
		TempLua f("dg_gbuf5", "return { dump_gbuf = 5 }\n");
		auto r = loadDiagnosticsConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->dumpGbuf == -1);
		TempLua g("dg_gbuf99", "return { dump_gbuf = 99 }\n");
		auto r99 = loadDiagnosticsConfig(g.pathStr());
		REQUIRE(r99.has_value());
		CHECK(r99->dumpGbuf == -1);
	}
	{  // **实际行为**：负数原样保留（−7 → −7，loader 不规范化成 −1）。
	   // 消费端以 "< 0 = 关"判断，−7 与 −1 等价；若哪天 loader 改成 clamp 到 −1，
	   // 本断言变红——届时去确认消费端语义后同步改，而不是无脑改期望。
		TempLua f("dg_gbufneg", "return { dump_gbuf = -7 }\n");
		auto r = loadDiagnosticsConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->dumpGbuf == -7);
	}
}

TEST_CASE("diagnostics：数值门负数=关哨兵（texel_canary=−1→MAX、frame_stats=−5→0）；正数原样") {
	{  // texel_canary：−1 → ~0（MAX=关）；0 是合法帧号（首帧）；正数原样
		TempLua f("dg_texel", "return { texel_canary = -1, frame_stats = -5, profiler = -2, texture_force_size = -1 }\n");
		auto r = loadDiagnosticsConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->texelCanary == ~uint64_t{0});
		CHECK(r->frameStats == 0u);   // max(0, −5)
		CHECK(r->profiler == 0u);     // max(0, −2)
		CHECK(r->textureForceSize == 0u);  // max(0, −1)
	}
	{  // 阳性对照：正数值原样进字段（负数夹取分支不是恒取 0）
		TempLua f("dg_pos", "return { texel_canary = 0, frame_stats = 30, profiler = 120, texture_force_size = 64 }\n");
		auto r = loadDiagnosticsConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->texelCanary == 0u);  // 0 = 合法帧号，不是"关"
		CHECK(r->frameStats == 30u);
		CHECK(r->profiler == 120u);
		CHECK(r->textureForceSize == 64u);
	}
}

TEST_CASE("diagnostics：rpc_port 越界回落 8788（0/70000），合法值原样；bool 门双向可写（解析是活的）") {
	{
		TempLua f("dg_port0", "return { rpc_port = 0 }\n");
		CHECK(loadDiagnosticsConfig(f.pathStr())->rpcPort == 8788u);
		TempLua g("dg_port_hi", "return { rpc_port = 70000 }\n");
		CHECK(loadDiagnosticsConfig(g.pathStr())->rpcPort == 8788u);
		TempLua h("dg_port_ok", "return { rpc_port = 9999 }\n");
		CHECK(loadDiagnosticsConfig(h.pathStr())->rpcPort == 9999u);
	}
	{  // bool 门两侧：shadow_pass（默认 true）可写 false、imgui（默认 false）可写 true。
	   // 若 bool 解析坏掉恒读默认，这两个断言必红——纯默认值测试永远绿不了坏读取器。
		TempLua f("dg_bools", "return { shadow_pass = false, imgui = true, rpc = true }\n");
		auto r = loadDiagnosticsConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->shadowPass == false);
		CHECK(r->imgui == true);
		CHECK(r->rpc == true);
	}
	{  // camera_probe 子表：数值字段 + 单字母键（SDL3 SDLK_A='a' 基，见 camera_config 组
	   // 注释："d"→SDLK_A+3=100）；非单字母 warn+忽略
		TempLua f("dg_probe", "return { camera_probe = { frame = 12, holdFrames = 3, key = \"d\" } }\n");
		auto r = loadDiagnosticsConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->cameraProbeFrame == 12u);
		CHECK(r->cameraProbeHold == 3u);
		CHECK(r->cameraProbeKey == 100u);  // SDLK_A(97) + 3
		TempLua g("dg_probe_bad", "return { camera_probe = { frame = 5, key = \"ab\" } }\n");
		auto r2 = loadDiagnosticsConfig(g.pathStr());
		REQUIRE(r2.has_value());  // warn 不报错
		CHECK(r2->cameraProbeKey == 0u);   // 非法键被忽略
		CHECK(r2->cameraProbeHold == 8u);  // **实际行为**：缺省回落 8
	}
}

TEST_CASE("diagnostics：dgc_static 空表回落 = false（默认关）+ 显式 true 被解析（批 12.3d · B2）") {
	// 契约（diagnostics.cppm:28）：RADV 静态 shader DGC spike 门**默认关**（实测性能亏
	// 1.85×，docs/archive/radv_static_dgc_spike.md）。解析实文（diagnostics_impl.cppm:141）：
	// cfg.dgcStatic = readBool("dgc_static", cfg.dgcStatic)——缺失回落 = 结构体默认值。
	{  // 空表 → false（变异红线：结构体默认翻成 true → 红）
		TempLua f("dg_dgc_def", "return {}\n");
		auto r = loadDiagnosticsConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->dgcStatic == false);
	}
	{  // 显式 true 生效（变异红线：bool 解析恒读默认 / key "dgc_static" 漂移 → 红）
		TempLua f("dg_dgc_on", "return { dgc_static = true }\n");
		auto r = loadDiagnosticsConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->dgcStatic == true);
	}
}

TEST_CASE("diagnostics：setDiagnostics → diagnostics() 单例往返（main 启动期唯一的填充路径）") {
	DiagnosticsConfig c;
	c.frameStats = 77;
	c.dumpGbuf = 2;
	c.shadowPass = false;
	setDiagnostics(c);
	CHECK(diagnostics().frameStats == 77u);
	CHECK(diagnostics().dumpGbuf == 2);
	CHECK(diagnostics().shadowPass == false);
	// 还原全默认，避免污染同进程其它用例（单例 = 进程级状态，谁改谁负责收尾）
	setDiagnostics(DiagnosticsConfig{});
	CHECK(diagnostics().frameStats == 0u);
}

}  // namespace RGL
