module;
/**
 * test_ssr_params —— Phase 15 批 15.1 参数面：SSR 区两个 vec4 子区的字节布局契约
 * + fillSsrParams 填值契约（纯 CPU、无设备）。
 *
 * 批 15.1 参数面把 ssrParams 的 yzw 从「恒 0 预留」换成三个可调系数，并新增 ssrGate 子区
 * （@723792，两个阈值门控系数）。**这是有意的契约变更**：原「yzw = 0」断言不是被删掉，
 * 而是改成「yzw = 配置系数」——预留位被正式占用后，"这里不该有别的值"这句话由
 * 「等于配置里的那个数」接替（更强的判据：0 只排除一个值，系数逐位钉住四个）。
 *
 * ## 钉什么
 *
 * (a) **布局**：按 vkrenderer_impl::writeSceneStaging 的 memcpy 口径把两个 vec4 写进
 *     @723776 / @723792，再按 **shader 侧提取链**（scene.slang readSsrParams / readSsrGate
 *     的 Load4 字面量）读回，断言逐位还原。抓的是**偏移错 / 步长错 / 两子区重叠**。
 * (b) **填值**：ssrParams = (sceneRadius, maxDistanceRadii, thicknessPx, biasPx)、
 *     ssrGate = (roughFull, roughCutoff, blurScale, 0)（z = blurScale 批 15.3B 占用），
 *     全部逐位。三条子判据：
 *     - **系数与 bounds 无关**：空场景（radius=0）时系数仍是配置值——系数若被 bounds
 *       污染（或反过来 center 混进上传区）在此红。
 *     - **center 不上传**：测试把 center 与 tuning 各分量取成互不相同的非零值，任何
 *       串位都会撞到别的断言（锚点只有"多大"没有"在哪"）。
 *     - **CPU 不做 clamp**：空场景 radius=0 原样透传（兜底住在消费端 shader）。
 * (c) **出厂默认值 = 批 15.1 那五个 shader 常量的原值**：红线 A（默认配置逐位不变）的
 *     前提。默认值唯一出处 = SsrTuning 成员初始化器，本测试把它钉成字面量。
 *
 * ## 两源结构（先例 = test_ssr_viewproj，理由在那不重复）
 *
 * 写入侧偏移 = C++ 常量 kSceneDataSsrParamsOffset / kSceneDataSsrGateOffset，
 * **读回侧偏移 = 字面量 723776u / 723792u**（手抄 scene.slang 的两个 Load4）。
 * 编译期另有 static_assert 双保险（C++ 常量 == 本文件手抄值）。
 *
 * ⚠ **本测试钉不住 shader 那一侧**（措辞纪律，别让注释把它吹成"两侧钉死"）：
 * `kSlangSsrParamsLiteral` / `kSlangSsrGateLiteral` 是**本文件手抄的 C++ 常量**，不是从
 * scene.slang 提取的。若有人只改 scene.slang 里的 `Load4(723776u)`，本测试与全部
 * static_assert **都不红**——它们只证明"C++ 常量 == 本文件手抄值"，与 shader 实际写了
 * 什么无关。所以变异①的运行时形态是"读回偏移与写入偏移不一致"，**不是**"shader 字面量
 * 单侧漂"。真正端到端抓住它的只有 `just ssr-check`（2026-10-10 实测：把 fillSsrParams 的
 * 锚点错填成 center.y ⟹ 靶覆盖率 97.2%→2.6%、判据 A 红 ⟹ 这条链路端到端有牙）。
 * 机器对账 shader 字面量 ↔ C++ 常量 = 尚未落地的护栏，缺口账见 phase15_plan §3.2。
 *
 * ## 变异红线声明（哪个变异让我红）
 *
 *   ① kSceneDataSsrParamsOffset / kSceneDataSsrGateOffset 改 +16 → **编译期红**（本文件与
 *      scene_data.cppm 的 static_assert 双钉炸编译）；把本文件读回字面量改 723792u →
 *      (a) 红（读回全 0）；只改 gate 字面量 → gate 读回全 0 红。⚠ 单改 scene.slang 的字
 *      面量**不在此覆盖内**（见上）。
 *   ② fillSsrParams 把 center.x 填进 y / 把系数填错槽（如 thickness↔bias 互换）→ (b) 红
 *      （测试里 center 与四个系数互不相同，串位必撞）。
 *   ③ fillSsrParams 填 `max(radius, 某下限)`（CPU 侧兜底）→ (b)「空场景原样透传」红。
 *   ④ fillSsrParams 漏填 gate（回到批 15.1 的恒 0）→ (b)「gate = 配置系数」红。
 *   ⑤ SsrTuning 默认值被改（哪怕只改一位）→ (c) 红 ⟹ 同时是红线 A 的编译前预警。
 *   ⑥ 两子区重叠（gate 偏移写成 params 的值）→ 编译期 static_assert 红（gate = params+16）；
 *      运行时形态 = 两个 load4 读到同一份字节 → (a) 的「两子区读回互不相等」红。
 *   ⑦ writeSceneStaging 用错常量（如误写 kSceneDataSsrOffset）→ 本测试**不红**，与
 *      test_ssr_viewproj 同款已知覆盖缺口（单测复刻 memcpy 而非调用它）；端到端护栏 =
 *      just ssr-check（参数错 ⟹ 倒影位置/覆盖率现形）+ 红线 A。
 *
 * 配置解析侧（缺块回落 / 逐键覆盖 / 类型不符）不在本文件——那是 loader 契约，
 * 见 test_scene_config.cppm 的 ssr 块 roundtrip 用例。
 */
