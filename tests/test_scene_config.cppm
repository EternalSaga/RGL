module;
#include <doctest/doctest.h>
#include <algorithm>   // std::ranges::count（回显全量条目计数；先例 test_evssm_math.cppm）
#include <glm/glm.hpp>
#include <filesystem>  // 临时 lua 落盘（AGENTS.md GMF 例外清单成员）
#include <iterator>    // std::istreambuf_iterator（LogTailer 读日志增量；modules 下不靠传递 include——rgl-test-discipline §modules 环境坑同款）

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
import rllogger;  // LogTailer 逼出 logger 构造（warn 阳性锚点的观测通道，见类注释）

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

/**
 * 日志增量锚点（债-11 阳性锚点的观测通道）：RLLogger 是进程单例劫持不了
 *（tests/test_mdi_shadow_emit 同结论），但它的 file sink **逐条 flush**
 *（flush_on(warn)）且路径 = configs/log_level.json 的 log_file——本文件的 loader
 * 未知键 lint 早已依赖「CWD = 仓库根能读到 configs/log_level.json」这一事实
 *（`just test` 保证；diagnostics 侧既有用例同款），这里只是把同一事实用到**读**上。
 * 构造时先逼出 logger 构造（含 file sink 的 truncate，**必须早于记大小**），
 * delta() = 自构造起新增的日志文本。
 * ⚠ 读不到日志文件 = REQUIRE 响亮失败，不静默空增量（空增量会被正向断言读成红、
 *   却被「不该喊」的负向断言读成绿——所以锚点自身不许假绿）。
 */
class LogTailer {
  public:
	LogTailer() {
		RLLogger::getInstance();  // 逼出构造（含 truncate），顺序必须早于 file_size
		std::error_code ec;
		offset_ = std::filesystem::file_size(path(), ec);
		if (ec)
			offset_ = 0;  // 文件不存在 = 从头读（下一条 REQUIRE 会点名"为什么没有"）
	}
	LogTailer(const LogTailer&) = delete;
	LogTailer& operator=(const LogTailer&) = delete;
	/** 自构造以来新增的日志文本。 */
	std::string delta() const {
		std::ifstream in(path(), std::ios::binary);
		REQUIRE(in.is_open());  // 读不到 = 判据不存在，FAIL 不 SKIP
		std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		if (data.size() < offset_)
			return {};  // 文件被再次截断（进程内二次构造 logger）——按空增量处理，正向断言会红
		return data.substr(static_cast<size_t>(offset_));
	}

  private:
	/** log_file 路径（configs/log_level.json 的字符串扫描，stdlib-only；键缺失 = 响亮红）。 */
	static const std::filesystem::path& path() {
		static const std::filesystem::path cached = [] {
			std::ifstream json{"configs/log_level.json"};
			REQUIRE(json.is_open());  // 与 loader lint 同一条 CWD 依赖
			std::string text((std::istreambuf_iterator<char>(json)), std::istreambuf_iterator<char>());
			const auto keyPos = text.find("\"log_file\"");
			REQUIRE(keyPos != std::string::npos);
			const auto open = text.find('"', keyPos + 10);
			REQUIRE(open != std::string::npos);
			const auto close = text.find('"', open + 1);
			REQUIRE(close != std::string::npos);
			return std::filesystem::path(text.substr(open + 1, close - open - 1));
		}();
		return cached;
	}

	std::uint64_t offset_ = 0;
};

/** 断言 delta 里出现「<前缀>: 未知键 '<key>'」warn 行（与三份 loader 的文案逐字同拼写）。 */
bool hasUnknownKeyWarn(const std::string& delta, std::string_view prefix, std::string_view key) {
	return delta.find(std::string(prefix) + ": 未知键 '" + std::string(key) + "'") != std::string::npos;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// scene_config
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("scene_config：缺文件回落默认（warn 不报错）——默认 = 9 管线 + 两族各 3 风格 + 空 entities") {
	// 缺文件路径：temp 目录下不存在的名
	std::string missing = (std::filesystem::temp_directory_path() / "rgl_cfg_no_such_scene.lua").string();
	auto r = loadSceneConfig(missing);
	REQUIRE(r.has_value());
	// 默认画像（defaultSceneConfig，scene_config_impl.cppm）。原 12 管线里的 forward/skybox
	// 规格与 styles 表已随刀②.2 删除（docs/archive/图变体收敛_plan.md §5），剩下的 = cube + 四条
	// deferred 规格 + 四条透明规格（透明混合管线刀1）。
	// **实际行为**：缺文件路径**不经 parseEntities**，entities 保持**空**——
	// 与"存在但空的 lua"不同（下一条），这个不对称是既有语义，钉住防"顺手统一"。
	CHECK(r->pipelines.size() == 9);
	CHECK(r->materialRules.size() == 2);
	CHECK(r->deferredStyleMap.size() == 3);
	// unlit 2026-10-18 起**必须在榜**（.at 缺键即抛 = 断言）：KHR_materials_unlit 材质
	// 回落 deferred_geometry 被静默打光 = D1 破口本体（探针重立实测抓出）。
	CHECK(r->deferredStyleMap.at("unlit") == "deferred_geometry_unlit");
	// 透明族与 deferred 族**同数同形**是刻意的（blend 与类别两个独立维度各自成立），
	// 两族条数不等 = 有一族漏了格，那条风格的透明体静默走 PBR 前向。
	CHECK(r->transparentStyleMap.size() == r->deferredStyleMap.size());
	CHECK(r->transparentStyleMap.at("unlit") == "transparent_unlit");
	CHECK(r->transparentStyleMap.at("vegetation") == "transparent_vegetation");
	CHECK(r->transparentStyleMap.at("light") == "transparent_light");
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
	// 管线/两族风格表 = 默认集（parsePipelines / parseStyleMap 缺段不覆盖）
	CHECK(r->pipelines.size() == 9);
	CHECK(r->deferredStyleMap.size() == 3);
	CHECK(r->deferredStyleMap.at("vegetation") == "deferred_geometry_vegetation");
	CHECK(r->transparentStyleMap.at("vegetation") == "transparent_vegetation");
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
  intensity = 0.5, hdr_size = 1024 } }
)lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->environment.enabled == true);
		CHECK(r->environment.dir == "sky");
		// faces 按 Vulkan 层序 0-5（+X,−X,+Y,−Y,+Z,−Z）逐位透传
		CHECK(r->environment.faces[0] == "px");
		CHECK(r->environment.faces[5] == "nz");
		expectNear(r->environment.intensity, 0.5, 0.0, 1e-6);
		// 原 skybox 布尔（背景天穹开关，forward draw 侧 knob）已随刀②.2 删除——键表里
		// 也没有它了，配置里再写它会被未知键 warn（键表对账 = 本文件「键覆盖」那组用例）。
		CHECK(r->environment.hdrSize == 1024u);
	}
	{  // hdr_size 非 2 的幂 = 报错（parseEnvironment:252-254）
		TempLua f("sc_env_pow2", R"lua(return { environment = { hdr_size = 100 } })lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().message.find("2") != std::string::npos);  // "须为 2 的幂"
	}
}

// ── resolvePipelineSpecName：风格 → 规格名（透明混合管线刀1 的 blend 护栏）─────────────
// 判据本体 = scene_config_impl.cppm::resolvePipelineSpecName（两步：风格键 → 族）。
// 🔴 本用例守的是那条**顺序护栏**：blend 图元永不查 deferred_styles，因此永不拿到
// 写 5-MRT 的 deferred_geometry* 规格——透明 pass 只挂一张色附件，那种条目画进那张 pass
// 是"编译得过、VVL 才炸"的形态（论证 → docs/透明混合管线_plan.md §1-3）。
//
// **变异红线**（改坏被测码必须让本用例红，答不出 = 摆设）：
//  A 族判据恒走 deferred（`alphaModeBlend ? transparentStyleMap : deferredStyleMap`
//    → 去掉三元）→ 「blend 拿透明族」三条 + 「缺格回落 transparent」一条红；
//  B 回落值写死 deferred 本体（三元回落 → kDeferredGeometrySpec）→ 「缺格回落」一条红
//    （这条专防"族判据只写了一半：查表走对了、回落走错了"）；
//  C 族判据反向（`!alphaModeBlend`）→ **反向用例**（blend=false 必须留在 deferred 族）红；
//  D 风格派生里 material_rules 的 break 丢掉（有序首个命中 → 末个命中）→ 风格键断言红
//    （本用例同时是刀1 之前那条派生逻辑的回归锚：blend=false 的期望值逐字承自旧实现）。
TEST_CASE("resolvePipelineSpecName：blend 决定族、风格决定族内哪一支；blend 永不拿 G-buffer 规格") {
	// ⚠ **不是 `SceneConfig d;`**：materialRules / 两张风格表**没有类内初始化器**，默认构造 =
	// 全空（默认值住 defaultSceneConfig()，同 DiagnosticsConfig 的"结构体默认 = 交付形态"不同）。
	const SceneConfig d = defaultSceneConfig();
	// ① blend=false：与刀1 之前逐字同结果（回归锚，四条覆盖 unlit/两 pattern/回落）
	CHECK(resolvePipelineSpecName(d, "", true, false) == "deferred_geometry_unlit");
	CHECK(resolvePipelineSpecName(d, "Foliage_Tree_01", false, false) == "deferred_geometry_vegetation");
	CHECK(resolvePipelineSpecName(d, "Emissive_Lamp", false, false) == "deferred_geometry_light");
	CHECK(resolvePipelineSpecName(d, "Wall_Stone", false, false) == "deferred_geometry");
	// ② blend=true：同一批输入必须**整族换掉**（风格语义保住，族换成前向单附件）
	CHECK(resolvePipelineSpecName(d, "", true, true) == "transparent_unlit");
	CHECK(resolvePipelineSpecName(d, "Foliage_Tree_01", false, true) == "transparent_vegetation");
	CHECK(resolvePipelineSpecName(d, "Emissive_Lamp", false, true) == "transparent_light");
	CHECK(resolvePipelineSpecName(d, "Wall_Stone", false, true) == "transparent");
	// ③ 护栏的**穷尽形式**：不是"上面四条恰好没返回 deferred"，而是"任何材质名都返回不了"。
	// 名字表刻意含两族全部风格词 + 空串（编译期字面量数组，不可能为空 = 无需空集合守卫）。
	static constexpr std::string_view names[]{
		"", "Foliage", "Emissive", "Foliage_Emissive_Both", "unlit", "Whatever_Mesh_02"};
	static constexpr bool unlitFlags[]{false, true};
	for (std::string_view n : names)
		for (bool u : unlitFlags) {
			const std::string spec = resolvePipelineSpecName(d, n, u, true);
			CHECK(spec.find("deferred") == std::string::npos);  // blend 永不进 G-buffer 族
			CHECK_FALSE(spec.empty());  // 空规格名 = findSpec 回落 nullptr，比错族更糟
		}
	// ④ 缺格回落（变异 B 的靶）：透明族清空 ⟹ 回落**透明本体**，不是 deferred 本体。
	// deferred 侧的对照用"风格不在表里"的输入（pbr 两族都不上榜 = 它就是回落值本身），
	// 而不是把 deferred 族也清空——清空两份就测不到"两族各自回落"这件事了。
	SceneConfig emptyMap = d;
	emptyMap.transparentStyleMap.clear();
	CHECK(resolvePipelineSpecName(emptyMap, "Foliage_Tree_01", false, true) == "transparent");
	CHECK(resolvePipelineSpecName(d, "Wall_Stone", false, false) == "deferred_geometry");
	// ⑤ 两族独立：用户只覆盖透明族 ⟹ deferred 族一格不动（防"两张表其实指向同一个 map"）
	SceneConfig one = d;
	one.transparentStyleMap["vegetation"] = "my_blend_veg";
	CHECK(resolvePipelineSpecName(one, "Foliage_Tree_01", false, true) == "my_blend_veg");
	CHECK(resolvePipelineSpecName(one, "Foliage_Tree_01", false, false) == "deferred_geometry_vegetation");
}

