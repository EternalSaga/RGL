module;
/**
 * test_ssr_params —— Phase 15 批 15.1 收尾：SSR 参数子区（kSceneDataSsrParamsOffset
 * @722336，vec4）的字节布局契约 + fillSsrParams 填值契约（纯 CPU、无设备）。
 *
 * ## 钉什么
 *
 * (a) **布局**：按 vkrenderer_impl::writeSceneStaging 的 memcpy 口径把 ssrParams 写进
 *     @722336，再按 **shader 侧提取链**（scene.slang readSsrParams 的 Load4 字面量）读回，
 *     断言逐位还原。抓的是**偏移错 / 步长错**。
 * (b) **填值**：x = SceneBounds.radius（逐位，float 直传无换算）；yzw = 0（**center 不
 *     上传**——锚点只有"多大"没有"在哪"，center 若混进来 = 第二真相源）；空场景
 *     radius=0 **原样透传**（兜底住在消费端 shader，CPU 不做 clamp 的第二口径）。
 *
 * ## 两源结构（先例 = test_ssr_viewproj，理由在那不重复）
 *
 * 写入侧偏移 = C++ 常量 kSceneDataSsrParamsOffset，**读回侧偏移 = 字面量 722336u**
 * （手抄 scene.slang 的 Load4）。编译期另有 static_assert 双保险（C++ 常量 == 本文件手抄值）。
 *
 * ⚠ **本测试钉不住 shader 那一侧**（措辞纪律，别让注释把它吹成"两侧钉死"）：
 * `kSlangSsrParamsLiteral` 是**本文件手抄的 C++ 常量**，不是从 scene.slang 提取的。
 * 若有人只改 scene.slang 里的 `Load4(722336u)`，本测试与全部 static_assert **都不红**
 * ——它们只证明"C++ 常量 == 本文件手抄值"，与 shader 实际写了什么无关。所以变异①的
 * 运行时形态是"读回偏移与写入偏移不一致"，**不是**"shader 字面量单侧漂"。
 * 真正端到端抓住它的只有 `just ssr-check`（2026-10-10 实测：把 fillSsrParams 的锚点错填
 * 成 center.y ⟹ 靶覆盖率 97.2%→2.6%、判据 A 红 ⟹ 这条链路端到端有牙）。
 * 机器对账 shader 字面量 ↔ C++ 常量 = 尚未落地的护栏，缺口账见 phase15_plan §3.2。
 *
 * ## 变异红线声明（哪个变异让我红）
 *
 *   ① kSceneDataSsrParamsOffset 改 +16 → **编译期红**（scene_data.cppm 与本文件的
 *      static_assert 双钉炸编译）；把本文件读回字面量改 722352u → (a) 红（读回全 0），
 *      它抓的是"读写偏移不一致"；⚠ 单改 scene.slang 的字面量**不在此覆盖内**（见上）。
 *   ② fillSsrParams 把 center.x 填进 y → (b)「yzw = 0」红。
 *   ③ fillSsrParams 填 `max(radius, 某下限)`（CPU 侧兜底）→ (b)「空场景原样透传」红。
 *   ④ writeSceneStaging 用错常量（如误写 kSceneDataSsrOffset）→ 本测试**不红**，与
 *      test_ssr_viewproj 同款已知覆盖缺口（单测复刻 memcpy 而非调用它）；端到端护栏 =
 *      just ssr-check（参数错 ⟹ 倒影位置/覆盖率现形）+ 红线 A。
 */
#include <doctest/doctest.h>
#include <cstring>
#include <glm/glm.hpp>

export module test_ssr_params;

import rlstd;
import render_list;      // fillSsrParams（可测接缝，fillSsrViewProj 同款先例）
import frame_draw_list;  // FrameDrawList / SceneFrameData.ssrParams
import scene_context;    // SceneBounds
import scene_data;       // kSceneDataSsrParamsOffset / kSceneDataSsrBytes / kSceneDataBytes

namespace RGL {
namespace {

/// scene.slang readSsrParams 的字面量基址（两源结构的 shader 侧镜像，勿改成常量引用）。
constexpr uint32_t kSlangSsrParamsLiteral = 722336u;

// 编译期双保险：C++ 常量与 shader 字面量同钉（变异①在此炸）；参数子区紧贴 mat4 之后、
// 恰盖到 SSR 区尾 = kSceneDataBytes 尾（SSR 区仍是最后一个尾区，无重叠无空洞）。
static_assert(kSceneDataSsrParamsOffset == kSlangSsrParamsLiteral,
	"scene.slang readSsrParams literal drifted from kSceneDataSsrParamsOffset");
static_assert(kSceneDataSsrParamsOffset == kSceneDataSsrOffset + 64u,
	"params sub-region must sit right after the ssrViewProj mat4");
static_assert(kSceneDataSsrParamsOffset + sizeof(glm::vec4) == kSceneDataBytes,
	"params sub-region must end exactly at the scene data buffer tail");
static_assert(kSceneDataSsrOffset + kSceneDataSsrBytes == kSceneDataBytes,
	"SSR region must be the last tail region of the scene data buffer");

/// shader `sceneData.Load4(byteOffset)` 等价形态（test_ssr_viewproj 同款：真 memcpy）。
glm::vec4 load4(const std::vector<std::byte>& buf, uint32_t byteOffset) {
	glm::vec4 v;
	std::memcpy(&v, buf.data() + byteOffset, sizeof(v));
	return v;
}

}  // namespace

TEST_CASE("SSR 参数子区布局：C++ 常量写、字面量 722336 读回逐位相同（两源结构）") {
	// 非退化输入：radius 取靶实测值量级；center **非零且各分量不同**——它不该出现在
	// 任何字节里（center 混进上传区 = 第二真相源，变异②的判据前提）。
	SceneBounds bounds;
	bounds.center = glm::vec3(1.5f, -2.5f, 3.5f);
	bounds.radius = 31.064859f;

	FrameDrawList list;
	fillSsrParams(list, bounds);

	// 阳性锚点：x = radius 逐位（float 直传），yzw = 0（center 不上传）。
	CHECK(list.sceneData.ssrParams.x == bounds.radius);
	CHECK(list.sceneData.ssrParams.y == 0.0f);
	CHECK(list.sceneData.ssrParams.z == 0.0f);
	CHECK(list.sceneData.ssrParams.w == 0.0f);

	// staging 写入：vkrenderer_impl::writeSceneStaging 同款 memcpy（偏移 = C++ 常量）。
	// 尾部 +16B 垫 = 变异①运行时形态（读回字面量 +16 单侧漂）的读取范围，避免越界 UB。
	std::vector<std::byte> stage(kSceneDataSsrParamsOffset + 2 * sizeof(glm::vec4), std::byte{0});
	std::memcpy(stage.data() + kSceneDataSsrParamsOffset, &list.sceneData.ssrParams,
		sizeof(glm::vec4));

	// shader 侧读回：字面量（两源结构）。
	const glm::vec4 v = load4(stage, kSlangSsrParamsLiteral);
	CHECK(v == list.sceneData.ssrParams);  // 提取链恒等（步长/偏移错在此红）
	CHECK(v.x != 0.0f);                    // 缓冲没读成全 0（变异①运行时形态的判据前提）
}

TEST_CASE("fillSsrParams：空场景 radius=0 原样透传（兜底住 shader，CPU 不做第二真相源）") {
	FrameDrawList list;
	fillSsrParams(list, SceneBounds{});
	CHECK(list.sceneData.ssrParams == glm::vec4(0.0f));  // 变异③（CPU 侧 clamp）在此红
}

}  // namespace RGL