#include <doctest/doctest.h>
#include <cstring>
#include <glm/glm.hpp>

export module test_ssr_params;

import rlstd;
import render_list;      // fillSsrParams（可测接缝，fillSsrViewProj 同款先例）
import frame_draw_list;  // FrameDrawList / SceneFrameData.ssrParams / ssrGate
import scene_context;    // SceneBounds
import runtime_config;   // SsrTuning（默认值唯一出处）
import scene_data;       // kSceneDataSsr*Offset / kSceneDataSsrBytes / kSceneDataBytes

namespace RGL {
namespace {

/// scene.slang readSsrParams 的字面量基址（两源结构的 shader 侧镜像，勿改成常量引用）。
constexpr uint32_t kSlangSsrParamsLiteral = 723776u;
/// scene.slang readSsrGate 的字面量基址（同上）。
constexpr uint32_t kSlangSsrGateLiteral = 723792u;

// 编译期双保险：C++ 常量与两个 shader 字面量同钉（变异①在此炸）。
static_assert(kSceneDataSsrParamsOffset == kSlangSsrParamsLiteral,
	"scene.slang readSsrParams literal drifted from kSceneDataSsrParamsOffset");
static_assert(kSceneDataSsrGateOffset == kSlangSsrGateLiteral,
	"scene.slang readSsrGate literal drifted from kSceneDataSsrGateOffset");
// 子区排布：params 紧贴 ssrViewProj mat4 之后、gate 紧贴 params 之后，gate 恰盖到
// 湿面区起点（批 15.2 子步 A 在 SSR 之后追加湿面区 ⟹「最后尾区」的旧断言改钉相邻关系，
// 无重叠无空洞；湿面区自身的相邻与末尾断言 = tests/test_wet_params）。
static_assert(kSceneDataSsrParamsOffset == kSceneDataSsrOffset + 64u,
	"params sub-region must sit right after the ssrViewProj mat4");
static_assert(kSceneDataSsrGateOffset == kSceneDataSsrParamsOffset + 16u,
	"gate sub-region must sit right after ssrParams (no overlap, no hole)");
// 批 L（§9.7）后相邻不变式 = 「下一区基址 = alignUp(前区尾, 256)」（无重叠 + 间隙 < 256）。
static_assert(alignUp(kSceneDataSsrGateOffset + sizeof(glm::vec4), kSceneRegionAlign) ==
		kSceneDataWetOffset,
	"wet region must start at the 256-aligned end of the ssrGate sub-region");
static_assert(alignUp(kSceneDataSsrOffset + kSceneDataSsrBytes, kSceneRegionAlign) ==
		kSceneDataWetOffset,
	"SSR region must sit right before the wet tail region (batch 15.2A appended after it)");
// 区宽 = 16B 矩阵 + 16B 正向矩阵 + 16B params + 16B gate（参数面由 80 扩到 96）。
static_assert(kSceneDataSsrBytes == 96u, "SSR region width drifted (mat4+mat4+vec4+vec4)");

/// shader `sceneData.Load4(byteOffset)` 等价形态（test_ssr_viewproj 同款：真 memcpy）。
glm::vec4 load4(const std::vector<std::byte>& buf, uint32_t byteOffset) {
	glm::vec4 v;
	std::memcpy(&v, buf.data() + byteOffset, sizeof(v));
	return v;
}

/**
 * 非退化 tuning：四个系数**互不相同且都不等于**下面 center 的任何一个分量。
 *
 * 为什么这么挑：变异②（串位）要求"填错槽"必须改变可观察字节。若系数之间或与 center 有
 * 相等者，swap 两个槽可能读回同一个数（假绿）。0.11/0.22/0.33/0.44 与 1.5/-2.5/3.5
 * 两两不等，且都不是 0（0 会让"漏填"和"填了 0"不可区分）。
 */
SsrTuning distinctTuning() {
	SsrTuning t;
	t.maxDistanceRadii = 4.44f;
	t.thicknessPx = 0.11f;
	t.biasPx = 0.22f;
	t.roughFull = 0.33f;
	t.roughCutoff = 0.55f;
	t.blurScale = 0.77f;  // 批 15.3B 第六枚（同样两两不等、非零、不等于 center 分量）
	return t;
}

}  // namespace

TEST_CASE("SSR 两个 vec4 子区布局：C++ 常量写、字面量 723776/723792 读回逐位相同（两源结构）") {
	// 非退化输入：radius 取靶实测值量级；center **非零且各分量不同**——它不该出现在
	// 任何字节里（center 混进上传区 = 第二真相源，变异②的判据前提）。
	SceneBounds bounds;
	bounds.center = glm::vec3(1.5f, -2.5f, 3.5f);
	bounds.radius = 31.064859f;
	const SsrTuning tuning = distinctTuning();

	FrameDrawList list;
	fillSsrParams(list, bounds, tuning);

	// staging 写入：vkrenderer_impl::writeSceneStaging 同款 memcpy（偏移 = C++ 常量）。
	// 尾部 +16B 垫 = 变异①运行时形态（读回字面量 +16 单侧漂）的读取范围，避免越界 UB。
	std::vector<std::byte> stage(kSceneDataSsrGateOffset + 2 * sizeof(glm::vec4), std::byte{0});
	std::memcpy(stage.data() + kSceneDataSsrParamsOffset, &list.sceneData.ssrParams,
		sizeof(glm::vec4));
	std::memcpy(stage.data() + kSceneDataSsrGateOffset, &list.sceneData.ssrGate, sizeof(glm::vec4));

	// shader 侧读回：字面量（两源结构）。
	const glm::vec4 p = load4(stage, kSlangSsrParamsLiteral);
	const glm::vec4 g = load4(stage, kSlangSsrGateLiteral);
	CHECK(p == list.sceneData.ssrParams);  // 提取链恒等（步长/偏移错在此红）
	CHECK(g == list.sceneData.ssrGate);
	CHECK(p.x != 0.0f);                    // 缓冲没读成全 0（变异①运行时形态的判据前提）
	CHECK(g.x != 0.0f);
	// 两子区互不重叠：读到同一份字节 ⟹ 变异⑥（gate 偏移写成 params 的值）。
	CHECK(p != g);
}

TEST_CASE("fillSsrParams：四个系数逐位落槽（批 15.1 参数面契约变更，接替原「yzw = 0」）") {
	SceneBounds bounds;
	bounds.center = glm::vec3(1.5f, -2.5f, 3.5f);
	bounds.radius = 31.064859f;
	const SsrTuning tuning = distinctTuning();

	FrameDrawList list;
	fillSsrParams(list, bounds, tuning);

	// ssrParams = (sceneRadius, maxDistanceRadii, thicknessPx, biasPx)，float 直传无换算。
	CHECK(list.sceneData.ssrParams.x == bounds.radius);
	CHECK(list.sceneData.ssrParams.y == tuning.maxDistanceRadii);
	CHECK(list.sceneData.ssrParams.z == tuning.thicknessPx);
	CHECK(list.sceneData.ssrParams.w == tuning.biasPx);
	// center 不上传：两个 vec4 的 8 个槽里，任何一个都不该等于 center 的某个分量
	// （变异②：把 center.x 填进 y 之类的串位）。上面已逐槽钉死，这里是显式的那句
	// "center 不在上传区"——它把判据意图写在明面上，而不是让读者反推四条 CHECK。
	const float slots[8] = {
		list.sceneData.ssrParams.x, list.sceneData.ssrParams.y,
		list.sceneData.ssrParams.z, list.sceneData.ssrParams.w,
		list.sceneData.ssrGate.x,   list.sceneData.ssrGate.y,
		list.sceneData.ssrGate.z,   list.sceneData.ssrGate.w,
	};
	const char* slotNames[8] = { "ssrParams.x", "ssrParams.y", "ssrParams.z", "ssrParams.w",
		"ssrGate.x", "ssrGate.y", "ssrGate.z", "ssrGate.w" };
	for (std::size_t i = 0; i < 8; ++i) {
		for (int k = 0; k < 3; ++k) {
			INFO(slotNames[i] << " == center[" << k << "]（center 混进上传区）");
			CHECK(slots[i] != bounds.center[k]);
		}
	}

	// ssrGate = (roughFull, roughCutoff, blurScale, 0)；w 预留必须恒 0（批 15.4 才占）。
	// z = blurScale 是批 15.3B 的占用（此前恒 0；消费者 = ssr_apply 的螺旋模糊半径）。
	CHECK(list.sceneData.ssrGate.x == tuning.roughFull);
	CHECK(list.sceneData.ssrGate.y == tuning.roughCutoff);
	CHECK(list.sceneData.ssrGate.z == tuning.blurScale);
	CHECK(list.sceneData.ssrGate.w == 0.0f);
}

TEST_CASE("fillSsrParams：空场景 radius=0 原样透传、系数不受 bounds 污染（无第二真相源）") {
	FrameDrawList list;
	fillSsrParams(list, SceneBounds{}, distinctTuning());
	// 变异③（CPU 侧 clamp）在前一条红；本条钉的是另一半：系数不能被 bounds 带跑
	// （radius=0 时把系数一起清零 = 配置失效，shader 端 max() 兜底也救不回系数）。
	CHECK(list.sceneData.ssrParams.x == 0.0f);
	CHECK(list.sceneData.ssrParams.y == 4.44f);  // maxDistanceRadii
	CHECK(list.sceneData.ssrParams.z == 0.11f);  // thicknessPx
	CHECK(list.sceneData.ssrParams.w == 0.22f);  // biasPx
	CHECK(list.sceneData.ssrGate == glm::vec4(0.33f, 0.55f, 0.77f, 0.0f));
}

TEST_CASE("SsrTuning 出厂默认值钉死（红线 A 前提，变异⑤）") {
	// 字面量手抄：maxDistanceRadii/biasPx/roughFull/roughCutoff = git 里 rgl/shaders/ssr.frag.slang
	// 批 15.1 版本常量原值（kMaxDistanceRadii / kBiasPx / kRoughFull / kRoughCutoff）；
	// thicknessPx = **15.3C 重定值 12.0**（窗口跟住 15.3B 半分辨率 march 网格，理由 →
	// runtime_config.cppm 字段注释）。**这里必须是字面量**：写成 SsrTuning{} 的字段引用就成了
	// "默认值等于默认值"的空断言。
	// 任一默认值被改 ⟹ 本条红 ⟹ 出厂配置的 ssr 输出不再逐位等于钉住的那一版（红线 A 破）。
	const SsrTuning d;
	CHECK(d.maxDistanceRadii == 0.52f);  // kMaxDistanceRadii（bistro 饱和判据定，非靶反推）
	CHECK(d.thicknessPx == 12.0f);       // kThicknessPx（15.1 原值 6.0 → 15.3C 跟网格翻倍）
	CHECK(d.biasPx == 0.6f);             // kBiasPx
	CHECK(d.roughFull == 0.15f);         // kRoughFull
	CHECK(d.roughCutoff == 0.40f);       // kRoughCutoff
	// blurScale = 8.0 是批 15.3B 的出厂值（ssr_apply 螺旋半径系数，configs/runtime.lua
	// `ssr.blur_scale` 注释里的换算 0.05→0.4px / 0.35→2.8px 以它为基准）。
	CHECK(d.blurScale == 8.0f);

	// 两条 shader 侧不变式在默认值下必须成立（越界不钳制，但出厂值没理由越界）：
	// bias 顶不进厚度、衰减在截断之前归零。默认值被改成越界 ⟹ 这两条红。
	CHECK(d.biasPx < d.thicknessPx);
	CHECK(d.roughFull < d.roughCutoff);
	// 出厂默认值必须**全非零**：FrameDrawList 的两个 vec4 零初始化，若某个默认系数也是 0，
	// "漏填"（变异④）在默认配置下与正确填法不可区分——本文件断言仍绿而画面已错。
	CHECK(d.roughFull != 0.0f);
	CHECK(d.maxDistanceRadii != 0.0f);
	CHECK(d.roughCutoff != 0.0f);
	CHECK(d.thicknessPx != 0.0f);
	CHECK(d.biasPx != 0.0f);
	CHECK(d.blurScale != 0.0f);  // 出厂非零（0 是消融档，但默认必须是有效半径）
}

}  // namespace RGL
