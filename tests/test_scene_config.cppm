module;
#include <doctest/doctest.h>
#include <algorithm>   // std::ranges::count（回显全量条目计数；先例 test_evssm_math.cppm）
#include <glm/glm.hpp>
#include <filesystem>  // 临时 lua 落盘（AGENTS.md GMF 例外清单成员）

// 五份配置 loader 的解析契约（批 12.1 · B1；批 N6a 加 shadow_config）——被测对象 =
// scene_config / camera_config / runtime_config / diagnostics / shadow_config
// （rlcore/rlbase 成员，sol2 沙箱 + 缺省回落）。
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
import shadow_config;
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

TEST_CASE("scene_config：taa 段解析（批 12.6；缺段回落调参默认 + 显式键逐项命中）") {
	// 缺段 = 全默认（调参画像）。批 14.6：`enabled` 已删（抗锯齿选路搬进 runtime.lua 的
	// `aa` 三态键，见本文件 runtime_config 段）——本用例从此只管**调参**。
	// 变异红线：taaFeedback 默认被翻成 0.12 → 首条断言红；默认值 = 交付画像的一部分，
	// 漂移不会被任何判据抓到（判据都在显式写键的 profile 上跑），只有这条覆盖「不写键」。
	{
		TempLua f("sc_taa_absent", "return {}\n");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		expectNear(r->taaFeedback, 0.01, 0.0, 1e-6);  // 本帧权重（≈100 帧等效累积窗）
		expectNear(r->taaJitterScale, 1.0, 0.0, 1e-6);
		CHECK(r->taaMotionCompensation == true);
		// 批 13.1f：base_space 默认**开**（游动 0.643 → 0.060px 实测后拍板）。变异红线：
		// 默认被翻成 false → 本条红。
		CHECK(r->taaBaseSpace == true);
	}
	// 显式命中（snake_case 键名 = 契约）。变异红线：任一 key 打错（如 jitterScale）→
	// 对应字段保持默认 → 本条红——与上面 exposure 正向对照同一手法。
	// ⚠ base_space 这里**显式写 false**：默认是 true，只写「显式 true 命中」测不出
	//   「键名拼错时静默回落默认」——键名打错与不写键在只测 true 的写法下**不可区分**。
	{
		TempLua f("sc_taa_on",
			"return { taa = { feedback = 0.3, jitter_scale = 0.5, "
			"motion_compensation = false, base_space = false } }\n");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		expectNear(r->taaFeedback, 0.3, 1e-4, 0.0);
		expectNear(r->taaJitterScale, 0.5, 0.0, 1e-6);
		CHECK(r->taaMotionCompensation == false);
		CHECK(r->taaBaseSpace == false);  // 与默认相反方向的命中（见上）
	}
	// 反方向命中：默认 true 时「显式 true」是恒真的，所以这条的价值不在断言本身，而在
	// 它与上一条**合起来**把 {默认, 显式真, 显式假} 三个取值钉死——任一处写反必有一条红。
	{
		TempLua f("sc_taa_bs_on", "return { taa = { base_space = true } }\n");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->taaBaseSpace == true);
	}
}

TEST_CASE("scene_config：帧驱动 spin 键（spin_frame_angle / spin_axis；默认 = 门关 = 逐位今天）") {
	// 默认画像（门关）：spin_frame_angle = 0 且 spin_axis = (0.5,1,0) = SceneSpin.axis 现值。
	// 变异红线：默认被翻成非 0 → 本条红（默认漂移 = 门默认开 = 基线不再是基线——
	// 与 taaBaseSpace 的「不写键」覆盖逻辑同族，只有这条断言覆盖缺省路径）。
	{
		TempLua f("sc_spinframe_absent", "return {}\n");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		expectNear(r->spinFrameAngle, 0.0, 0.0, 1e-9);
		expectVec(r->spinAxis, glm::vec3(0.5f, 1.0f, 0.0f));
	}
	// 显式命中（snake_case 键名 = 契约；轴写 up = imgqa temporal-motion 的激励形态，
	// 与默认 (0.5,1,0) 方向相反 ⟹ 键名打错与不写键**可区分**，同 base_space 的手法）。
	// 变异红线：readFloat/readVec3 任一键名打错 → 字段保持默认 → 本条红。
	{
		TempLua f("sc_spinframe_on",
			"return { spin_frame_angle = 0.00349066, spin_axis = { 0, 1, 0 } }\n");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		expectNear(r->spinFrameAngle, 0.00349066, 1e-6, 0.0);
		expectVec(r->spinAxis, glm::vec3(0.0f, 1.0f, 0.0f));
	}
}

TEST_CASE("scene_config：taa.reprojection / taa.diag_reject（默认 = 逐位今天；显式命中）") {
	// 默认画像：reprojection = true（消融门默认不生效）、diag_reject = false（诊断门默认关）。
	// 变异红线：默认被翻 ⟹ 本条红（门默认开 = 基线不再是基线 = 交付图被诊断码污染）。
	{
		TempLua f("sc_taa_repro_absent", "return {}\n");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->taaReprojection == true);
		CHECK(r->taaDiagReject == false);
	}
	// 显式命中（snake_case 键名 = 契约；两键都写**非默认方向** ⟹ 键名打错与不写键可区分）。
	// 变异红线：readBool 任一键名打错 → 字段保持默认 → 本条红。
	{
		TempLua f("sc_taa_repro_on",
			"return { taa = { reprojection = false, diag_reject = true } }\n");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->taaReprojection == false);
		CHECK(r->taaDiagReject == true);
	}
	// 反向（显式写默认值也必须命中 true/false 本身，不是「缺省回落」蒙对）
	{
		TempLua f("sc_taa_repro_off",
			"return { taa = { reprojection = true, diag_reject = false } }\n");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->taaReprojection == true);
		CHECK(r->taaDiagReject == false);
	}
}

TEST_CASE("scene_config：taa.clip_mode / taa.clip_soft（批 13.11 消融门；默认 = 逐位今天）") {
	// 默认画像：clip_mode = 0（现行硬裁剪 = 交付行为）、clip_soft = 1.0（λ 只在 mode=4 被读，
	// 默认值本身不参与像素）。变异红线：默认被翻成非 0 ⟹ 本条红 = 门默认开 = 基线不再是基线。
	{
		TempLua f("sc_taa_clip_absent", "return {}\n");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->taaClipMode == 0.0f);
		expectNear(r->taaClipSoft, 1.0f, 0.0, 1e-6);
	}
	// 显式命中（两键都写**非默认方向** ⟹ 键名打错与不写键可区分）。
	// 变异红线：readFloat 的键名打错（clip_mode ↔ clip_soft 打串是本仓加相邻键时的真实风险）
	// ⟹ 字段保持默认 ⟹ 本条红。
	{
		TempLua f("sc_taa_clip_on", "return { taa = { clip_mode = 4, clip_soft = 0.1 } }\n");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->taaClipMode == 4.0f);
		expectNear(r->taaClipSoft, 0.1f, 1e-6, 0.0f);
	}
	// **两键互不越界**（反向用例的正题）：只写一个键，另一个必须**纹丝不动**停在默认。
	// 变异红线：把第二行 readFloat 的目标字段写成第一个（cfg.taaClipMode）→ 只写 clip_soft
	// 那条会把它灌进 clip_mode → 本条红。
	{
		TempLua f("sc_taa_clip_only_mode", "return { taa = { clip_mode = 2 } }\n");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->taaClipMode == 2.0f);
		expectNear(r->taaClipSoft, 1.0f, 0.0, 1e-6);
	}
	{
		TempLua f("sc_taa_clip_only_soft", "return { taa = { clip_soft = 0.25 } }\n");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->taaClipMode == 0.0f);
		expectNear(r->taaClipSoft, 0.25f, 1e-6, 0.0f);
	}
}