TEST_CASE("scene_config：transparent_styles 与 deferred_styles 各自覆盖默认（共用 parseStyleMap）") {
	// 共用解析函数（parseStyleMap）的阳性锚点：只写 transparent_styles 时，deferred_styles
	// 保持默认集（若两表误共用一个 clear/into，这里必红）。
	TempLua f("sc_tstyles", R"lua(return { transparent_styles = { pbr = "mine_blend" } })lua");
	auto r = loadSceneConfig(f.pathStr());
	REQUIRE(r.has_value());
	REQUIRE(r->transparentStyleMap.size() == 1);  // 覆盖而非追加
	CHECK(r->transparentStyleMap.at("pbr") == "mine_blend");
	CHECK(r->deferredStyleMap.size() == 3);  // 另一族没被牵连
	CHECK(resolvePipelineSpecName(*r, "Wall_Stone", false, true) == "mine_blend");
}

TEST_CASE("scene_config：pipelines/deferred_styles 非空覆盖默认；pipeline 缺 name 报错；语法错误报错") {
	{
		// 原 styles 表（风格→forward 规格）已随刀②.2 删除，本用例的 map 段半边换喂
		// deferred_styles——两段的「非空 = 覆盖而非追加」语义同源（parseStyleMap 先 clear）。
		TempLua f("sc_pipe",
			R"lua(return { pipelines = { { name = "mine", mesh = "m.spv", frag = "f.spv" } },
  deferred_styles = { pbr = "mine" } })lua");
		auto r = loadSceneConfig(f.pathStr());
		REQUIRE(r.has_value());
		REQUIRE(r->pipelines.size() == 1);  // 覆盖而非追加
		CHECK(r->pipelines[0].name == "mine");
		CHECK(r->pipelines[0].meshSpv == "m.spv");
		CHECK(r->pipelines[0].fragSpv == "f.spv");
		REQUIRE(r->deferredStyleMap.size() == 1);  // 覆盖而非追加（map 段半边）
		CHECK(r->deferredStyleMap.at("pbr") == "mine");
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
	// fsr 态**隐含**打开速度场（FSR 的 motionVectors 输入就是那张图）。
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
	// 旧键压不过派生（债-129 S2.5-C 撤并后仍成立的半边）：`velocity_field = false` 已无人读，
	// aa = fsr 的派生照常为 true。变异红线：解析器重新 readBoolKey("velocity_field") → 本条
	// 不红（两态同真）——真正钉「键已死」的是下面反向用例，本条钉的是「旧键写法不炸也不改判」。
	{
		TempLua f("rt_aa_fsr_vel_off", R"lua(return { aa = "fsr", velocity_field = false })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->velocityField == true);
	}
	// 反向（债-129 S2.5-C 撤并判据本体）：旧「非 FSR 消费者」来路已死——velocity_field = true
	// × aa = off 不再打开速度场（键不被解析器读，lintUnknownRuntimeKeys warn 点名）。旧形态
	// 「velocity_field=true × aa=off = 每帧白付全屏 pass + RT」被**结构性**消灭。
	// 变异红线：解析器重新读该键 → 本条红（velocityField 被置 true）。
	{
		TempLua f("rt_aa_off_vel_on", R"lua(return { aa = "off", velocity_field = true })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->aaWantsFsr() == false);
		CHECK(r->velocityField == false);  // 键已撤并 = 纯派生件，只有 aa=fsr 能置真
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

TEST_CASE("runtime_config：meshlet 块（★0-b1 簇路径总门）——缺块 = true、显式关门、无扁平键双读") {
	// 判据来源 = 实读 runtime_config_impl::readMeshletBlock / readBoolKey（缺键 = 不写目标字段）。
	// 🔴 ★18-C 起默认 **true**（用户拍板「默认开 D=5」）。P5「门关 = 逐位不变」仍是硬护栏，
	// 只是**门关不再是出厂态** ⟹ 本用例钉的是「缺块落到出厂默认 true」，而「显式关掉必须生效」
	// 由「块内键名命中」那条钉。
	// 🔴 翻默认同时必须**翻探针方向**：默认变 true 后探针若还写 true，则「键被读到」与
	// 「键被静默忽略」都得到 true ⟹ 用例失去判别力（翻默认最容易留下的假绿形态）。
	// 变异红线声明：
	//   ① 默认值翻回 false → 「缺块回落」与「RuntimeConfig{} 成员初始化」两条断言红。
	//   ② readMeshletBlock 漏读 assets / 键名打错 → 「显式关门」红（探针值 false 读不到 = 恒 true）。
	//   ③ 加了扁平键双读兼容 → 「顶层 meshlet_enable = false 什么都不发生」红（两个真相源反模式）。
	{  // 成员初始化默认路径（不看 lua 文件的默认值 = 主 agent 验收口径）
		CHECK(RuntimeConfig{}.meshletAssets == true);
	}
	{  // 缺块 = 出厂默认（★18-C 起 = 开）
		TempLua f("rt_meshlet_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletAssets == true);
	}
	{  // 块内键名命中（🔴 探针值 **false** ≠ 默认 true：漏读即恒 true，显式关门会静默失效）
		TempLua f("rt_meshlet_off", R"lua(return { meshlet = { assets = false } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletAssets == false);
	}
	{  // **没有**扁平键的兼容双读（两个真相源 = 本项目反复登记的反模式）
		// 探针取 false：默认已是 true，取 true 就分不出「忽略」与「读到」。
		TempLua f("rt_meshlet_flat", R"lua(return { meshlet_enable = false })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletAssets == true);
	}
}

TEST_CASE("runtime_config：meshlet.mode（债-129 S2 三值选路位）——默认 off、三值命中、"
          "未知值 fail-fast 点名全部合法值、旧两布尔无兼容双读") {
	// 被测 = applyMeshletModeKey（形态先例 = applyMeshletInstrumentKey / applyMeshletCoplanarKey /
	// applyAaKey）。判据来源 = 实读解析器。三值 = off/overlay/switch（原 meshlet.overlay +
	// meshlet.switch 两布尔合并，「同真 ⟹ 运行期裁决、switch 赢」的互斥态**写不出来** = 本键的
	// 存在理由；实测证据 = 合并前基线第四行与 switch 单开逐位同而 warnings 1→2，
	// /tmp/opencode/o3b_premerge_anchors.txt）。🔴 配置串 "switch" ↔ 枚举值 inplaceSwitch
	// （C++ 关键字让位的只有枚举名，用户写的东西不让位）。
	// 变异红线声明：
	//   ① 成员初始化默认翻成 overlay/inplaceSwitch → 「成员初始化」与「缺块」两条断言红
	//      （出厂白付叠加段 +1.59ms / 出厂即取代 = 逐位护栏失效）。
	//   ② 解析器键名打错（mode → mdoe）→ 「三值命中」红（探针值读不到 = 回落 off）。
	//   ③ 未知值回落成默认（删 else 分支）→ 「未知值 fail-fast」红；失效形态 = 选错路而
	//      截图判据全绿（instrument/coplanar 假绿同族）。
	//   ④ 错误文本漏点名某个合法值 → 对应 CHECK 红（报错必须教人怎么改，aa 先例）。
	//   ⑤ 加旧键 overlay/switch 的兼容双读 → 「无兼容双读」红（两个真相源 = 本仓反模式；
	//      迁移安全网是未知键护栏——warn 点名那条在下方债-129 S2 变异红线用例）。
	//   ⑥ "switch" 档错映射到 overlay（else-if 链写串）→ 「三值命中」的 switch 行 + 谓词
	//      同步断言红（谓词是消费侧唯一入口）。
	{  // 成员初始化默认路径（不看 lua 文件）= 出厂不叠加不取代（债-105「不白付」+ P5 护栏）
		CHECK(RuntimeConfig{}.meshletMode == MeshletMode::off);
		CHECK(RuntimeConfig{}.modeIsOverlay() == false);
		CHECK(RuntimeConfig{}.modeIsSwitch() == false);
	}
	{  // 缺块 / 缺键 = 保持默认 off
		TempLua f("rt_mode_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletMode == MeshletMode::off);
		TempLua g("rt_mode_nokey", R"lua(return { meshlet = { assets = true } })lua");
		auto r2 = loadRuntimeConfig(g.pathStr());
		REQUIRE(r2.has_value());
		CHECK(r2->meshletMode == MeshletMode::off);
	}
	{  // 三个合法值各自命中（探针值 ≠ 默认，漏读即红）+ 谓词与枚举同步（谓词是消费侧唯一入口）
		struct Row { const char* lua; MeshletMode want; };
		const Row rows[]{{"off", MeshletMode::off},
			{"overlay", MeshletMode::overlay}, {"switch", MeshletMode::inplaceSwitch}};
		int seen = 0;
		for (const auto& row : rows) {
			const std::string body =
				std::string("return { meshlet = { mode = \"") + row.lua + "\" } }\n";
			TempLua f("rt_mode_val", body);
			auto r = loadRuntimeConfig(f.pathStr());
			REQUIRE(r.has_value());
			CHECK(r->meshletMode == row.want);
			CHECK(r->modeIsOverlay() == (row.want == MeshletMode::overlay));
			CHECK(r->modeIsSwitch() == (row.want == MeshletMode::inplaceSwitch));
			++seen;
		}
		CHECK(seen == 3);  // 前置守卫：三档都真跑了（空列表假绿防线）
	}
	{  // 未知值 = **启动失败**且错误点名键名 + 全部合法值 + 回显写错的值（不静默回落）
		TempLua f("rt_mode_bad", R"lua(return { meshlet = { mode = "both" } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		CHECK_FALSE(r.has_value());
		if (!r) {
			const std::string& msg = r.error().message;
			CHECK(msg.find("meshlet.mode") != std::string::npos);
			CHECK(msg.find("\"off\"") != std::string::npos);      // 错误文本列出合法值
			CHECK(msg.find("\"overlay\"") != std::string::npos);
			CHECK(msg.find("\"switch\"") != std::string::npos);
			CHECK(msg.find("both") != std::string::npos);         // 且回显写错的值本体
		}
		// 大小写敏感 + 空串不合法（shadow mode / instrument 同口径）
		for (const char* bad : {R"lua(return { meshlet = { mode = "Off" } })lua",
		                        R"lua(return { meshlet = { mode = "" } })lua"}) {
			TempLua f("rt_mode_bad2", bad);
			CHECK_FALSE(loadRuntimeConfig(f.pathStr()).has_value());
		}
	}
	{  // 🔴 已知限制（与先例 instrument / coplanar / shadow_lod_fallback 共有，实机对照同形）：
	   // 值写成**非字符串**时 sol 的 optional<string> 读成 nullopt = 走缺键保默认，**不**报错。
	   // （旧布尔时代的「非布尔即失败」严格面随键形态一起退役；未知键护栏管不到**值**类型。）
		TempLua f("rt_mode_bool", R"lua(return { meshlet = { mode = true } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletMode == MeshletMode::off);
	}
	{  // 🔴 旧两布尔键**无兼容双读**（硬切红线）：顶层扁平键与块内旧键都不被解析器读——
	   // 块内旧键的「出声」由未知键护栏负责（变异红线用例在下方 meshlet.overlay warn 那条），
	   // 本条钉的是「就算不喊，旧键也**绝不**被当成新键读」（双读 = 两个真相源）。
		TempLua f("rt_mode_flat", R"lua(return { meshlet_mode = "switch" })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletMode == MeshletMode::off);
		TempLua g("rt_mode_old_overlay_block", R"lua(return { meshlet = { overlay = true } })lua");
		auto r2 = loadRuntimeConfig(g.pathStr());
		REQUIRE(r2.has_value());
		CHECK(r2->meshletMode == MeshletMode::off);  // 旧键**绝不**被映射进新字段
		TempLua h("rt_mode_old_switch_block", R"lua(return { meshlet = { switch = true } })lua");
		auto r3 = loadRuntimeConfig(h.pathStr());
		REQUIRE(r3.has_value());
		CHECK(r3->meshletMode == MeshletMode::off);
	}
	{  // 本档**不**与 assets 焊死：assets=false + mode="overlay" 读到的仍是 overlay
	   // （孤立组合按装配层事实出声，配置层不做隐含耦合 = 债-105 交付物 (1) 的红线）
		TempLua f("rt_mode_orphan",
		          R"lua(return { meshlet = { assets = false, mode = "overlay" } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->modeIsOverlay() == true);
		CHECK(r->meshletAssets == false);
	}
	{  // 同块其余布尔键同享严格面（★1-a1ii-β-1 起整块严格；债-129 S2 起 overlay/switch 两布尔
	   // 并成 mode 三态字符串键，严格面兄弟换成 cull/mdi_shadow）
		TempLua f("rt_mode_strict_siblings", R"lua(return { meshlet = { assets = "on" } })lua");
		CHECK_FALSE(loadRuntimeConfig(f.pathStr()).has_value());
		TempLua g("rt_mode_strict_mdi", R"lua(return { meshlet = { mdi_shadow = 0 } })lua");
		CHECK_FALSE(loadRuntimeConfig(g.pathStr()).has_value());
		TempLua h("rt_mode_strict_cull", R"lua(return { meshlet = { cull = "yes" } })lua");
		CHECK_FALSE(loadRuntimeConfig(h.pathStr()).has_value());
	}
}

TEST_CASE("runtime_config：meshlet.instrument（债-129 S2 三值仪器位）——默认 none、三值命中、"
          "未知值 fail-fast 点名全部合法值、旧两布尔无兼容双读") {
	// 被测 = applyMeshletInstrumentKey（形态先例 applyMeshletCoplanarKey / applyAaKey）。
	// 判据来源 = 实读解析器。三值 = none/offset/tint（原 meshlet.debug + meshlet.tint 两布尔
	// 合并，「同真 ⟹ tint 优先」的让位态**写不出来** = 本键的存在理由）。
	// 变异红线声明：
	//   ① 成员初始化默认翻成 offset/tint → 「成员初始化」与「缺块」两条断言红（出厂白烘仪器变体）。
	//   ② 解析器键名打错（instrument → instrumnt）→ 「三值命中」红（探针值读不到 = 回落 none）。
	//   ③ 未知值回落成默认（删 else 分支）→ 「未知值 fail-fast」红；失效形态 = 仪器臂根本没跑
	//      而截图判据全绿（coplanar 四臂假绿同族）。
	//   ④ 错误文本漏点名某个合法值 → 对应 CHECK 红（报错必须教人怎么改，shadow mode 先例）。
	//   ⑤ 加旧键 debug/tint 的兼容双读 → 「无兼容双读」两条红（两个真相源 = 本仓反模式；
	//      迁移安全网是未知键护栏，不是双读——warn 点名那条见债-129 未知键用例）。
	{  // 成员初始化默认路径（不看 lua 文件）= 出厂不选仪器变体
		CHECK(RuntimeConfig{}.meshletInstrument == MeshletInstrument::none);
		CHECK(RuntimeConfig{}.instrumentIsOffset() == false);
		CHECK(RuntimeConfig{}.instrumentIsTint() == false);
	}
	{  // 缺块 / 缺键 = 保持默认 none
		TempLua f("rt_instr_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletInstrument == MeshletInstrument::none);
		TempLua g("rt_instr_nokey", R"lua(return { meshlet = { assets = true } })lua");
		auto r2 = loadRuntimeConfig(g.pathStr());
		REQUIRE(r2.has_value());
		CHECK(r2->meshletInstrument == MeshletInstrument::none);
	}
	{  // 三个合法值各自命中（探针值 ≠ 默认，漏读即红）+ 谓词与枚举同步（谓词是消费侧唯一入口）
		struct Row { const char* lua; MeshletInstrument want; };
		const Row rows[]{{"none", MeshletInstrument::none},
			{"offset", MeshletInstrument::offset}, {"tint", MeshletInstrument::tint}};
		int seen = 0;
		for (const auto& row : rows) {
			const std::string body =
				std::string("return { meshlet = { instrument = \"") + row.lua + "\" } }\n";
			TempLua f("rt_instr_val", body);
			auto r = loadRuntimeConfig(f.pathStr());
			REQUIRE(r.has_value());
			CHECK(r->meshletInstrument == row.want);
			CHECK(r->instrumentIsOffset() == (row.want == MeshletInstrument::offset));
			CHECK(r->instrumentIsTint() == (row.want == MeshletInstrument::tint));
			++seen;
		}
		CHECK(seen == 3);  // 前置守卫：三档都真跑了（空列表假绿防线）
	}
	{  // 未知值 = **启动失败**且错误点名键名 + 全部合法值 + 回显写错的值（不静默回落）
		TempLua f("rt_instr_bad", R"lua(return { meshlet = { instrument = "debug" } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		CHECK_FALSE(r.has_value());
		if (!r) {
			const std::string& msg = r.error().message;
			CHECK(msg.find("meshlet.instrument") != std::string::npos);
			CHECK(msg.find("\"none\"") != std::string::npos);   // 错误文本列出合法值
			CHECK(msg.find("\"offset\"") != std::string::npos);
			CHECK(msg.find("\"tint\"") != std::string::npos);
			CHECK(msg.find("debug") != std::string::npos);      // 且回显写错的值本体
		}
		// 大小写敏感 + 空串不合法（shadow mode / coplanar 同口径）
		for (const char* bad : {R"lua(return { meshlet = { instrument = "None" } })lua",
		                        R"lua(return { meshlet = { instrument = "" } })lua"}) {
			TempLua f("rt_instr_bad2", bad);
			CHECK_FALSE(loadRuntimeConfig(f.pathStr()).has_value());
		}
	}
	{  // 🔴 已知限制（与先例 coplanar / shadow_lod_fallback 共有，实机对照同形）：值写成**非
	   // 字符串**时 sol 的 optional<string> 读成 nullopt = 走缺键保默认，**不**报错。
		TempLua f("rt_instr_bool", R"lua(return { meshlet = { instrument = true } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletInstrument == MeshletInstrument::none);
	}
	{  // 🔴 旧两布尔键**无兼容双读**（硬切红线）：顶层扁平键与块内旧键都不被解析器读——
	   // 块内旧键的「出声」由未知键护栏负责（变异红线用例在下方 meshlet.debug warn 那条），
	   // 本条钉的是「就算不喊，旧键也**绝不**被当成新键读」（双读 = 两个真相源）。
		TempLua f("rt_instr_flat", R"lua(return { meshlet_instrument = "offset" })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletInstrument == MeshletInstrument::none);
		TempLua g("rt_instr_old_debug_flat", R"lua(return { meshlet_debug = true })lua");
		auto r2 = loadRuntimeConfig(g.pathStr());
		REQUIRE(r2.has_value());
		CHECK(r2->meshletInstrument == MeshletInstrument::none);
	}
}

TEST_CASE("runtime_config：meshlet.shadow_lod（★18-B1 阴影段选级）——默认 7、三态取值、严格整数面") {
	// 判据来源 = 实读 readMeshletBlock / readStrictIntKey。🔴 ★18-C 曾默认 5（2026-10-12 拍板
	// 「默认开 D=5」）；**N10 起默认 7**（2026-10-20 用户拍板转默认，依据 = shadow_proxy_plan.md
	// 数 8 四臂实测：D=7 + 钉段比出厂快 0.121ms、画质判据不劣化；与 assets = true、
	// shadow_proxy = true 成对给默认，拆开转会把 D=7 的画质风险裸露）。
	// −1 / 0 / N≥1 是三条**语义不同**的档（−1 = 关、0 = §18.6 判据 2 的阳性对照 L0 臂、
	// N≥1 = uniform 选级），解析层必须原样透传、不得合并。
	// 🔴 探针一律避开默认值 7（取 4）：默认 = 探针 ⟹ 「读到」与「静默忽略」不可区分。
	// 变异红线声明：
	//   ① 默认值翻成 0 → 「成员初始化」「缺块」「缺键」三条红（出厂即开阳性对照臂）。
	//   ② 解析层把 0 当「关」（写成 `if (v <= 0)` 而非 `< 0`）→ 「0 = L0 臂」红。
	//      🔴 这条最要紧：0 被吞掉 ⟹ §18.6 判据 2 根本跑不出来，而截图与门关逐位同 = 假绿。
	//   ③ 退回宽容的 readFloatKey/readBoolKey 式读法（类型错静默当缺键）→ 「非整数即失败」
	//      四条红（字符串/布尔/小数/越界各一条，小数与越界钉的是"截断"这条独立漂移）。
	//   ④ 加扁平键双读兼容 → 「顶层 meshlet_shadow_lod = 4 什么都不发生」红（两个真相源反模式）。
	{  // 成员初始化默认路径（不看 lua 文件的默认值 = 主 agent 验收口径）
		CHECK(RuntimeConfig{}.meshletShadowLod == 7);
	}
	{  // 缺块 / 缺键 = 出厂默认（N10 起 = 7）
		TempLua f("rt_mlod_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletShadowLod == 7);
		TempLua g("rt_mlod_nokey", R"lua(return { meshlet = { assets = true } })lua");
		auto r2 = loadRuntimeConfig(g.pathStr());
		REQUIRE(r2.has_value());
		CHECK(r2->meshletShadowLod == 7);
	}
	{  // 三态取值原样透传（探针值 4 / 0 / −3 互不相同且 ≠ 默认 7，漏读或错并即红）
		TempLua a("rt_mlod_four", R"lua(return { meshlet = { shadow_lod = 4 } })lua");
		auto r = loadRuntimeConfig(a.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletShadowLod == 4);
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
	{  // **没有**扁平键的兼容双读（探针 4 ≠ 默认 7，否则「忽略」与「读到」不可区分）
		TempLua f("rt_mlod_flat", "return { meshlet_shadow_lod = 4 }\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletShadowLod == 7);
	}
}

TEST_CASE("runtime_config：meshlet.shadow_proxy（N10 钉段表门）——默认 true、严格布尔面、显式 false 生效") {
	// N10 P3 后转默认开（2026-10-20 用户拍板，依据 → shadow_proxy_plan.md 数 8）。严格布尔面
	// 纪律同 meshlet 块全部键（写成字符串/数字 = 启动失败点名键，readStrictBoolKey）。
	// 变异红线：默认翻成 false → 「成员初始化」「缺块」红；退回宽容 readBoolKey → 非布尔两条红。
	{
		CHECK(RuntimeConfig{}.meshletShadowProxy == true);
		TempLua f("rt_mproxy_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletShadowProxy == true);
	}
	{  // 显式 false 生效（门关三脚配方的一半；探针 false ≠ 默认 true）
		TempLua f("rt_mproxy_off", R"lua(return { meshlet = { shadow_proxy = false } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletShadowProxy == false);
	}
	{  // 非布尔 = 启动失败点名键（严格面）
		for (const char* bad : {R"lua(return { meshlet = { shadow_proxy = "true" } })lua",
		                        "return { meshlet = { shadow_proxy = 1 } }\n"}) {
			TempLua f("rt_mproxy_bad", bad);
			CHECK_FALSE(loadRuntimeConfig(f.pathStr()).has_value());
		}
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
	// 本用例是防漂移护栏（docs/archive/gate_lint_plan.md §3-B）：两个方向都断——
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
	CHECK(echo.size() == 33u);            // 钉死门数：加门不更新回显表 → 这里先红
	CHECK(trace.read.size() == 33u);      // 解析器恰好读一遍（重复读 = 记录点放错层）
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

TEST_CASE("diagnostics 未知键（探针子表）：探针表名不算未知键——前缀判据的正向对照；债-129 第三刀下钻后子键走精确成员") {
	// 若判据写成「回显键名全名 == 顶层键」，三张探针表名会全被误报（子表字段的
	// 回显拼写是 `表名.字段`）。本用例钉住「表名 → 前缀命中 → 不喊」。债-129 第三刀：
	// picking_probe 被下钻，present 多出 `picking_probe.frame`（合法子键 = 精确成员，
	// 不喊）。变异红线：下钻段删掉 → present.size() 红；精确成员判据写成前缀 → 本用例
	// 仍绿但「探针子表内拼错」用例（见下方第三刀 diagnostics 嵌套阳性锚点）红。
	TempLua f("dg_probetab", "return { picking_probe = { frame = 5 }, rpc_port = 9999 }\n");
	DiagnosticsKeyTrace trace;
	auto r = loadDiagnosticsConfig(f.pathStr(), &trace);
	REQUIRE(r.has_value());
	REQUIRE(trace.present.size() == 3u);  // picking_probe / picking_probe.frame / rpc_port
	CHECK(containsKey(trace.present, "picking_probe.frame"));
	for (const auto& key : trace.present) {
		// 与 lintUnknownDiagnosticsKeys 同判据（债-129 第三刀口径）：含 `.` = 回显表
		// 精确成员；不含 `.` = 任一回显键名的 `.` 前缀等于它（判据的表 = 回显表本身）
		const bool dotted = key.find('.') != std::string::npos;
		bool known = false;
		for (const auto& e : diagnosticsEchoKeys()) {
			std::string_view ev{e};
			if (dotted ? (ev == key) : (ev.substr(0, ev.find('.')) == key))
				known = true;
		}
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
	// 全量表 = 33 段 `k=v`（空格分隔），抽查首、中、尾与两个特殊格式条目
	CHECK(std::ranges::count(all, '=') == 33);
	CHECK(all.find("no_vvl=false") != std::string::npos);
	CHECK(all.find("cull_replay=cpu") != std::string::npos);
	CHECK(all.find("fault_poll=false") != std::string::npos);  // device fault masked 轮询门
	CHECK(all.find("gpuav_compat=false") != std::string::npos);  // VVL GPU-AV 兼容模式门
	CHECK(all.find("heap_slot_audit=false") != std::string::npos);  // heap 槽内容对账门
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
	// 「默认 = 交付形态（mode=csm、izb=false，2026-10-10 换红线）」由结构体定义承载（shadow_config.cppm）。
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

TEST_CASE("shadow_config：izb 子表形态（on+grid+dump_frame；grid clamp [64,2048]、类型错与扁平布尔 fail-fast）") {
	{  // 扁平布尔 = 报错（硬切 2026-10-17：布尔/表双读 = 「扁平键双读兼容 = 两个真相源」
	   // 红线的活体违反，被债-129 第三刀嵌套键护栏照出后主 agent 裁删）。两向都要钉：
	   // 变异红线：applyIzbKey 的 boolean 分支加回来 → 本块两条红。
		TempLua f("sh_izb_flat0", "return { izb = false }\n");
		auto r = loadShadowConfig(f.pathStr());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().message.find("izb must be a table") != std::string::npos);
		TempLua g("sh_izb_flat1", "return { izb = true }\n");
		CHECK_FALSE(loadShadowConfig(g.pathStr()).has_value());
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

TEST_CASE("shadow_config：csm.slope_bias（斜率深度偏置）——出厂 16、显式值生效、clamp [0,64]、0 不被吞") {
	// 判据来源 = 本批实测（出厂默认 16 的读数 → ShadowCsmTuning::slopeBias 注释）+ 闭式口径
	// 「clamp 不是恒取界、0 是合法档（= 关闭）」。
	// 变异红线（实测形态，逐条跑过）：① 默认值 16.0 改成 15.0 → 「出厂默认」两条红；
	// ② std::clamp 整条删掉 → 上下界两条红；③ 把 `v ? *v : into.slopeBias` 写成真值判据
	// `v && *v ? …`（= 「0 当缺省」）→ 「0 不被吞」红（0 = 关闭档，被读成缺省 = 门关红线从此
	// 关不拢）；④ 键轨迹里的键名改拼写 → 回显覆盖率护栏红（🔴 不能整行删：`trace` 会变成
	// 未用参数被 -Werror=unused-parameter 拦下，拿到的是"编译红"而不是"判据红"，同 skill
	// 「谓词类变异要保留参数使用」）；⑤ 回显表删掉本行 → 覆盖率护栏 + 本用例的 present 断言红。
	const auto load = [](std::string_view body) {
		TempLua f("sh_csm", body);
		return loadShadowConfig(f.pathStr());
	};
	{  // 出厂默认 = 0.0（🔴 这里**刻意写字面量**而不是 `ShadowConfig{}` 符号：本键的出厂值是
	   // 红线锚的取数口径，只钉符号 = 「默认值被人改了」测不出来；其余回落断言仍吃符号。
	   // 为什么是 8 不是 16 → `ShadowCsmTuning::slopeBias` 注释里的定档表：16 那一档靶上
	   // 闭式真实影带存活率 = 0%，等于拿真影子换读数）
		CHECK(ShadowConfig{}.csm.slopeBias == 0.0);
		auto r = load("return {}\n");
		REQUIRE(r.has_value());
		CHECK(r->csm.slopeBias == 0.0);
		TempLua g("sh_csm_empty", "return { csm = { } }\n");  // 空块 = 回落（预留块的老形态）
		auto e = loadShadowConfig(g.pathStr());
		REQUIRE(e.has_value());
		CHECK(e->csm.slopeBias == 0.0);
	}
	{  // 显式给值生效（界内原样 + 上界 clamp）
		CHECK(load("return { csm = { slope_bias = 6.5 } }\n")->csm.slopeBias == 6.5);
		CHECK(load("return { csm = { slope_bias = 64.0 } }\n")->csm.slopeBias == 64.0);
		// ⚠ 出厂值 = 0 之后，**两条曾经有效的断言退化成恒真，已删**（2026-10-19）：
		//   ① 「显式 0 不被当缺省吞掉」——默认就是 0，吞与不吞同值；
		//   ② 「clamp 下界 −1 → 0」——夹到 0 与回落默认 0 同值，结果分不出来。
		//   别把它们"顺手补回来"当成覆盖率：那种断言红了也不指向任何缺陷。
		//   门关（本键 = 0）的真实证据在**消费点**（`test_state_kit` 断言 `depthBiasEnable ==
		//   false`）与截图逐位对拍（skill rgl-verify 的门关口径），不在解析器里。
		CHECK(load("return { csm = { slope_bias = -1 } }\n").has_value());  // 越界不被拒（只夹）
	}
	{  // 反向用例（clamp 上界；下界见上方说明——默认 0 下不可判，已删）：
	   // 越界**夹到界值**——不是被拒（解析仍成功），也不是原样通过
		auto hi = load("return { csm = { slope_bias = 1000 } }\n");
		REQUIRE(hi.has_value());
		CHECK(hi->csm.slopeBias == 64.0);
	}
	{  // csm 存在但不是表 = 报错不静默（"配置写了、什么都没发生"是本项目最怕的失效形态；
	   // 判据本体 = checkTableOrAbsent，csm 自本批起与 pcss/evssm 同吃这道闸门）
		TempLua f("sh_csm_scalar", "return { csm = 5 }\n");
		auto r = loadShadowConfig(f.pathStr());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().message.find("csm must be a table") != std::string::npos);
	}
	{  // 块内拼错的子键：进 present（下钻采集）、不进读集、**不落到字段上**
	   // （批 N6a 时代 csm 无点分成员 ⟹ 不钻，"写了不认识的子键"既不报错也不生效；
	   //  填入第一枚键后 declaresNestedKeys 认它 ⟹ 本用例钉的是"从此会喊"）
		TempLua f("sh_csm_typo", "return { csm = { slope_bia = 5 } }\n");
		ShadowKeyTrace trace;
		auto r = loadShadowConfig(f.pathStr(), &trace);
		REQUIRE(r.has_value());
		CHECK(containsKey(trace.present, "csm.slope_bia"));
		CHECK_FALSE(containsKey(trace.read, "csm.slope_bia"));
		CHECK(r->csm.slopeBias == 0.0);  // 拼错的键没被当成 slope_bias 读进来（回落出厂 0）
	}
}

TEST_CASE("shadow_config：csm.force_cascade（强制选级诊断键）——出厂 -1、0 不被吞、clamp [-1,7]") {
	// 判据来源 = 本键的契约（-1 = 关 / 0..count-1 = 钉级 / 越界只夹不拒），口径与 slope_bias
	// 那条同形。变异红线（逐条对应下面哪一臂）：① 默认值 -1 改成 0 → 「出厂默认」两条红
	// （0 = 钉第 0 级，出厂画面从此不是出厂画面）；② `f ? *f : …` 写成真值判据 `f && *f`
	// → 「0 不被吞」红（钉级 0 被读成关，量具最要紧的「级 0 vs 级 3」差分从此变成「关 vs 级 3」）；
	// ③ clamp 上界
	// 7 改成 64 → 「写 99 夹到 7」红；下界 -1 改成 -64 → 「写 -5 夹到 -1」红；④ clamp 整条
	// 删掉 → 两条越界臂都红；⑤ 键轨迹漏记 / 回显表漏行 → 覆盖率护栏红（键数钉 14 先红）。
	const auto load = [](std::string_view body) {
		TempLua f("sh_fc", body);
		return loadShadowConfig(f.pathStr());
	};
	{  // 出厂默认 = -1（**写字面量**而不是 `ShadowConfig{}` 符号：本键的出厂值是"门关逐位
	   // 不变"红线的取数口径，只钉符号 = 默认值被人改了测不出来）
		CHECK(ShadowConfig{}.csm.forceCascade == -1);
		CHECK(load("return {}\n")->csm.forceCascade == -1);
		TempLua e("sh_fc_empty", "return { csm = { } }\n");  // 空块 = 回落
		CHECK(loadShadowConfig(e.pathStr())->csm.forceCascade == -1);
		TempLua b("sh_fc_noblock", "return { mode = \"csm\" }\n");  // 整块缺失 = 回落
		CHECK(loadShadowConfig(b.pathStr())->csm.forceCascade == -1);
	}
	{  // 显式给值生效；🔴 0 是**合法档**（钉第 0 级），不许被当缺省吞掉
		CHECK(load("return { csm = { force_cascade = 0 } }\n")->csm.forceCascade == 0);
		CHECK(load("return { csm = { force_cascade = 2 } }\n")->csm.forceCascade == 2);
		CHECK(load("return { csm = { force_cascade = 3 } }\n")->csm.forceCascade == 3);
		// 与 slope_bias 同时给：两键各读各的，互不串（同块两键的接线判据）
		CHECK(load("return { csm = { slope_bias = 6.5, force_cascade = 2 } }\n")->csm.slopeBias == 6.5);
		CHECK(load("return { csm = { slope_bias = 6.5, force_cascade = 2 } }\n")->csm.forceCascade == 2);
	}
	{  // 越界**夹到界值**（不是被拒，也不是原样通过）；-5 夹回 -1 = 夹回"关"这一侧
		auto hi = load("return { csm = { force_cascade = 99 } }\n");
		REQUIRE(hi.has_value());
		CHECK(hi->csm.forceCascade == 7);
		auto lo = load("return { csm = { force_cascade = -5 } }\n");
		REQUIRE(lo.has_value());
		CHECK(lo->csm.forceCascade == -1);
		// 夹完仍是整数语义：写 1.7 → 截断为 1（消费端按整数比射程，不四舍五入到 2）
		CHECK(load("return { csm = { force_cascade = 1.7 } }\n")->csm.forceCascade == 1);
	}
	{  // 块内拼错的子键：进 present、不进读集、不落到字段上（同 slope_bias 那条的形态）
		TempLua f("sh_fc_typo", "return { csm = { force_cascad = 2 } }\n");
		ShadowKeyTrace trace;
		auto r = loadShadowConfig(f.pathStr(), &trace);
		REQUIRE(r.has_value());
		CHECK(containsKey(trace.present, "csm.force_cascad"));
		CHECK_FALSE(containsKey(trace.read, "csm.force_cascad"));
		CHECK(r->csm.forceCascade == -1);  // 拼错的键没被当成 force_cascade 读进来
	}
}

TEST_CASE("shadow_config 回显覆盖率护栏：解析器读过的键集合 == 回显表键集合（双向）+ 未知键判据") {
	// diagnostics 同形护栏（docs/archive/gate_lint_plan.md §3-B）：回显表删一行 / 解析器漏记一键 /
	// 两处任一侧改拼写 → set 相等立刻红。变异红线：kShadowEcho 删任一条目 → 本用例红。
	TempLua f("sh_keys", "return { mode = \"csm\", izb = { on = true }, csm = { slope_bias = 16.0, force_cascade = 2 }, pcss = { }, evssm = { } }\n");
	ShadowKeyTrace trace;
	auto r = loadShadowConfig(f.pathStr(), &trace);
	REQUIRE(r.has_value());
	const auto echo = shadowConfigEchoKeys();
	CHECK(echo.size() == 14u);         // 钉死键数：加键不更新回显表 → 这里先红
	CHECK(trace.read.size() == 14u);   // 解析器恰好读一遍（重复读 = 记录点放错层）
	for (const auto& k : echo)
		CHECK(containsKey(trace.read, k));  // 回显有而解析器不读 = 陈旧键
	for (const auto& k : trace.read)
		CHECK(containsKey(echo, k));        // 解析器读而回显不列 = 撒谎行
	// csm 块：顶层表名在 present 里（未知键 lint 的前缀判据认它），字段键 `csm.slope_bias`
	// 既在 present（下钻采集）也在读集（= 回显成员）
	CHECK(containsKey(trace.present, "csm"));
	CHECK(containsKey(trace.present, "csm.slope_bias"));
	CHECK(containsKey(trace.read, "csm.slope_bias"));
	// 同块第二枚键（强制选级诊断键）同形：present + read 都在。漏记一键 → 上面的 size 钉先红；
	// 只漏 present 侧的采集 → 这两条红（"回显有、解析器不读"与"读了、下钻没采"是两种坏法）。
	CHECK(containsKey(trace.present, "csm.force_cascade"));
	CHECK(containsKey(trace.read, "csm.force_cascade"));
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

TEST_CASE("shadow_config 回显 describe：全默认 = 空串；改动只列改动键（info 差集语义）；debug 全量 14 段") {
	const ShadowConfig d;
	CHECK(describeShadowGates(d, d).empty());  // 反向用例：全默认一个都不喊
	ShadowConfig c;
	c.mode = ShadowMode::evssm;
	c.izb = true;  // 默认已关 ⟹ 开回来才是"改动"，才会进 info 差集
	c.evssm.searchRadius = 48.0;
	const std::string s = describeShadowGates(c, d);
	CHECK(s.find("mode=evssm") != std::string::npos);
	CHECK(s.find("izb.on=true") != std::string::npos);
	CHECK(s.find("evssm.search_radius=48") != std::string::npos);
	CHECK(s.find("pcss.sun_degrees") == std::string::npos);  // 未动的键不出现
	const std::string all = describeAllShadowGates(d);
	CHECK(std::ranges::count(all, '=') == 14);  // 全量 = 14 段 k=v（空格分隔）
	CHECK(all.find("mode=csm") != std::string::npos);
	CHECK(all.find("izb.on=false") != std::string::npos);
	CHECK(all.find("csm.slope_bias=0") != std::string::npos);  // 出厂值出可读文本（差集语义下不喊）
	// 诊断键出厂 -1 也在回显里（钉级跑两臂时日志必须看得见那枚键与它的值，否则差分图的归因
	// 只能靠「我记得我写了 --set」——那是回忆不是证据）。
	// 🔴 期望写的是**拼好的文本**而不是数字比较：回显漏挂一行时这里红，而不是"数对不上"。
	CHECK(all.find("csm.force_cascade=-1") != std::string::npos);
	// 差集语义：钉到第 2 级才是"改动"，才会进 info 那一份（门关档不喊 = 出厂日志零噪声）
	ShadowConfig f;
	f.csm.forceCascade = 2;
	CHECK(describeShadowGates(f, d).find("csm.force_cascade=2") != std::string::npos);
	CHECK(describeShadowGates(d, d).find("force_cascade") == std::string::npos);
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

// ═══════════════════════════════════════════════════════════════════════════
// 债-11：scene/runtime/camera 三份 loader 的键轨迹 + 未知键 lint + 覆盖率护栏
// （diagnostics/shadow 已建好的解法回灌；判据形状照 tests 里 diagnostics 的既有
//  覆盖率护栏，不另立第二套。与 diagnostics 既有用例的**一处加强**：那边"warn 测不到
// ——只测判据本体"，本组用 LogTailer 把 warn 本身钉住 = lint 接线被删必须红。）
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/** 三份 loader 的键轨迹护栏共用的覆盖率断言（双向 set 相等 + 恰好读一遍）。 */
template <typename Trace, typename Loader, typename KeysFn>
void expectFullCoverage(Loader&& load, KeysFn&& knownKeys, std::string_view tag) {
	TempLua f(std::string(tag) + "_cov", "return {}\n");
	Trace trace;
	REQUIRE(load(f.pathStr(), &trace).has_value());
	const auto keys = knownKeys();
	CHECK_FALSE(keys.empty());              // 键表为空 = 判据不存在，先钉扫描面
	CHECK(trace.read.size() == keys.size());  // 解析器恰好读一遍（重复读 = 记录点放错层）
	for (const auto& k : keys)
		CHECK(containsKey(trace.read, k));  // 表列了而解析器不读 = 陈旧键
	for (const auto& k : trace.read)
		CHECK(containsKey(keys, k));        // 解析器读了而表不列 = 表撒谎
}

}  // namespace

TEST_CASE("债-11 scene 覆盖率护栏：读过的键集合 == 键表键集合（双向）") {
	// 变异红线：kSceneKeys 删任一条目 → 「陈旧键」方向红；解析器删任一 emplace_back →
	// 「表撒谎」方向红；两处任一侧改拼写 → 两侧同时红。
	expectFullCoverage<SceneKeyTrace>(
		[](std::string_view p, SceneKeyTrace* t) { return loadSceneConfig(p, t); },
		sceneConfigKnownKeys, "sc_cov");
}

TEST_CASE("债-11 runtime 覆盖率护栏：读过的键集合 == 键表键集合（双向）") {
	// 变异红线同 scene 侧（kRuntimeKeys / emplace_back 任一侧动 = 对应方向红）。
	expectFullCoverage<RuntimeKeyTrace>(
		[](std::string_view p, RuntimeKeyTrace* t) { return loadRuntimeConfig(p, t); },
		runtimeConfigKnownKeys, "rt_cov");
}

TEST_CASE("债-11 camera 覆盖率护栏：读过的键集合 == 键表键集合（双向）") {
	// 变异红线同 scene 侧（kCameraKeys / emplace_back 任一侧动 = 对应方向红）。
	expectFullCoverage<CameraKeyTrace>(
		[](std::string_view p, CameraKeyTrace* t) { return loadCameraConfig(p, t); },
		cameraConfigKnownKeys, "cm_cov");
}

TEST_CASE("债-11 scene 未知键阳性锚点：拼错的键**真的打出 warn**（日志增量捕获）；正确键不误报") {
	// 阳性锚点（rgl-test-discipline）：如果 lint 接线从未生效，本用例绿吗？——不：
	// delta 里不会出现 warn 行 → 红。变异红线：lintUnknownSceneKeys 函数体删空 /
	// loadSceneConfig 里的调用删掉 → 前两条 CHECK 红。
	LogTailer log;
	TempLua f("sc_typo", "return { explosure_g5 = 2.0, spin2_g5 = true, spin = false }\n");
	SceneKeyTrace trace;
	auto r = loadSceneConfig(f.pathStr(), &trace);
	REQUIRE(r.has_value());
	const std::string delta = log.delta();
	CHECK(hasUnknownKeyWarn(delta, "scene", "explosure_g5"));
	CHECK(hasUnknownKeyWarn(delta, "scene", "spin2_g5"));
	// 反向用例（常错的 lint = 被静音的 lint）：拼写正确的键不喊（带闭引号精确匹配，
	// 不被 'spin2_g5' 的行前缀命中）
	CHECK(delta.find("未知键 'spin'") == std::string::npos);
	// 解析本身不受影响：正确键照常生效
	CHECK(r->spin == false);
	// 判据本体（present 采集 + 读集边界）同步钉住
	CHECK(trace.present.size() == 3u);
	CHECK_FALSE(containsKey(trace.read, "explosure_g5"));
	CHECK(containsKey(trace.read, "spin"));
}

TEST_CASE("债-11 runtime 未知键阳性锚点：拼错的键**真的打出 warn**；未声明点分前缀的表名不钻只喊顶层（债-129 第三刀口径）") {
	// 变异红线：lintUnknownRuntimeKeys 函数体删空 / loadRuntimeConfig 里的调用删掉 → 红。
	// meshlet2_g5 键表里没有 `meshlet2_g5.` 点分名 ⟹ 不钻（present 不增子键），顶层名照喊。
	LogTailer log;
	TempLua f("rt_typo", "return { velocity_feeld_g5 = true, meshlet2_g5 = { enable = true }, headless = true }\n");
	RuntimeKeyTrace trace;
	auto r = loadRuntimeConfig(f.pathStr(), &trace);
	REQUIRE(r.has_value());
	const std::string delta = log.delta();
	CHECK(hasUnknownKeyWarn(delta, "runtime", "velocity_feeld_g5"));
	CHECK(hasUnknownKeyWarn(delta, "runtime", "meshlet2_g5"));  // 未知**表名**同样喊
	CHECK(delta.find("未知键 'headless'") == std::string::npos);
	CHECK(r->headless == true);  // 正确键照常生效
	CHECK(trace.present.size() == 3u);
}

TEST_CASE("债-11 camera 未知键阳性锚点：拼错的键**真的打出 warn**；keys 表名本身不算未知键") {
	// 变异红线：lintUnknownCameraKeys 函数体删空 / loadCameraConfig 里的调用删掉 → 红。
	// keys/trackball/fps 是 `表名.字段` 键名的表名前缀 → 前缀判据的正向对照（探针表名
	// 同款，diagnostics 的 probetab 用例同形）。债-129 第三刀：两表被下钻，present 各
	// 多一枚合法子键（keys.forward / trackball.sensitivity，精确成员不喊）。
	LogTailer log;
	TempLua f("cm_typo", "return { fovvDegrees_g5 = 60.0, keys = { forward = \"W\" }, trackball = { sensitivity = 0.01 } }\n");
	CameraKeyTrace trace;
	auto r = loadCameraConfig(f.pathStr(), &trace);
	REQUIRE(r.has_value());
	const std::string delta = log.delta();
	CHECK(hasUnknownKeyWarn(delta, "camera", "fovvDegrees_g5"));
	CHECK(delta.find("未知键 'keys'") == std::string::npos);       // 表名 = 前缀命中，不喊
	CHECK(delta.find("未知键 'trackball'") == std::string::npos);  // 同上
	CHECK(trace.present.size() == 5u);  // 顶层 3 + 下钻合法子键 2
	CHECK(containsKey(trace.present, "keys.forward"));
	CHECK(containsKey(trace.present, "trackball.sensitivity"));
}

TEST_CASE("债-11 扫描面非空 + 交付 configs 零 warn：configs/{scene,runtime,camera}.lua 实文件实测") {
	// 护栏静默失效比没有更坏（rules_layers 同款纪律）：若 collectPresentKeys 在某种
	// 排版下扫出零个键，未知键 warn 永远不响 = 假绿。本用例把「扫描面非空」钉在**交付
	// 配置实文件**上（CWD = 仓库根，`just test` 保证；LogTailer 注释同一条 CWD 依赖），
	// 并顺带完成「交付 configs 零 warn」断言（改了交付配置引入拼错键 → 这里红）。
	// 变异红线：collectPresentKeys 函数体删空 → present 空 → 三条 REQUIRE 全红；
	// 交付配置塞任一拼错顶层键 → delta 含 warn → 红。
	struct DeliveryConfig {
		const char* file;
		const char* probeKey;  // 该文件必有的特征顶层键（扫描面内容哨兵）
	};
	static constexpr std::array<DeliveryConfig, 3> configs{{
		{"configs/scene.lua", "entities"},
		{"configs/runtime.lua", "meshlet"},
		{"configs/camera.lua", "keys"},
	}};
	for (const auto& cfg : configs) {
		LogTailer log;
		SceneKeyTrace sceneTrace;
		RuntimeKeyTrace runtimeTrace;
		CameraKeyTrace cameraTrace;
		std::vector<std::string> present;
		if (std::string_view{cfg.file}.ends_with("scene.lua")) {
			auto r = loadSceneConfig(cfg.file, &sceneTrace);
			REQUIRE(r.has_value());
			present = sceneTrace.present;
		} else if (std::string_view{cfg.file}.ends_with("runtime.lua")) {
			auto r = loadRuntimeConfig(cfg.file, &runtimeTrace);
			REQUIRE(r.has_value());
			present = runtimeTrace.present;
		} else {
			auto r = loadCameraConfig(cfg.file, &cameraTrace);
			REQUIRE(r.has_value());
			present = cameraTrace.present;
		}
		// ① 扫描面非空（假绿守卫）+ 特征键在场（扫到了**对的东西**，不只是"扫到了什么"）
		CHECK_FALSE(present.empty());
		CHECK(containsKey(present, cfg.probeKey));
		// ② 交付配置上未知键 warn **零条**（warn 走日志，这里读的就是日志增量本体）
		CHECK(log.delta().find("未知键") == std::string::npos);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// 债-129 第三刀：嵌套未知键护栏（拍板 A「meshlet.assets 改名」的硬前置，旧名按拍板原文引）
// collectPresentKeys 对**键表声明了 `块.` 点分前缀**的块下钻一层记 `块.子键`；
// lint 对点分名走键表精确成员、无点名保持顶层前缀语义。五个 loader 同治。
// 🔴 变异红线（整组）：① 下钻段注释掉 → 五条阳性锚点全红；② 前缀条件去掉（无条件钻）
// → 「开放 map 不钻」用例红（deferred_styles.pbr 被喊）；③ 精确成员判据弱化回前缀 → 五条
// 阳性锚点全红（meshlet.ovrelay 会被 meshlet.mode 之外的同块键前缀命中而漏喊）。
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("债-129 runtime 嵌套阳性锚点：meshlet 块内拼错 → warn 点名 meshlet.ovrelay 全名；合法嵌套键零 warn") {
	// 断言钉**完整点分名**（hasUnknownKeyWarn 带闭引号）——只钉 "ovrelay" 子串会被
	// 「只报了顶层块名 meshlet」假通过。反向用例先跑：合法 meshlet.mode 零 warn。
	// 🔴 债-129 S2 起 `overlay = true` 本身已是**旧键**（合并进 mode）——本用例的合法嵌套键
	// 换成 mode = "overlay"，拼错探针 ovrelay 同形保留（它验的是护栏的下钻+精确成员判据，
	// 与键名本体无关；旧键点名那条红线在下方 S2 变异用例）。
	LogTailer log;
	{  // 反向：合法嵌套键不得产生任何未知键 warn
		TempLua ok("rt_nested_ok", "return { meshlet = { mode = \"overlay\" } }\n");
		auto r = loadRuntimeConfig(ok.pathStr(), nullptr);
		REQUIRE(r.has_value());
		CHECK(r->modeIsOverlay());  // 键真的被解析（否则"不 warn"是假绿）
		CHECK(log.delta().find("未知键") == std::string::npos);
	}
	{  // 阳性：块内拼错点名全名
		TempLua f("rt_nested_typo",
		          "return { meshlet = { ovrelay = true, mode = \"overlay\" } }\n");
		RuntimeKeyTrace trace;
		auto r = loadRuntimeConfig(f.pathStr(), &trace);
		REQUIRE(r.has_value());
		CHECK(hasUnknownKeyWarn(log.delta(), "runtime", "meshlet.ovrelay"));
		CHECK(log.delta().find("未知键 'meshlet.mode'") == std::string::npos);  // 合法子键不喊
		CHECK(containsKey(trace.present, "meshlet.ovrelay"));  // 判据本体：下钻采集到点分名
		CHECK(r->modeIsOverlay());  // lint 不改解析：同块合法键照常生效
	}
}

TEST_CASE("债-129 S2 变异红线：残留旧键 meshlet.debug/tint/overlay/switch → 启动 warn 点名（硬切护栏）") {
	// 🔴 本用例是「instrument + mode 两次合并 = 硬切、无旧键双读兼容」的**迁移安全网本体**
	// （S2 段验收原文：配置里留一枚 meshlet.debug = true → 启动 warn 点名它；债-129 S2 第二件
	// 追加 overlay/switch 两枚——主 agent 合并前基线的第四行「overlay=true switch=true」在
	// 新配置面**写不出来**，残留旧键必须出声而不是静默消失）。护栏为本次合并背书的证明。
	// 变异红线声明：
	//   ① 把 kRuntimeKeys 里 `meshlet.instrument` 改回 `meshlet.debug`（= 偷偷恢复旧键）→
	//      第一条 warn 断言红（未知键判据不再喊它 = 双读的静默形态复活）。
	//   ② 解析器加旧键双读（debug → instrument / overlay → mode 映射）→ 「保持默认」红。
	//   ③ 未知键 lint 下钻射程缩回顶层（declaresNestedKeys 失效）→ present 不含点分名 →
	//      containsKey 红（债-129 第三刀护栏同锚）。
	//   ④ 🔴 把 kRuntimeKeys 里 `meshlet.mode` 改回 `meshlet.overlay`（本刀点名的变异）→
	//      阳性③ 的 warn 断言红（残留 overlay = true 静默消失 = 硬切红线破防）。
	// 反向（常错的 lint = 被静音的 lint）：新键出厂写法**零 warn**，
	// 且键真的被解析（「不 warn」不是漏扫的假绿）。
	// ⚠ LogTailer::delta() = 自**构造**起的增量 ⟹ 每块各立一个（共用一个时反向块会读到
	// 阳性块的 warn，先跑顺序也救不了「谁先谁进账」的语义）。
	{  // 阳性①：块内残留 debug = true → warn 点名 'meshlet.debug' 全名，且**不被**当新键读
		LogTailer log;
		TempLua f("rt_s2_oldkey_debug",
			R"lua(return { meshlet = { mode = "overlay", debug = true } })lua");
		RuntimeKeyTrace trace;
		auto r = loadRuntimeConfig(f.pathStr(), &trace);
		REQUIRE(r.has_value());
		CHECK(hasUnknownKeyWarn(log.delta(), "runtime", "meshlet.debug"));
		CHECK(containsKey(trace.present, "meshlet.debug"));  // 判据本体：下钻采集到点分名
		CHECK(r->meshletInstrument == MeshletInstrument::none);  // 旧键**绝不**被映射进新字段
		CHECK(r->modeIsOverlay());  // lint 不改解析：同块合法键照常生效
	}
	{  // 阳性②：tint 同形
		LogTailer log;
		TempLua f("rt_s2_oldkey_tint",
			R"lua(return { meshlet = { mode = "overlay", tint = true } })lua");
		RuntimeKeyTrace trace;
		auto r = loadRuntimeConfig(f.pathStr(), &trace);
		REQUIRE(r.has_value());
		CHECK(hasUnknownKeyWarn(log.delta(), "runtime", "meshlet.tint"));
		CHECK(containsKey(trace.present, "meshlet.tint"));
		CHECK(r->meshletInstrument == MeshletInstrument::none);
	}
	{  // 阳性③（债-129 S2 第二件）：块内残留 overlay = true → warn 点名 'meshlet.overlay'
	   // 全名，且**不被**当 mode 读（mode 保持 off = 「同真写不出来」的机器形态半边）
		LogTailer log;
		TempLua f("rt_s2_oldkey_overlay",
			R"lua(return { meshlet = { overlay = true } })lua");
		RuntimeKeyTrace trace;
		auto r = loadRuntimeConfig(f.pathStr(), &trace);
		REQUIRE(r.has_value());
		CHECK(hasUnknownKeyWarn(log.delta(), "runtime", "meshlet.overlay"));
		CHECK(containsKey(trace.present, "meshlet.overlay"));
		CHECK(r->meshletMode == MeshletMode::off);  // 旧键**绝不**被映射进新字段
	}
	{  // 阳性④：switch 同形（另一半：旧「同真」组合如今只剩一条 warn 通道点名第一个旧键，
	   // 第二枚旧键同样被点名——两枚都不被读 = 互斥态写不出来）
		LogTailer log;
		TempLua f("rt_s2_oldkey_switch",
			R"lua(return { meshlet = { switch = true } })lua");
		RuntimeKeyTrace trace;
		auto r = loadRuntimeConfig(f.pathStr(), &trace);
		REQUIRE(r.has_value());
		CHECK(hasUnknownKeyWarn(log.delta(), "runtime", "meshlet.switch"));
		CHECK(containsKey(trace.present, "meshlet.switch"));
		CHECK(r->meshletMode == MeshletMode::off);
	}
	{  // 反向：出厂写法 mode = "off" + instrument = "none" 零未知键 warn，且键真的被解析
		LogTailer log;
		TempLua ok("rt_s2_newkey_ok",
			R"lua(return { meshlet = { mode = "off", instrument = "none" } })lua");
		auto r = loadRuntimeConfig(ok.pathStr(), nullptr);
		REQUIRE(r.has_value());
		CHECK(r->meshletInstrument == MeshletInstrument::none);
		CHECK(r->meshletMode == MeshletMode::off);
		CHECK(log.delta().find("未知键") == std::string::npos);
	}
}

TEST_CASE("债-129 camera 嵌套阳性锚点：keys 块内拼错 → warn 点名 keys.forwarwd 全名；合法嵌套键零 warn") {
	LogTailer log;
	{  // 反向：trackball.sensitivity 合法 → 零 warn 且生效
		TempLua ok("cm_nested_ok", "return { trackball = { sensitivity = 0.02 } }\n");
		auto r = loadCameraConfig(ok.pathStr(), nullptr);
		REQUIRE(r.has_value());
		expectNear(r->camera.trackballSensitivity, 0.02, 0.0, 1e-6);
		CHECK(log.delta().find("未知键") == std::string::npos);
	}
	{  // 阳性：keys 块内拼错点名全名
		TempLua f("cm_nested_typo", "return { keys = { forwarwd = \"W\", forward = \"W\" } }\n");
		CameraKeyTrace trace;
		auto r = loadCameraConfig(f.pathStr(), &trace);
		REQUIRE(r.has_value());
		CHECK(hasUnknownKeyWarn(log.delta(), "camera", "keys.forwarwd"));
		CHECK(log.delta().find("未知键 'keys.forward'") == std::string::npos);
		CHECK(containsKey(trace.present, "keys.forwarwd"));
	}
}

TEST_CASE("债-129 diagnostics 嵌套阳性锚点：探针子表拼错 → warn 点名 picking_probe.frmae 全名；合法嵌套键零 warn") {
	LogTailer log;
	{  // 反向：picking_probe.frame 合法 → 零 warn 且生效
		TempLua ok("dg_nested_ok", "return { picking_probe = { frame = 7 } }\n");
		auto r = loadDiagnosticsConfig(ok.pathStr(), nullptr);
		REQUIRE(r.has_value());
		CHECK(r->pickingProbeFrame == 7u);
		CHECK(log.delta().find("未知键") == std::string::npos);
	}
	{  // 阳性：探针子表内拼错点名全名
		TempLua f("dg_nested_typo", "return { picking_probe = { frmae = 5, frame = 7 } }\n");
		DiagnosticsKeyTrace trace;
		auto r = loadDiagnosticsConfig(f.pathStr(), &trace);
		REQUIRE(r.has_value());
		CHECK(hasUnknownKeyWarn(log.delta(), "diagnostics", "picking_probe.frmae"));
		CHECK(log.delta().find("未知键 'picking_probe.frame'") == std::string::npos);
		CHECK(containsKey(trace.present, "picking_probe.frmae"));
	}
}

TEST_CASE("债-129 scene 嵌套阳性锚点：environment 块内拼错 → warn 点名 environment.intensy 全名；合法嵌套键零 warn") {
	LogTailer log;
	{  // 反向：environment.intensity 合法 → 零 warn 且生效
		TempLua ok("sc_nested_ok", "return { environment = { intensity = 2.0 } }\n");
		auto r = loadSceneConfig(ok.pathStr(), nullptr);
		REQUIRE(r.has_value());
		expectNear(r->environment.intensity, 2.0, 0.0, 1e-6);
		CHECK(log.delta().find("未知键") == std::string::npos);
	}
	{  // 阳性：environment 块内拼错点名全名
		TempLua f("sc_nested_typo", "return { environment = { intensy = 1.0, intensity = 2.0 } }\n");
		SceneKeyTrace trace;
		auto r = loadSceneConfig(f.pathStr(), &trace);
		REQUIRE(r.has_value());
		CHECK(hasUnknownKeyWarn(log.delta(), "scene", "environment.intensy"));
		CHECK(log.delta().find("未知键 'environment.intensity'") == std::string::npos);
		CHECK(containsKey(trace.present, "environment.intensy"));
	}
}

TEST_CASE("债-129 shadow 嵌套阳性锚点：pcss 块内拼错 → warn 点名 pcss.sun_degreess 全名；合法嵌套键零 warn") {
	LogTailer log;
	{  // 反向：pcss.sun_degrees 合法 → 零 warn 且生效
		TempLua ok("sh_nested_ok", "return { pcss = { sun_degrees = 0.7 } }\n");
		auto r = loadShadowConfig(ok.pathStr(), nullptr);
		REQUIRE(r.has_value());
		expectNear(r->pcss.sunDegrees, 0.7, 0.0, 1e-6);
		CHECK(log.delta().find("未知键") == std::string::npos);
	}
	{  // 阳性：pcss 块内拼错点名全名
		TempLua f("sh_nested_typo", "return { pcss = { sun_degreess = 1.0, sun_degrees = 0.7 } }\n");
		ShadowKeyTrace trace;
		auto r = loadShadowConfig(f.pathStr(), &trace);
		REQUIRE(r.has_value());
		CHECK(hasUnknownKeyWarn(log.delta(), "shadow", "pcss.sun_degreess"));
		CHECK(log.delta().find("未知键 'pcss.sun_degrees'") == std::string::npos);
		CHECK(containsKey(trace.present, "pcss.sun_degreess"));
	}
}

TEST_CASE("债-129 开放 map 不钻（前缀条件的判据本体）：scene deferred_styles 用户自起子键**不得** warn") {
	// 🔴 本用例 = declaresNestedKeys 前缀条件存在的理由：deferred_styles 的键表条目是
	// **裸表名**（无 `deferred_styles.` 点分名）⟹ 不钻。变异红线：前缀条件去掉（无条件钻）
	// → deferred_styles.pbr / deferred_styles.my_style_z1 全被喊 → 红。
	// （原 styles 表是本判据的第二个样本，已随刀②.2 删除；scene.lua 里剩下的开放 map 只有
	//  deferred_styles 一枚——样本少一枚，判据本体不变。）
	LogTailer log;
	TempLua f("sc_openmap",
		"return { deferred_styles = { pbr = \"deferred_geometry\", "
		"my_style_z1 = \"deferred_geometry_unlit\" } }\n");
	SceneKeyTrace trace;
	auto r = loadSceneConfig(f.pathStr(), &trace);
	REQUIRE(r.has_value());
	// 零 warn 断言在**全 delta** 上（不只钉"没喊 deferred_styles.pbr"——任何一条未知键 warn 都算违反红线）
	CHECK(log.delta().find("未知键") == std::string::npos);
	// 解析照常：开放 map 的子键真的被吃进映射（否则"不 warn"是假绿）
	CHECK(r->deferredStyleMap.size() == 2u);
	CHECK(r->deferredStyleMap.at("my_style_z1") == "deferred_geometry_unlit");
	// 判据本体：present 里只有裸表名，没有任何 deferred_styles.* 点分名
	CHECK(containsKey(trace.present, "deferred_styles"));
	CHECK_FALSE(containsKey(trace.present, "deferred_styles.pbr"));
	CHECK_FALSE(containsKey(trace.present, "deferred_styles.my_style_z1"));
}

}  // namespace RGL