TEST_CASE("camera_config：frame_yaw 帧驱动 yaw 门（默认 0 = 门关；显式命中且不碰初始朝向反推）") {
	// 默认 = 0（门关 = 逐位今天）。变异红线：默认被翻成非 0 = 门默认开 = 交付相机自己转。
	{
		TempLua f("cc_frameyaw_absent", "return {}\n");
		auto r = loadCameraConfig(f.pathStr());
		REQUIRE(r.has_value());
		expectNear(r->camera.frameYaw, 0.0, 0.0, 1e-9);
	}
	// 显式命中 + **不干扰** position/target 反推（frame_yaw 只填自己的字段；yaw 仍由机位
	// 反推，门推进发生在 cameraSystem，不在 loader）。反向对照：yaw 若被 frame_yaw 覆写，
	// 本条的 yaw 断言红。
	{
		TempLua f("cc_frameyaw_on",
			"return { mode = \"fps\", position = { 0, 0, 5 }, target = { 0, 0, 0 }, "
			"frame_yaw = 0.00349066 }\n");
		auto r = loadCameraConfig(f.pathStr());
		REQUIRE(r.has_value());
		expectNear(r->camera.frameYaw, 0.00349066, 1e-6, 0.0);
		// fps + position/target 反推：看向 −z ⟹ yaw = atan2(0, 5) = 0、pitch = 0
		expectNear(r->camera.yaw, 0.0, 1e-6, 0.0);
		expectNear(r->camera.pitch, 0.0, 1e-6, 0.0);
	}
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

TEST_CASE("runtime_config：aa 三态（批 14.6；缺键 = off、三值命中、未知值 fail-fast）") {
	// 出厂画像：aa = off 且两个谓词都关。变异红线：默认被翻成 taa → 本条红（默认漂移 =
	// 出厂就开一条时域滤波，而所有红线基线都是在 off 上拍的，没有任何判据会因此变红）。
	{
		TempLua f("rt_aa_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->aa == AaMode::off);
		CHECK(r->aaWantsTaa() == false);
		CHECK(r->aaWantsFsr() == false);
		CHECK(r->velocityField == false);  // 关态绝不凭空冒出 velocity pass
	}
	// 三值逐个命中。变异红线：off/taa 分支写反（taa 落进 off 支）→ 对应 CHECK 红；
	// 谓词与枚举比错成员（aaWantsFsr 比成 AaMode::taa）→ 同一条里的两个 CHECK 互相矛盾 ⟹ 红。
	{
		TempLua f("rt_aa_off", R"lua(return { aa = "off" })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->aa == AaMode::off);
		CHECK(r->aaWantsTaa() == false);
		CHECK(r->aaWantsFsr() == false);
	}
	{  // taa 态：只推导 TAA，**不**碰 velocity（原 scene.lua taa.enabled 的消费者改走 aa）
		TempLua f("rt_aa_taa", R"lua(return { aa = "taa" })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->aa == AaMode::taa);
		CHECK(r->aaWantsTaa() == true);
		CHECK(r->aaWantsFsr() == false);
		CHECK(r->velocityField == false);
	}
	// fsr 态**隐含** velocity_field（FSR 的 motionVectors 输入就是那张图）。
	// 变异红线：删掉 applyAaKey 里 `cfg.velocityField = true` 这一行 → 本条红，而失效形态是
	// 「FSR 静默退化成双线性放大」（vendor 拿不到速度场就每帧 reset），判据不会替引擎喊。
	{
		TempLua f("rt_aa_fsr", R"lua(return { aa = "fsr" })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->aa == AaMode::fsr);
		CHECK(r->aaWantsFsr() == true);
		CHECK(r->aaWantsTaa() == false);
		CHECK(r->velocityField == true);  // 隐含打开
	}
	// 隐含关系是**单向且强制**的：显式 velocity_field = false 也压不过 aa = fsr（解析顺序 =
	// 先读 velocity_field 再 applyAaKey，见 runtime_config_impl）。变异红线：把两行解析调个
	// 顺序（applyAaKey 先跑）→ 本条红，且红的是「谁赢」这条契约而不是某个默认值。
	{
		TempLua f("rt_aa_fsr_vel_off", R"lua(return { aa = "fsr", velocity_field = false })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->velocityField == true);
	}
	// 反向独立性：velocity_field 有自己的非 FSR 消费者（批 14.4 逐像素重投影线），所以
	// off/taa 态下它必须**照配置走**、不被 aa 清掉。变异红线：在 applyAaKey 的 off/taa 支
	// 里补一句 `cfg.velocityField = false`「归一化」→ 本条红（那会把重投影线的开关吃掉）。
	{
		TempLua f("rt_aa_off_vel_on", R"lua(return { aa = "off", velocity_field = true })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->aaWantsFsr() == false);
		CHECK(r->velocityField == true);
	}
	{  // 未知值 = fail-fast（present_mode 同纪律：静默回落 = 你以为开着抗锯齿在量图）
		TempLua f("rt_aa_bad", R"lua(return { aa = "msaa" })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().message.find("aa") != std::string::npos);
	}
}

TEST_CASE("runtime_config：fsr 嵌套块（缺块 = true/true 设计点；显式命中；无扁平键双读）") {
	// 缺块默认 = true/true = FSR 设计点。变异红线：默认翻成 false/false → 本条红（出厂态
	// 静默从「vendor 累积历史 + 抖动」换成「纯空间放大」= 换了一档，而配置里一个字都没改）。
	{
		TempLua f("rt_fsr_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->fsrTemporal == true);
		CHECK(r->fsrJitter == true);
	}
	{  // 块内键名命中（temporal / jitter）。变异红线：键名打错 → 回落默认 → 本条红。
		TempLua f("rt_fsr_off", R"lua(return { fsr = { temporal = false, jitter = false } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->fsrTemporal == false);
		CHECK(r->fsrJitter == false);
	}
	{  // 只写一枚 = 另一枚保持默认（块内逐键可选，不是整块覆盖）
		TempLua f("rt_fsr_half", R"lua(return { fsr = { temporal = false } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->fsrTemporal == false);
		CHECK(r->fsrJitter == true);
	}
	{  // **没有**扁平键的兼容双读（两个真相源 = 本项目反复登记的反模式）：顶层写
	  // fsr_temporal 必须**什么都不发生**，否则旧键与新块并存 = 静默用哪个说不清。
		TempLua f("rt_fsr_flat", R"lua(return { fsr_temporal = false, fsr_jitter = false })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->fsrTemporal == true);
		CHECK(r->fsrJitter == true);
	}
}

TEST_CASE("runtime_config：ssr 块（批 15.1 参数面）——缺块回落出厂系数、逐键命中、消融档不钳制") {
	// 判据来源 = 实读 runtime_config_impl::readSsrBlock / readFloatKey（缺键 = 不写目标字段）。
	// 本用例是 SSR 参数链的**上游一半**；下游一半（tuning → scene data 字节）= test_ssr_params。
	// 两半合起来才是「lua 文本 → 字节」整链——分开测是因为中间隔着 registry。
	//
	// 变异红线声明：
	//   ① readSsrBlock 漏读某枚键 → 该键的"显式命中"红（默认值 ≠ 本用例写的探针值）。
	//   ② 键名打错（lua 侧或 C++ 侧）→ 同上红（回落默认，探针值读不到）。
	//   ③ 配置层加了 clamp / 不变式校验 → 「消融档可写出来」红。**这条是刻意的反向用例**：
	//      批 15.1 参数面拍板配置层不校验（理由 → readSsrBlock 注释），将来谁想补校验必须先推翻
	//      那段理由并同步改本用例，不能悄悄加。
	//   ④ 出厂默认值被改 → 「缺块回落」红（断言的是字面量，不是 SsrTuning{} 的字段）。
	{  // 缺块 = 出厂系数（红线 A 的配置层前提：出厂 runtime.lua 的 ssr 块只写 enabled）
		TempLua f("rt_ssr_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->ssrEnabled == false);
		// ⚠ 字面量手抄（与 test_ssr_params 的默认值用例同源）：thicknessPx = 15.3C 重定值 12.0，
		// 其余四枚 = 批 15.1 shader 常量原值。写成 SsrTuning{} 的字段就成了"默认值等于默认值"的空断言。
		CHECK(r->ssr.maxDistanceRadii == 0.52f);
		CHECK(r->ssr.thicknessPx == 12.0f);
		CHECK(r->ssr.biasPx == 0.6f);
		CHECK(r->ssr.roughFull == 0.15f);
		CHECK(r->ssr.roughCutoff == 0.40f);
		CHECK(r->ssr.blurScale == 8.0f);  // 批 15.3B 第六枚（出厂 = 螺旋半径系数标定值）
	}
	{  // 块内六枚键名逐个命中（探针值互不相同、且都不等于任何默认值）
		TempLua f("rt_ssr_full", R"lua(return { ssr = {
			enabled = true,
			max_distance_radii = 1.25, thickness_px = 2.5, bias_px = 0.75,
			rough_full = 0.05, rough_cutoff = 0.9, blur_scale = 12.5,
		} })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->ssrEnabled == true);  // 创建期量与系数同块不同键，互不干扰
		CHECK(r->ssr.maxDistanceRadii == 1.25f);
		CHECK(r->ssr.thicknessPx == 2.5f);
		CHECK(r->ssr.biasPx == 0.75f);
		CHECK(r->ssr.roughFull == 0.05f);
		CHECK(r->ssr.roughCutoff == 0.9f);
		CHECK(r->ssr.blurScale == 12.5f);
	}
	{  // 只写一枚 = 其余保默认（块内逐键可选，不是整块覆盖；先例 = fsr 块同款）
		TempLua f("rt_ssr_half", R"lua(return { ssr = { thickness_px = 3.0 } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->ssr.thicknessPx == 3.0f);
		CHECK(r->ssr.maxDistanceRadii == 0.52f);
		CHECK(r->ssr.biasPx == 0.6f);
	}
	{  // 🔴 **反向消融档必须写得出来**（变异③）：bias ≥ thickness 与 thickness = 0 都是
	   // 合法档（闭式期望 = 凸包内覆盖率恰为 0）。配置层若钳制/拒绝，消融就静默变成另一档。
		TempLua f("rt_ssr_ablate", R"lua(return { ssr = {
			bias_px = 8.0, thickness_px = 6.0,
		} })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->ssr.biasPx == 8.0f);        // 违反不变式：原样透传，不 warn 不 clamp
		CHECK(r->ssr.thicknessPx == 6.0f);
		TempLua g("rt_ssr_zero", R"lua(return { ssr = { thickness_px = 0.0 } })lua");
		auto r2 = loadRuntimeConfig(g.pathStr());
		REQUIRE(r2.has_value());
		CHECK(r2->ssr.thicknessPx == 0.0f);  // 0 不是"缺键"哨兵，写 0 就得是 0
		// 批 15.3B：blur_scale = 0 同为合法消融档（纯双线性上采样），同一条不钳制纪律。
		TempLua h("rt_ssr_blur0", R"lua(return { ssr = { blur_scale = 0.0 } })lua");
		auto r3 = loadRuntimeConfig(h.pathStr());
		REQUIRE(r3.has_value());
		CHECK(r3->ssr.blurScale == 0.0f);
	}
	{  // **没有**扁平键的兼容双读（两个真相源 = 本项目反复登记的反模式）：顶层写
	   // ssr_thickness_px 必须**什么都不发生**，否则旧键与新块并存 = 静默用哪个说不清。
		TempLua f("rt_ssr_flat", R"lua(return { ssr_thickness_px = 9.0, ssr_bias_px = 9.0 })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->ssr.thicknessPx == 12.0f);  // 出厂默认（15.3C 重定值）
		CHECK(r->ssr.biasPx == 0.6f);
	}
}

TEST_CASE("runtime_config：wet 块（批 15.2A 湿面参数）——缺块回落默认、逐键命中、消融档可写出、无扁平键双读") {
	// 判据来源 = 实读 runtime_config_impl::readWetBlock / readFloatKey（缺键 = 不写目标字段）。
	// 本用例是湿面参数链的**上游一半**；下游一半（tuning → scene data 字节）= test_wet_params。
	// 两半合起来才是「lua 文本 → 字节」整链——分开测是因为中间隔着 registry。
	//
	// 变异红线声明：
	//   ① readWetBlock 漏读某枚键 → 该键的"显式命中"红（默认值 ≠ 本用例写的探针值）。
	//   ② 键名打错（lua 侧或 C++ 侧）→ 同上红（回落默认，探针值读不到）。
	//   ③ 配置层加了 clamp / 不变式校验 → 「消融档 strength=0 可写出」红。**这条是刻意的
	//      反向用例**：批 15.2A 拍板配置层不校验（理由 → readWetBlock 注释，与 readSsrBlock
	//      同判），将来谁想补校验必须先推翻那段理由并同步改本用例，不能悄悄加。
	//   ④ 出厂默认值被改 → 「缺块回落」红（断言的是字面量，不是 WetTuning{} 的字段）。
	{  // 缺块 = 出厂系数（红线 A 的配置层前提；strength 默认 1.0 = 「湿面默认开」拍板的钉，
	   // 与 test_wet_params 的默认值用例同源——字面量手抄，写成 WetTuning{} 就是空断言）。
		TempLua f("rt_wet_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->wet.strength == 1.0f);
		CHECK(r->wet.dark == 0.6f);
		CHECK(r->wet.roughLo == 0.05f);
		CHECK(r->wet.roughHi == 0.35f);
		CHECK(r->wet.rippleAmp == 0.3f);
		CHECK(r->wet.puddleFreqRadii == 12.0f);
	}
	{  // 块内六枚键名逐个命中（探针值互不相同、且都不等于任何默认值）
		TempLua f("rt_wet_full", R"lua(return { wet = {
			strength = 0.5, dark = 0.75, rough_lo = 0.01, rough_hi = 0.5,
			ripple_amp = 0.25, puddle_freq_radii = 8.0,
		} })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->wet.strength == 0.5f);
		CHECK(r->wet.dark == 0.75f);
		CHECK(r->wet.roughLo == 0.01f);
		CHECK(r->wet.roughHi == 0.5f);
		CHECK(r->wet.rippleAmp == 0.25f);
		CHECK(r->wet.puddleFreqRadii == 8.0f);
	}
	{  // 只写一枚 = 其余保默认（块内逐键可选，不是整块覆盖；先例 = fsr/ssr 块同款）
		TempLua f("rt_wet_half", R"lua(return { wet = { dark = 0.5 } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->wet.dark == 0.5f);
		CHECK(r->wet.strength == 1.0f);
		CHECK(r->wet.roughHi == 0.35f);
	}
	{  // 🔴 **消融档必须写得出来**（变异③）：strength = 0 是合法档（湿面关 = 因果泄漏恒等式
	   // 的门关档）。配置层若钳制/拒绝，消融就静默变成另一档。
		TempLua f("rt_wet_ablate", R"lua(return { wet = { strength = 0.0 } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->wet.strength == 0.0f);  // 0 不是"缺键"哨兵，写 0 就得是 0
		CHECK(r->wet.dark == 0.6f);      // 消融只关总开关，其余系数保默认（无联动）
	}
	{  // **没有**扁平键的兼容双读（两个真相源 = 本项目反复登记的反模式）：顶层写
	   // wet_strength 必须**什么都不发生**，否则旧键与新块并存 = 静默用哪个说不清。
		TempLua f("rt_wet_flat", R"lua(return { wet_strength = 9.0, wet_dark = 9.0 })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->wet.strength == 1.0f);
		CHECK(r->wet.dark == 0.6f);
	}
}

TEST_CASE("runtime_config：meshlet 块（★0-b1 簇路径总门）——缺块 = false、显式命中、无扁平键双读") {
	// 判据来源 = 实读 runtime_config_impl::readMeshletBlock / readBoolKey（缺键 = 不写目标字段）。
	// 默认 false = P5 拍板「门关 = 逐位不变是硬护栏」的配置层前提；本批无消费者（★0-b1 只落
	// 配置项与读取侧模块，接线 = ★0-b2/★1）。
	// 变异红线声明：
	//   ① 默认值翻成 true → 「缺块回落」与「RuntimeConfig{} 成员初始化」两条断言红。
	//   ② readMeshletBlock 漏读 enable / 键名打错 → 「显式命中」红（探针值 true 读不到）。
	//   ③ 加了扁平键双读兼容 → 「顶层 meshlet_enable 什么都不发生」红（两个真相源反模式）。
	{  // 成员初始化默认路径（不看 lua 文件的默认值 = 主 agent 验收口径）
		CHECK(RuntimeConfig{}.meshletEnable == false);
	}
	{  // 缺块 = 门关（出厂态 = 逐位护栏）
		TempLua f("rt_meshlet_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletEnable == false);
	}
	{  // 块内键名命中（探针值 true ≠ 默认 false，漏读即红）
		TempLua f("rt_meshlet_on", R"lua(return { meshlet = { enable = true } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletEnable == true);
	}
	{  // **没有**扁平键的兼容双读（两个真相源 = 本项目反复登记的反模式）
		TempLua f("rt_meshlet_flat", R"lua(return { meshlet_enable = true })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletEnable == false);
	}
}

TEST_CASE("runtime_config：meshlet.switch（★1-a1ii-β-1 原位取代）——默认 false、显式命中、非布尔即失败") {
	// 判据来源 = 实读 readMeshletBlock / readStrictBoolKey。默认 false = 「α 形态（旧路径 +
	// 影子叠加）是出厂态」的配置层前提；非布尔 = 启动失败是 β-1 新增的严格面。
	// 变异红线声明：
	//   ① 默认值翻成 true → 「成员初始化」与「缺块」两条断言红（出厂即取代 = 逐位护栏失效）。
	//   ② readMeshletBlock 漏读 switch / 键名打错 → 「显式命中」红（探针值 true 读不到）。
	//   ③ 退回宽容的 readBoolKey（类型错静默当缺键）→ 「非布尔即失败」两条红。
	//      🔴 这条正是本批实测到的真实失效：`switch = "yes"` 与 `switch = 1` 在宽容读法下
	//      跑完整程 errors=0 warnings=0，而实际跑的是旧路径 = 截图哈希判据整批假绿。
	{  // 成员初始化默认路径（不看 lua 文件的默认值）
		CHECK(RuntimeConfig{}.meshletSwitch == false);
	}
	{  // 缺块 / 缺键 = 不取代（= α 形态 = 出厂态）
		TempLua f("rt_mswitch_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletSwitch == false);
		TempLua g("rt_mswitch_nokey", R"lua(return { meshlet = { enable = true } })lua");
		auto r2 = loadRuntimeConfig(g.pathStr());
		REQUIRE(r2.has_value());
		CHECK(r2->meshletSwitch == false);
	}
	{  // 块内键名命中（探针值 true ≠ 默认 false，漏读即红）
		TempLua f("rt_mswitch_on", R"lua(return { meshlet = { enable = true, switch = true } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletSwitch == true);
	}
	{  // 非布尔值 = **启动失败**，不静默回落（实测旧行为：字符串/数字都当键缺失跑完）
		for (const char* bad : {R"lua(return { meshlet = { switch = "yes" } })lua",
		                        R"lua(return { meshlet = { switch = 1 } })lua"}) {
			TempLua f("rt_mswitch_bad", bad);
			auto r = loadRuntimeConfig(f.pathStr());
			CHECK_FALSE(r.has_value());
		}
	}
	{  // 同块其余三枚布尔同享严格面（本批把整块换掉，不是只换 switch）
		TempLua f("rt_mswitch_strict_siblings", R"lua(return { meshlet = { enable = "on" } })lua");
		CHECK_FALSE(loadRuntimeConfig(f.pathStr()).has_value());
		TempLua g("rt_mswitch_strict_debug", R"lua(return { meshlet = { debug = 0 } })lua");
		CHECK_FALSE(loadRuntimeConfig(g.pathStr()).has_value());
	}
}

TEST_CASE("runtime_config：meshlet.shadow_lod（★18-B1 阴影段选级）——默认 −1、三态取值、严格整数面") {
	// 判据来源 = 实读 readMeshletBlock / readStrictIntKey。默认 **−1 = 本档关** 是「门关逐位
	// 不变」的配置层前提；0 与 N≥1 是两条**语义不同**的合法档（0 = §18.6 判据 2 的阳性对照
	// L0 臂，N≥1 = uniform 选级），解析层必须原样透传、不得合并。
	// 变异红线声明：
	//   ① 默认值翻成 0 → 「成员初始化」「缺块」「缺键」三条红（出厂即开阳性对照臂 = 逐位护栏失效）。
	//   ② 解析层把 0 当「关」（写成 `if (v <= 0)` 而非 `< 0`）→ 「0 = L0 臂」红。
	//      🔴 这条最要紧：0 被吞掉 ⟹ §18.6 判据 2 根本跑不出来，而截图与门关逐位同 = 假绿。
	//   ③ 退回宽容的 readFloatKey/readBoolKey 式读法（类型错静默当缺键）→ 「非整数即失败」
	//      四条红（字符串/布尔/小数/越界各一条，小数与越界钉的是"截断"这条独立漂移）。
	//   ④ 加扁平键双读兼容 → 「顶层 meshlet_shadow_lod 什么都不发生」红（两个真相源反模式）。
	{  // 成员初始化默认路径（不看 lua 文件的默认值 = 主 agent 验收口径）
		CHECK(RuntimeConfig{}.meshletShadowLod == -1);
	}
	{  // 缺块 / 缺键 = 关（出厂态 = 逐位护栏）
		TempLua f("rt_mlod_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletShadowLod == -1);
		TempLua g("rt_mlod_nokey", R"lua(return { meshlet = { enable = true } })lua");
		auto r2 = loadRuntimeConfig(g.pathStr());
		REQUIRE(r2.has_value());
		CHECK(r2->meshletShadowLod == -1);
	}
	{  // 三态取值原样透传（探针值 5 / 0 / −3 互不相同且 ≠ 默认，漏读或错并即红）
		TempLua a("rt_mlod_five", R"lua(return { meshlet = { shadow_lod = 5 } })lua");
		auto r = loadRuntimeConfig(a.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletShadowLod == 5);
		TempLua b("rt_mlod_zero", R"lua(return { meshlet = { shadow_lod = 0 } })lua");
		auto r0 = loadRuntimeConfig(b.pathStr());
		REQUIRE(r0.has_value());
		CHECK(r0->meshletShadowLod == 0);  // 🔴 0 是合法档（阳性对照 L0 臂），不是"关"
		TempLua c("rt_mlod_neg3", R"lua(return { meshlet = { shadow_lod = -3 } })lua");
		auto r3 = loadRuntimeConfig(c.pathStr());
		REQUIRE(r3.has_value());
		CHECK(r3->meshletShadowLod == -3);  // 解析层不解释语义：只有 < 0 这一件事由消费方读
	}
	{  // 非整数 = **启动失败**并点名键（严格面，同 readStrictBoolKey 的实测纪律）
		for (const char* bad : {R"lua(return { meshlet = { shadow_lod = "5" } })lua",
		                        "return { meshlet = { shadow_lod = true } }\n",
		                        "return { meshlet = { shadow_lod = 5.5 } }\n",
		                        "return { meshlet = { shadow_lod = 3000000000 } }\n"}) {
			TempLua f("rt_mlod_bad", bad);
			auto r = loadRuntimeConfig(f.pathStr());
			CHECK_FALSE(r.has_value());
		}
	}
	{  // **没有**扁平键的兼容双读
		TempLua f("rt_mlod_flat", "return { meshlet_shadow_lod = 5 }\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletShadowLod == -1);
	}
}

TEST_CASE("runtime_config：meshlet.shadow_lod_fallback（★18-B1 回落策略）——两半各钉 + 未知值 fail-fast") {
	// 判据来源 = 实读 applyMeshletShadowLodFallbackKey（形态先例 = applyMeshletCoplanarKey）。
	// 🔴 两半各一条用例（不是"测一个代表值"）：deepest 与 l0 在 D=7 上差 10 倍成本
	// （§18.2：3.32% vs 34.97% 的 L0 三角），只钉默认那侧的话，另一侧的映射写错无人发现。
	// 变异红线声明：
	//   ① 默认值翻成 l0 → 「成员初始化」「缺块」「缺键」三条红。
	//   ② "l0" 分支映射错（写成 deepest）→ 「显式命中 l0」红（默认也是 deepest，故必须显式给）。
	//   ③ 未知字符串静默回落 → 「未知字符串即失败」三条红（拼错值 / 大小写错 / 空串）。
	//      这条是本键最坏的失效：静默回落成默认 = 成本判据整批读错的臂，而日志一切正常。
	//      ⚠ 覆盖面 = 字符串值；**数字值**走「缺键保默认」不报错（sol 的 optional<string> 对
	//      number 给 nullopt，与先例 coplanar 共有）——该限制由下方最后一条用例显式钉住。
	CHECK(MeshletLodFallbackMode::deepest != MeshletLodFallbackMode::l0);  // 两档必须可区分
	{  // 成员初始化默认路径
		CHECK(RuntimeConfig{}.meshletShadowLodFallback == MeshletLodFallbackMode::deepest);
	}
	{  // 缺块 / 缺键 = deepest
		TempLua f("rt_mfb_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletShadowLodFallback == MeshletLodFallbackMode::deepest);
		TempLua g("rt_mfb_nokey", R"lua(return { meshlet = { shadow_lod = 5 } })lua");
		auto r2 = loadRuntimeConfig(g.pathStr());
		REQUIRE(r2.has_value());
		CHECK(r2->meshletShadowLodFallback == MeshletLodFallbackMode::deepest);
	}
	{  // 两半各钉：显式 deepest / 显式 l0（探针值 ≠ 默认，漏读或错映射即红）
		TempLua a("rt_mfb_deepest",
			R"lua(return { meshlet = { shadow_lod_fallback = "deepest" } })lua");
		auto r = loadRuntimeConfig(a.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletShadowLodFallback == MeshletLodFallbackMode::deepest);
		TempLua b("rt_mfb_l0", R"lua(return { meshlet = { shadow_lod_fallback = "l0" } })lua");
		auto r2 = loadRuntimeConfig(b.pathStr());
		REQUIRE(r2.has_value());
		CHECK(r2->meshletShadowLodFallback == MeshletLodFallbackMode::l0);
	}
	{  // 未知**字符串** = **启动失败**并点名键，不静默回落（两半之外的值都要落这条）
		for (const char* bad : {R"lua(return { meshlet = { shadow_lod_fallback = "shallowest" } })lua",
		                        R"lua(return { meshlet = { shadow_lod_fallback = "L0" } })lua",
		                        R"lua(return { meshlet = { shadow_lod_fallback = "" } })lua"}) {
			TempLua f("rt_mfb_bad", bad);
			auto r = loadRuntimeConfig(f.pathStr());
			CHECK_FALSE(r.has_value());
		}
	}
	{  // 🔴 已知限制（**与先例 coplanar 共有，实机对照过**）：值写成**数字**时 sol 的
		// `sol::optional<std::string>` 给 nullopt ⟹ 走「缺键保默认」那条，**不**报错。
		// 本用例把它钉成显式行为而不是留白：① 别误以为本键有 readStrictIntKey 那种全类型严格面
		// （那枚走的是 `sol::object::get_type()`，另一条代码形状）；② 将来若把字符串键也换成
		// 类型严格读，本断言会红 ⟹ 逼改的人显式裁决，而不是悄悄多覆盖一种输入。
		TempLua f("rt_mfb_number_is_missing_key",
			"return { meshlet = { shadow_lod_fallback = 0 } }\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletShadowLodFallback == MeshletLodFallbackMode::deepest);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// diagnostics
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("diagnostics：空表 = 全默认（N6b 起无阴影字段；texelCanary=MAX、dumpGbuf=−1）") {
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
	// N6b：阴影九字段（shadowPass / shadowPcss / shadowEvssm / pcss* / izb*）已从
	// DiagnosticsConfig 删除——交付形态的真相搬到 ShadowConfig（其默认值由下方
	// shadow_config 那组用例钉），这里只余「diagnostics 侧不再有阴影字段」的事实。
	// 缺文件同样全默认 + warn（不报错）
	std::string missing = (std::filesystem::temp_directory_path() / "rgl_cfg_no_such_diag.lua").string();
	auto m = loadDiagnosticsConfig(missing);
	REQUIRE(m.has_value());
	CHECK(m->texelCanary == ~uint64_t{0});
	CHECK(m->frameStats == 0u);
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
	{  // bool 门两侧：imgui / rpc（默认 false）可写 true、imgui 显式写 false 也生效。
	   // 若 bool 解析坏掉恒读默认，这些断言必红——纯默认值测试永远绿不了坏读取器。
	   // （N6a 起 shadow_pass 不再可用：旧阴影键出现 = 启动失败，见下方 fail-fast 用例；
	   //   「默认 true 的门可写 false」方向由 shadow_config 的 izb 用例接管。）
		TempLua f("dg_bools", "return { imgui = true, rpc = true, no_vvl = true }\n");
		auto r = loadDiagnosticsConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->imgui == true);
		CHECK(r->rpc == true);
		CHECK(r->noVvl == true);
		TempLua g("dg_bools_off", "return { imgui = false }\n");
		auto r2 = loadDiagnosticsConfig(g.pathStr());
		REQUIRE(r2.has_value());
		CHECK(r2->imgui == false);
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
	c.picking = true;  // N6b：原来这里用 shadowPass，字段已删；换任意标量门等价
	setDiagnostics(c);
	CHECK(diagnostics().frameStats == 77u);
	CHECK(diagnostics().dumpGbuf == 2);
	CHECK(diagnostics().picking == true);
	// 还原全默认，避免污染同进程其它用例（单例 = 进程级状态，谁改谁负责收尾）
	setDiagnostics(DiagnosticsConfig{});
	CHECK(diagnostics().frameStats == 0u);
}

TEST_CASE("shadow_config：setShadowConfig → shadowConfig() 单例往返（N6b 回灌路径）") {
	// 消费点全部经 shadowConfig() 读形态（六处签名不穿参数）⟹ 这条往返 = 那条回灌
	// 路径唯一的单测覆盖点。变异红线：main 的 setShadowConfig 删掉 → 交付时单例恒为
	// 出厂默认，本用例仍绿（它测的是 setter/getter 契约），但 just smoke 的
	// 「shadow: mode=…」回显行会与 configs/shadow.lua 不符。
	ShadowConfig c;
	c.mode = ShadowMode::evssm;
	c.izb = false;
	c.evssm.searchRadius = 41.0;
	setShadowConfig(c);
	CHECK(shadowConfig().mode == ShadowMode::evssm);
	CHECK(shadowConfig().izb == false);
	CHECK(shadowConfig().evssm.searchRadius == 41.0);
	// 还原出厂形态（单例 = 进程级状态，谁改谁负责收尾）
	setShadowConfig(ShadowConfig{});
	CHECK(shadowConfig().mode == ShadowMode::csm);
	CHECK(shadowConfig().izb == false);  // 交付默认关（2026-10-10 换红线，理由见 ShadowConfig 头注）
}

TEST_CASE("shadowFormPlan：形态决策表逐档四件事（N6b 选路判断的唯一收口）") {
	// 表本体是纯函数（不读单例）⟹ 这里逐档钉，加一档漏改一行立刻红。
	// 口径权威 = docs/shadow_routes_plan.md §2；本文件不抄表（红线 4），只钉**行为**。
	const auto planOf = [](ShadowMode m, bool izb) {
		const ShadowConfig c{.mode = m, .izb = izb};
		return shadowFormPlan(c);
	};
	// drawAtlas：none 之外三档都画（pcss/evssm 的软影建立在同一张 CSM 图集上）
	CHECK(shadowFormPlan(ShadowConfig{.mode = ShadowMode::none}).drawAtlas == false);
	for (const auto m : {ShadowMode::csm, ShadowMode::pcss, ShadowMode::evssm})
		CHECK(planOf(m, false).drawAtlas == true);
	// spvSuffix：只有两档软影有变体后缀（门关 = 零后缀 = 零变体的逐位红线）
	CHECK(planOf(ShadowMode::none, false).spvSuffix == "");
	CHECK(planOf(ShadowMode::csm, false).spvSuffix == "");
	CHECK(planOf(ShadowMode::pcss, false).spvSuffix == "_PCSS_SOFT");
	CHECK(planOf(ShadowMode::evssm, false).spvSuffix == "_EVSSM_SOFT");
	// buildEvssmMoments：只有 evssm 档建矩图（pcss 档图集照画但不建矩图）
	CHECK(planOf(ShadowMode::pcss, true).buildEvssmMoments == false);
	CHECK(planOf(ShadowMode::evssm, false).buildEvssmMoments == true);
	// consumeIzbMask：**只看 izb**，刻意不掺 drawAtlas（债-32 的收口 = IzbShadow 在
	// mode=none 下也构造、也每帧无条件清零）。变异红线：掺上 drawAtlas → 本断言红。
	CHECK(planOf(ShadowMode::none, true).consumeIzbMask == true);
	CHECK(planOf(ShadowMode::csm, false).consumeIzbMask == false);
	// **本用例刻意不测"本档的调参块"**：那个量在运行期没有消费者（两个 scene data 区
	// 同时被写、各自只有一个 shader 变体会读），给它写断言 = 自指绿例。调参按档分立的
	// 真护栏在 tests/test_render_list_shadow.cppm 的同名用例（串档即红）。
}

namespace {
bool containsKey(const std::vector<std::string>& v, std::string_view k) {
	for (const auto& s : v)
		if (s == k)
			return true;
	return false;
}
}  // namespace

TEST_CASE("diagnostics 回显覆盖率护栏：解析器读过的键集合 == 回显表键集合（双向）") {
	// 回显此前是手写子集（38 门只列 ~11 个，连默认开的阴影总开关都不出现）。
	// 本用例是防漂移护栏（docs/gate_lint_plan.md §3-B）：两个方向都断——
	//   回显表删一行 / 解析器漏记一键 / 两处任一侧改拼写 → set 相等立刻红。
	// 探针子表字段只在表存在时才被读 → 测试文件写全三张探针表。
	// 变异红线：kGateEcho 删任一条目或 readBool/readI64 删记录行 → 本用例红。
	TempLua f("dg_keys", R"lua(
return {
  picking_probe = { frame = 0, x = 0, y = 0 },
  ui_click_probe = { frame = 0, x = 0, y = 0 },
  camera_probe = { frame = 0, key = "W", holdFrames = 8 },
}
)lua");
	DiagnosticsKeyTrace trace;
	auto r = loadDiagnosticsConfig(f.pathStr(), &trace);
	REQUIRE(r.has_value());
	const auto echo = diagnosticsEchoKeys();
	CHECK(echo.size() == 30u);            // 钉死门数：加门不更新回显表 → 这里先红
	CHECK(trace.read.size() == 30u);      // 解析器恰好读一遍（重复读 = 记录点放错层）
	for (const auto& k : echo)
		CHECK(containsKey(trace.read, k));    // 回显有而解析器不读 = 陈旧键
	for (const auto& k : trace.read)
		CHECK(containsKey(echo, k));          // 解析器读而回显不列 = 撒谎行（原病灶）
}

TEST_CASE("diagnostics 未知键：拼错的顶层键被记录且不在读集（lint 判据成立）；正确键不误报") {
	TempLua f("dg_typo", "return { no_vvvl = true, shadow_pas = true, no_vvl = true }\n");
	DiagnosticsKeyTrace trace;
	auto r = loadDiagnosticsConfig(f.pathStr(), &trace);
	REQUIRE(r.has_value());
	CHECK(trace.present.size() == 3u);
	CHECK(containsKey(trace.present, "no_vvvl"));
	CHECK(containsKey(trace.present, "shadow_pas"));
	// 反向用例（常错的 lint = 被静音的 lint）：拼写正确的键必须**在**读集里，
	// 探针表名本身不算未知键（前缀 = 子表字段键的表名前缀）
	CHECK(containsKey(trace.read, "no_vvl"));
	CHECK_FALSE(containsKey(trace.read, "no_vvvl"));
	CHECK_FALSE(containsKey(trace.read, "shadow_pas"));
	// 解析本身不受影响：正确键照常生效
	CHECK(r->noVvl == true);
}

TEST_CASE("diagnostics 未知键（探针子表）：探针表名不算未知键——前缀判据的正向对照") {
	// 若判据写成「回显键名全名 == 顶层键」，三张探针表名会全被误报（子表字段的
	// 回显拼写是 `表名.字段`）。本用例钉住「表名 → 前缀命中 → 不喊」。
	TempLua f("dg_probetab", "return { picking_probe = { frame = 5 }, rpc_port = 9999 }\n");
	DiagnosticsKeyTrace trace;
	auto r = loadDiagnosticsConfig(f.pathStr(), &trace);
	REQUIRE(r.has_value());
	REQUIRE(trace.present.size() == 2u);
	for (const auto& key : trace.present) {
		// 与 lintUnknownDiagnosticsKeys 同判据：任一回显键名的 `.` 前缀 == 键 → 已知
		// （判据的表 = 回显表本身，不另立名单）
		bool known = false;
		for (const auto& e : diagnosticsEchoKeys())
			if (std::string_view{e}.substr(0, std::string_view{e}.find('.')) == key)
				known = true;
		CHECK(known);  // known = false 会被解析器 warn，但 warn 测不到——这里测判据本体
	}
	CHECK(r->rpcPort == 9999u);  // 合法值照常生效（lint 不改解析结果）
}

TEST_CASE("diagnostics cull_replay：三态各自生效 + 缺键回落 cpu + 未知值 fail-fast") {
	// 债-30 方案 C（docs/cpu_perf_plan.md §4）。为什么未知值必须启动失败而不是回落默认：
	// 回落的失效形态 = "你以为在测 GPU 剔除路，其实跑的是 CPU 路"，而两种读数看起来都
	// 合理（present_mode / aa 的同一条纪律）。反向用例（合法值不误拒）逐态各钉一条。
	auto load = [](std::string_view body) {
		TempLua f("dg_cullreplay", body);
		return loadDiagnosticsConfig(f.pathStr(), nullptr);
	};
	CHECK(load("return {}\n")->cullReplay == CullReplayMode::cpu);  // 缺键 = 出厂默认
	CHECK(load("return { cull_replay = \"cpu\" }\n")->cullReplay == CullReplayMode::cpu);
	CHECK(load("return { cull_replay = \"gpu_lagged\" }\n")->cullReplay == CullReplayMode::gpuLagged);
	CHECK(load("return { cull_replay = \"gpu_sync\" }\n")->cullReplay == CullReplayMode::gpuSync);
	const auto bad = load("return { cull_replay = \"gpu\" }\n");  // 前缀命中也不算合法
	CHECK_FALSE(bad.has_value());
	if (!bad)
		CHECK(bad.error().message.find("cull_replay") != std::string::npos);
	// 回显 = 配置名本身，且差集语义下默认值不喊（默认配置写进 lua 也不狂喊）
	const DiagnosticsConfig d;
	CHECK(describeDiagnosticsGates(d, d).find("cull_replay") == std::string::npos);
	DiagnosticsConfig c;
	c.cullReplay = CullReplayMode::gpuSync;
	CHECK(describeDiagnosticsGates(c, d) == "cull_replay=gpu_sync");
	CHECK(std::string_view{cullReplayName(CullReplayMode::gpuLagged)} == "gpu_lagged");
}

TEST_CASE("diagnostics 回显 describe：全默认 = 空串；改动只列改动门（info 差集语义）；debug 全量") {
	const DiagnosticsConfig d;
	// 反向用例：全默认一个都不喊（configs 写满默认值不狂喊的实现基础）
	CHECK(describeDiagnosticsGates(d, d).empty());
	DiagnosticsConfig c;
	c.dumpGbuf = 2;
	c.picking = true;  // N6b：原来这里改的是 izbShadow / shadowPass（字段已删，改判任意标量门）
	const std::string s = describeDiagnosticsGates(c, d);
	CHECK(s.find("dump_gbuf=2") != std::string::npos);
	CHECK(s.find("picking=true") != std::string::npos);
	CHECK(s.find("no_vvl") == std::string::npos);       // 未动的门不出现
	CHECK(s.find("texel_canary") == std::string::npos); // 未动的哨兵门不出现
	const std::string all = describeAllDiagnosticsGates(d);
	// 全量表 = 30 段 `k=v`（空格分隔），抽查首、中、尾与两个特殊格式条目
	CHECK(std::ranges::count(all, '=') == 30);
	CHECK(all.find("no_vvl=false") != std::string::npos);
	CHECK(all.find("cull_replay=cpu") != std::string::npos);
	CHECK(all.find("texel_canary=off") != std::string::npos);  // MAX 哨兵出可读文本
	CHECK(all.find("camera_probe.key=none") != std::string::npos);
	CHECK(all.find("camera_probe.holdFrames=0") != std::string::npos);
	// N6b：阴影九键不在 diagnostics 回显里（配置面 = configs/shadow.lua，回显由
	// describeAllShadowGates 承担，见下方 shadow 侧用例）
	CHECK(all.find("shadow") == std::string::npos);
	CHECK(all.find("izb") == std::string::npos);
	CHECK(all.find("pcss") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// shadow_config（批 N6a，docs/shadow_routes_plan.md §2 定案的配置面）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("shadow_config：mode 四值各自解析正确 + 缺键回落交付默认 + 配置名往返（回显=配置名）") {
	auto load = [](std::string_view body) {
		TempLua f("sh_mode", body);
		return loadShadowConfig(f.pathStr());
	};
	// 回落断言的基准 = ShadowConfig{} 符号本身（不把默认值抄第二遍当断言）；
	// 「默认 = 交付形态（mode=csm、izb=true）」由结构体定义承载（shadow_config.cppm）。
	const ShadowConfig def;
	{
		auto r = load("return {}\n");
		REQUIRE(r.has_value());
		CHECK(r->mode == def.mode);
		CHECK(r->izb == def.izb);
		CHECK(r->izbGrid == def.izbGrid);
		CHECK(r->izbDumpFrame == def.izbDumpFrame);
		CHECK(r->pcss.sunDegrees == def.pcss.sunDegrees);
		CHECK(r->evssm.maxPenumbra == def.evssm.maxPenumbra);
	}
	// 四值逐个命中（反向用例：合法值不误拒）
	CHECK(load("return { mode = \"none\" }\n")->mode == ShadowMode::none);
	CHECK(load("return { mode = \"csm\" }\n")->mode == ShadowMode::csm);
	CHECK(load("return { mode = \"pcss\" }\n")->mode == ShadowMode::pcss);
	CHECK(load("return { mode = \"evssm\" }\n")->mode == ShadowMode::evssm);
	// 名字往返：shadowModeName → parseShadowMode 同集合（cullReplayName 同一条护栏形状）
	for (const auto m : {ShadowMode::none, ShadowMode::csm, ShadowMode::pcss, ShadowMode::evssm}) {
		auto p = parseShadowMode(shadowModeName(m));
		REQUIRE(p.has_value());
		CHECK(*p == m);
	}
	CHECK(std::string_view{shadowModeName(ShadowMode::evssm)} == "evssm");
}

TEST_CASE("shadow_config：未知 mode fail-fast 且错误列出全部合法值（不静默回落）") {
	// 回落的失效形态 = "你以为在测 EVSSM 其实跑的是 CSM"（cull_replay / present_mode / aa
	// 的同一条纪律）。变异红线：解析表加 else 回落 → 本用例红。
	TempLua f("sh_badmode", "return { mode = \"evssm2\" }\n");
	auto r = loadShadowConfig(f.pathStr());
	CHECK_FALSE(r.has_value());
	if (r)
		return;  // 上面已红；下面解引用 error() 只在失败路径成立
	const std::string& msg = r.error().message;
	CHECK(msg.find("mode") != std::string::npos);
	CHECK(msg.find("\"none\"") != std::string::npos);   // 错误文本列出合法值
	CHECK(msg.find("\"csm\"") != std::string::npos);
	CHECK(msg.find("\"pcss\"") != std::string::npos);
	CHECK(msg.find("\"evssm\"") != std::string::npos);
	CHECK(msg.find("evssm2") != std::string::npos);     // 且回显写错的值本体
	// parseShadowMode 独立面：大小写敏感（"CSM" 不合法）、空串不合法、四合法值不误拒
	CHECK_FALSE(parseShadowMode("CSM").has_value());
	CHECK_FALSE(parseShadowMode("").has_value());
	CHECK(parseShadowMode("none").has_value());
	CHECK(parseShadowMode("csm").has_value());
	CHECK(parseShadowMode("pcss").has_value());
	CHECK(parseShadowMode("evssm").has_value());
}

TEST_CASE("shadow_config：缺文件回落交付默认（warn 不报错，与 diagnostics 同口径）") {
	std::string missing =
		(std::filesystem::temp_directory_path() / "rgl_cfg_no_such_shadow.lua").string();
	auto m = loadShadowConfig(missing);
	REQUIRE(m.has_value());
	CHECK(m->mode == ShadowConfig{}.mode);
	CHECK(m->izb == ShadowConfig{}.izb);
	CHECK(m->pcss.searchRadius == ShadowConfig{}.pcss.searchRadius);
}

TEST_CASE("shadow_config：数值键 clamp 边界（sun_degrees [0,5]、两半径 [1,64]）；pcss/evssm 按档分立不串味") {
	// clamp 口径 = 旧 pcss_* 三键原样（diagnostics_impl applyScalarGates 的 std::clamp 界）。
	{  // 下界外 → 界值（不是 0、不是原值）
		TempLua f("sh_clamp_lo",
			"return { pcss = { sun_degrees = -1.0, search_radius = 0.5, max_penumbra = 0.0 } }\n");
		auto r = loadShadowConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->pcss.sunDegrees == 0.0);
		CHECK(r->pcss.searchRadius == 1.0);
		CHECK(r->pcss.maxPenumbra == 1.0);
	}
	{  // 上界外 → 界值
		TempLua f("sh_clamp_hi",
			"return { pcss = { sun_degrees = 99.0, search_radius = 100.0, max_penumbra = 65.0 } }\n");
		auto r = loadShadowConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->pcss.sunDegrees == 5.0);
		CHECK(r->pcss.searchRadius == 64.0);
		CHECK(r->pcss.maxPenumbra == 64.0);
	}
	{  // 界内原样（clamp 不是恒取界）
		TempLua f("sh_inrange",
			"return { pcss = { sun_degrees = 2.5, search_radius = 16.0, max_penumbra = 32.0 } }\n");
		auto r = loadShadowConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->pcss.sunDegrees == 2.5);
		CHECK(r->pcss.searchRadius == 16.0);
		CHECK(r->pcss.maxPenumbra == 32.0);
	}
	{  // 按档分立（债-16⑤ 的消账锚点）：pcss 写值不串 evssm，evssm 未写键保默认
		TempLua f("sh_split",
			"return { pcss = { sun_degrees = 1.0 }, evssm = { sun_degrees = 2.0, search_radius = 8.0 } }\n");
		auto r = loadShadowConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->pcss.sunDegrees == 1.0);
		CHECK(r->evssm.sunDegrees == 2.0);
		CHECK(r->evssm.searchRadius == 8.0);
		CHECK(r->pcss.searchRadius == ShadowConfig{}.pcss.searchRadius);  // 未写键 = 默认
		CHECK(r->evssm.maxPenumbra == ShadowConfig{}.evssm.maxPenumbra);
	}
}

TEST_CASE("shadow_config：izb 两形态（布尔简写 / 子表 on+grid+dump_frame；grid clamp [64,2048]、类型错 fail-fast）") {
	{  // 布尔简写 = 只开关，grid/dump_frame 保默认
		TempLua f("sh_izb_off", "return { izb = false }\n");
		auto r = loadShadowConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->izb == false);
		CHECK(r->izbGrid == ShadowConfig{}.izbGrid);
		CHECK(r->izbDumpFrame == ShadowConfig{}.izbDumpFrame);
	}
	{  // 子表三键各命中
		TempLua f("sh_izb_tab", "return { izb = { on = false, grid = 1024, dump_frame = 7 } }\n");
		auto r = loadShadowConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->izb == false);
		CHECK(r->izbGrid == 1024u);
		CHECK(r->izbDumpFrame == 7u);
	}
	{  // 子表缺 on = 保默认 **false**（交付默认，2026-10-10）；grid 越界夹 [64,2048]；
	   // dump_frame 负数 = 0（旧 izb_* 口径）
		TempLua f("sh_izb_clamp", "return { izb = { grid = 10, dump_frame = -3 } }\n");
		auto r = loadShadowConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->izb == false);  // 只写 grid/dump_frame 不动开关 = 保交付默认
		CHECK(r->izbGrid == 64u);
		CHECK(r->izbDumpFrame == 0u);
		TempLua g("sh_izb_big", "return { izb = { grid = 99999 } }\n");
		CHECK(loadShadowConfig(g.pathStr())->izbGrid == 2048u);
	}
	{  // 类型错 = 报错不静默（"配置写了、什么都没发生"是本项目最怕的失效形态）
		TempLua f("sh_izb_str", "return { izb = \"yes\" }\n");
		CHECK_FALSE(loadShadowConfig(f.pathStr()).has_value());
	}
}

TEST_CASE("shadow_config 回显覆盖率护栏：解析器读过的键集合 == 回显表键集合（双向）+ 未知键判据") {
	// diagnostics 同形护栏（docs/gate_lint_plan.md §3-B）：回显表删一行 / 解析器漏记一键 /
	// 两处任一侧改拼写 → set 相等立刻红。变异红线：kShadowEcho 删任一条目 → 本用例红。
	TempLua f("sh_keys", "return { mode = \"csm\", izb = true, csm = { }, pcss = { }, evssm = { } }\n");
	ShadowKeyTrace trace;
	auto r = loadShadowConfig(f.pathStr(), &trace);
	REQUIRE(r.has_value());
	const auto echo = shadowConfigEchoKeys();
	CHECK(echo.size() == 13u);         // 钉死键数：加键不更新回显表 → 这里先红
	CHECK(trace.read.size() == 13u);   // 解析器恰好读一遍（重复读 = 记录点放错层）
	for (const auto& k : echo)
		CHECK(containsKey(trace.read, k));  // 回显有而解析器不读 = 陈旧键
	for (const auto& k : trace.read)
		CHECK(containsKey(echo, k));        // 解析器读而回显不列 = 撒谎行
	// csm = {} 是定案形状里的预留块：present 有它、读集也有它（未知键 lint 不喊）
	CHECK(containsKey(trace.present, "csm"));
	{  // 未知键判据：拼错的顶层键进 present 而不进 read（lint 的输入成立）；正确键不误报
		TempLua g("sh_typo", "return { mode2 = \"csm\", mod = \"csm\", mode = \"csm\" }\n");
		ShadowKeyTrace t2;
		auto r2 = loadShadowConfig(g.pathStr(), &t2);
		REQUIRE(r2.has_value());
		CHECK(t2.present.size() == 3u);
		CHECK(containsKey(t2.present, "mode2"));
		CHECK(containsKey(t2.read, "mode"));            // 正确键照常生效
		CHECK_FALSE(containsKey(t2.read, "mode2"));
		CHECK_FALSE(containsKey(t2.read, "mod"));
		CHECK(r2->mode == ShadowMode::csm);
	}
}

TEST_CASE("shadow_config 回显 describe：全默认 = 空串；改动只列改动键（info 差集语义）；debug 全量 13 段") {
	const ShadowConfig d;
	CHECK(describeShadowGates(d, d).empty());  // 反向用例：全默认一个都不喊
	ShadowConfig c;
	c.mode = ShadowMode::evssm;
	c.izb = true;  // 默认已关 ⟹ 开回来才是"改动"，才会进 info 差集
	c.evssm.searchRadius = 48.0;
	const std::string s = describeShadowGates(c, d);
	CHECK(s.find("mode=evssm") != std::string::npos);
	CHECK(s.find("izb=true") != std::string::npos);
	CHECK(s.find("evssm.search_radius=48") != std::string::npos);
	CHECK(s.find("pcss.sun_degrees") == std::string::npos);  // 未动的键不出现
	const std::string all = describeAllShadowGates(d);
	CHECK(std::ranges::count(all, '=') == 13);  // 全量 = 15 段 k=v（空格分隔）
	CHECK(all.find("mode=csm") != std::string::npos);
	CHECK(all.find("izb=false") != std::string::npos);
	CHECK(all.find("csm=reserved") != std::string::npos);  // 预留块出可读文本（差集永不喊）
}

TEST_CASE("diagnostics 旧阴影键 fail-fast：9 键逐个出现 = 启动失败并指路 configs/shadow.lua；无旧键文件正常") {
	// N6a 红线 2（docs/shadow_routes_plan.md §2）：不静默映射。每个旧键单独出现都要炸，
	// 且报错**点名旧键 + 指路新落点**（指到能直接抄的程度）。反向用例（无旧键不误拒）
	// 钉底——常炸的 fail-fast = 被静音的 fail-fast。
	struct OldKey {
		const char* key;
		const char* value;
	};
	static constexpr std::array<OldKey, 9> oldKeys{{
		{"shadow_pass", "true"},
		{"shadow_pcss", "false"},
		{"shadow_evssm", "false"},
		{"pcss_sun_degrees", "0.53"},
		{"pcss_search_radius", "16.0"},
		{"pcss_max_penumbra", "32.0"},
		{"izb_shadow", "true"},
		{"izb_grid", "512"},
		{"izb_dump_frame", "0"},
	}};
	for (const auto& [key, value] : oldKeys) {
		const std::string body = "return { " + std::string(key) + " = " + value + " }\n";
		TempLua f("dg_movedkey", body);
		auto r = loadDiagnosticsConfig(f.pathStr());
		CHECK_FALSE(r.has_value());
		if (!r) {
			CHECK(r.error().message.find(key) != std::string::npos);           // 点名旧键
			CHECK(r.error().message.find("shadow.lua") != std::string::npos);  // 指路新落点
		}
	}
	// 反向：不含旧键的文件正常加载（fail-fast 不是恒拒）
	TempLua ok("dg_movedok", "return { no_vvl = true, rpc_port = 9000 }\n");
	auto r = loadDiagnosticsConfig(ok.pathStr());
	REQUIRE(r.has_value());
	CHECK(r->noVvl == true);
	CHECK(r->rpcPort == 9000u);
}

}  // namespace RGL
