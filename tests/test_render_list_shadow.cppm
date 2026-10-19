module;
/**
 * test_render_list_shadow —— fillShadowData 尾区填写契约（批 12.3d-A）
 *
 * 为什么值得测：fillShadowData 是「CPU 级联几何 → scene data 字节 → shader 选级采样」
 * 的交接点，5a-6 黑屏回归的根因（未生效 splits 槽留 0 → 全场景落到零矩阵槽 → NaN →
 * 阴影恒 0）就发生在这个函数里。这类"写错一个分量、画面整片不对但无报错"的契约，
 * 数值指标全绿而 bug 在场（阴影链七轮的共同死法，见 test_shadow_geometry 头注释）。
 *
 * 接缝（12.3d）：shadowPassEnabled 显式入参取代内部 diagnostics() 单例读取，
 * enabled 两态可在无单例下各打一条。
 *
 * ## 与指令预期的一处差异（以源码为准）
 * 指令写"未生效级保持默认（读源码确认写没写）"：现值 kActiveShadowCascades == 区容量
 * == 4，四槽 shadowViewProj 全部被写（循环上界就是 4），**不存在"未写级"**；
 * "未生效槽不得为 0"的红线落在 splits[3] = 1e9（末级哨兵）上——kNoFar→0 变异必红。
 *
 * ## 变异红线声明（每条断言"哪个变异让我红"）
 *   splits 用例   ：kNoFar 初始化/末级值改 0.0f → ①末级 ==1e9 与 ②全槽 >0 双红；
 *                  循环里 cascade 参数写死（如恒 0）→ ③逐级值 == cascadeSplitDistance(c) 红
 *   viewProj 用例 ：拷贝循环删掉/写错槽 → 逐元素 != 比对红（默认零矩阵 vs 特征矩阵）
 *   texel 用例    ：分母 kShadowMapSize 代 shadowTileSize(cols) → 差 2 倍红；
 *                  分子丢 ×2 → 差 2 倍红
 *   params 用例   ：params.w 改 0（或漏 shadowFar）→ 两态 w==shadowFar 红；
 *                  enabled 三元写反 → 两态 z 红；删 enabled 依赖（恒 1）→ 关态 z==0 红
 *   atlas 用例    ：cols 与 tileSize 错位 → 红。**已知弱锚**：FrameDrawList 默认值
 *                  (2048,1024,2,0) 恰等于契约值，"整行删除写入"本用例不红——
 *                  该字段真正的失效形态是 cols/tileSize 不同步（改级联时），此断言有效；
 *                  "从未写入"形态由 shadowTileSize/shadowAtlasColumns 的图侧同源断言兜底
 *                  （tests/test_render_graph 装饰段）。
 *   force 用例    ：见该用例内联的四条（删覆写 / 覆写泄漏到几何 / 门关档写 0 / 覆写函数恒等）。
 *                  这条是"诊断键真被消费"在**消费点**的判据——纯函数用例只钉编码本身，
 *                  接线断了它照样绿（本仓红线：单一来源不承重，访问路径必须消费那个来源）。
 */
#include <doctest/doctest.h>
#include <initializer_list>  // 必须：modules 下 std::initializer_list 不从 import 带进来
#include <cmath>
#include <glm/glm.hpp>

export module test_render_list_shadow;

import rlstd;
import test_support;
import render_list;    // fillShadowData（12.3d 接缝）
import frame_draw_list;  // FrameDrawList / SceneFrameData
import scene_context;  // ShadowVolume / cascadeSplitDistance
import scene_data;     // kActiveShadowCascades / kSceneShadowCascadeCount / kShadowDepthBias / kShadowNormalOffsetScale
import render_graph;   // kShadowMapSize / shadowAtlasColumns / shadowTileSize
import shadow_config;  // ShadowConfig / setShadowConfig（按档分立用例，N6b）

namespace RGL {
namespace {

using testing::expectNear;

/** 末级"没有更远的级"哨兵值（与实现体 kNoFar 同值；这里是**期望常量**，非同源引用）。 */
constexpr float kExpectNoFar = 1e9f;

/** 强制选级诊断键的**门关**档（= csm.force_cascade 出厂值）。每个既有用例都显式吃它：
 *  本文件钉的是"门关时 scene data 字节与键加入前逐位相同"，写死 -1 而不是省掉实参，
 *  是为了让"新调用点忘了想这件事"在编译期就过不去（fillShadowData 无默认实参）。 */
constexpr int kForceOff = -1;

/** 测试输入（量化形态的 near/far——生产调用点传 quantizeShadowDistance 产物，这里
 *  直接给 1/8 格上的值；48/0.25 = 192 倍跨度覆盖 log/uniform 混合的分布形状）。 */
constexpr float kNear = 0.25f;
constexpr float kFar = 48.0f;

/** 逐级可辨识的 xyRadius（2r/1024 在二进制下逐值精确，期望可写死对比式）。 */
constexpr std::array<float, kSceneShadowCascadeCount> kRadii{1.5f, 2.25f, 3.75f, 100.5f};

/** 造一组逐级特征不同的阴影体：viewProj 每个元素都不同且跨级不同
 *  （元素 = 1000·(c+1) + 列·4 + 行），拷贝错位/漏写都藏不住。 */
std::array<ShadowVolume, kActiveShadowCascades> makeVolumes() {
	std::array<ShadowVolume, kActiveShadowCascades> vols{};
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		glm::mat4 m(0.0f);
		for (int col = 0; col < 4; ++col)
			for (int row = 0; row < 4; ++row)
				m[col][row] = static_cast<float>(1000u * (c + 1u) + 4u * col + row);
		vols[c].viewProj = m;
		vols[c].xyRadius = kRadii[c];
	}
	return vols;
}

}  // namespace

TEST_CASE("fillShadowData：splits 契约——逐级远截止 + 末级 1e9 哨兵（5a-6 黑屏红线）") {
	const auto vols = makeVolumes();
	FrameDrawList list;
	fillShadowData(list, kNear, kFar, {vols.data(), vols.size()}, true, kForceOff);

	const glm::vec4& s = list.sceneData.shadowSplits;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		// **红线**：任何槽都不得为 0——留 0 = "已越过这一级分界"，片元被推到从未写过的槽。
		CHECK(s[c] > 0.0f);
		if (c + 1u < kActiveShadowCascades) {
			// 接线判据：第 c 槽必须来自 cascadeSplitDistance(量化输入, c, active)——
			// 循环里把 c 写死 / near、far 传反 / 用错级数，本断言红。
			expectNear(s[c], cascadeSplitDistance(kNear, kFar, c, kActiveShadowCascades));
			// 独立形态锚（不依赖 cascadeSplitDistance 实现）：截止点严格落在 (near, far) 内
			// 且逐级递增（log+uniform 混合的固有性质）。
			CHECK(s[c] > kNear);
			CHECK(s[c] < kFar);
			CHECK(s[c] < s[c + 1]);
		}
	}
	// 末级 = "没有更远的级"：恒为哨兵 1e9，不是 far、更不是 0。
	CHECK(s[kActiveShadowCascades - 1] == kExpectNoFar);
}

TEST_CASE("fillShadowData：shadowViewProj 逐级 = volumes[c].viewProj（逐位拷贝）") {
	const auto vols = makeVolumes();
	FrameDrawList list;
	fillShadowData(list, kNear, kFar, {vols.data(), vols.size()}, true, kForceOff);

	// 现状：生效级数(4) == 区容量(4)，四槽全写——若未来生效级数 < 容量，本断言的
	// 覆盖范围随 kActiveShadowCascades 自动收缩，未写槽 = FrameDrawList 默认（零矩阵）。
	static_assert(kActiveShadowCascades <= kSceneShadowCascadeCount);
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		// 拷贝是赋值不是变换：逐元素精确相等（特征矩阵跨级互异，错槽必红）。
		CHECK(list.sceneData.shadowViewProj[c] == vols[c].viewProj);
	}
}

TEST_CASE("fillShadowData：texelSizes = 2·xyRadius / shadowTileSize(cols)（分母是块边长不是图集）") {
	const auto vols = makeVolumes();
	FrameDrawList list;
	fillShadowData(list, kNear, kFar, {vols.data(), vols.size()}, true, kForceOff);

	const uint32_t cols = shadowAtlasColumns(kActiveShadowCascades);
	const float tile = static_cast<float>(shadowTileSize(cols));
	// 锚：4 级 → 2 列 → 块 1024（≠ 图集整幅 2048）。分母误用 kShadowMapSize 时
	// tile == kShadowMapSize 不成立，且下面的 expectNear 差 2 倍也红。
	CHECK(tile == kShadowMapSize / 2.0f);
	CHECK(tile != static_cast<float>(kShadowMapSize));
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		expectNear(list.sceneData.shadowTexelSizes[c], 2.0 * kRadii[c] / tile);
}

TEST_CASE("fillShadowData：params = (bias, normalOffsetScale, enabled, shadowFar) 两态") {
	const auto vols = makeVolumes();

	FrameDrawList on;
	fillShadowData(on, kNear, kFar, {vols.data(), vols.size()}, true, kForceOff);
	CHECK(on.sceneData.shadowParams ==
		glm::vec4(kShadowDepthBias, kShadowNormalOffsetScale, 1.0f, kFar));

	// 反向用例：关阴影只改 z=0，其余三项**照常填**（w 仍 = shadowFar——末级淡出的
	// 采样端契约不因开关退化；z 误写死 1 或 w 改 0 都在本断言红）。
	FrameDrawList off;
	fillShadowData(off, kNear, kFar, {vols.data(), vols.size()}, false, kForceOff);
	CHECK(off.sceneData.shadowParams ==
		glm::vec4(kShadowDepthBias, kShadowNormalOffsetScale, 0.0f, kFar));
}

TEST_CASE("fillShadowData：atlas = (图集边长, 单块边长, 列数, 0)（与图装饰 extent 同源）") {
	const auto vols = makeVolumes();
	FrameDrawList list;
	fillShadowData(list, kNear, kFar, {vols.data(), vols.size()}, true, kForceOff);

	const uint32_t cols = shadowAtlasColumns(kActiveShadowCascades);
	CHECK(list.sceneData.shadowAtlas == glm::vec4(static_cast<float>(kShadowMapSize),
		static_cast<float>(shadowTileSize(cols)), static_cast<float>(cols), 0.0f));
}

TEST_CASE("fillShadowData：软影两区各读各的调参块（按档分立 = 消债-16⑤ 的唯一机器护栏）") {
	// 为什么值得测：改造前两个区共用一套 `pcss_*` 键，"EVSSM 的半影上限"只能借 PCSS 的
	// 键名写。N6b 起 configs/shadow.lua 有 pcss / evssm 两块，**取值来源**就是这条契约的
	// 本体——串档（两块都从同一块取）不会崩、不会报错，只会让"改 evssm 的半影上限"
	// 静默改到 PCSS 身上（正是债-16⑤ 的形态）。两区各自只有一个 shader 变体会读，
	// 所以"按当前档挑一块"在运行期没有消费者（ShadowFormPlan 头注记此事），能测的、
	// 也是唯一需要钉的，就是**两块互不串**。
	// 变异红线：把 `shadowConfig().evssm` 改成 `.pcss`（或反向）→ 本用例红。
	const auto vols = makeVolumes();
	ShadowConfig c;
	c.pcss = {1.0, 10.0, 11.0};   // 三值互不相同且与 evssm 块两两不同 ⟹ 错位藏不住
	c.evssm = {2.0, 20.0, 21.0};
	setShadowConfig(c);
	// 单例 = 进程级状态，谁改谁收尾（同 test_scene_config 的单例往返用例）。本用例只用
	// CHECK（失败不中断）⟹ 末尾还原足够，不需要 scope guard。

	FrameDrawList list;
	fillShadowData(list, kNear, kFar, {vols.data(), vols.size()}, true, kForceOff);

	constexpr float kDegToHalfRad = 0.01745329252f * 0.5f;
	CHECK(list.sceneData.shadowPcss.x == doctest::Approx(2.0f * std::tan(1.0 * kDegToHalfRad)));
	CHECK(list.sceneData.shadowPcss.y == 10.0f);   // ← pcss.search_radius
	CHECK(list.sceneData.shadowPcss.z == 11.0f);   // ← pcss.max_penumbra
	CHECK(list.sceneData.shadowPcss.w == 0.0f);

	CHECK(list.sceneData.shadowEvssm.x == doctest::Approx(std::tan(2.0f * kDegToHalfRad)));
	CHECK(list.sceneData.shadowEvssm.y == 20.0f);  // ← evssm.search_radius（不是 pcss 的 10）
	CHECK(list.sceneData.shadowEvssm.z == 21.0f);  // ← evssm.max_penumbra（不是 pcss 的 11）
	// w = 矩链最大 mip（期望常量，非同源引用；EvssmShadow 8 级链固定 7）
	CHECK(list.sceneData.shadowEvssm.w == 7.0f);
	setShadowConfig(ShadowConfig{});  // 还原单例（见上）
}

TEST_CASE("fillShadowData：csm.force_cascade 只覆写 scene data 的 splits，几何其余字段逐位不变（消费点接线）") {
	// 为什么值得测：本键的全部意义 = "同一套级联几何、只换采样源"。实现若图省事把 force 也
	// 喂给 cascadeSplitDistance / volumes，几何就跟着变了，两臂差分测的不再是选级（而这条
	// 走样**不会崩、不会报错**，只会让结论静默失去意义——正是本仓最怕的形态）。
	// 所以钉三件事：① 阳性 = force 真被消费（splits 变成钉级形态）；② 反向 = 覆写没有泄漏到
	// 几何其余字段（同一次调用里 viewProj/texels/params/atlas 与门关那份逐位相同）；
	// ③ 门关那份仍是**真实** splits（闭式期望，不吃被测输出当期望）——否则"两臂不同"
	// 可以靠两边都写死糊过去。
	// 变异红线：① 删掉 forceShadowCascadeSplits 那层（直接 `= splits`）→ ① 红；
	// ② 把覆写同时施加到 texels/volumes 上 → ② 红；③ 门关档写成 0（= 恒钉第 0 级）→ ③ 红；
	// ④ forceShadowCascadeSplits 自己改成恒等返回 → ① 红（与纯函数用例双保险）。
	const auto vols = makeVolumes();
	FrameDrawList off;
	FrameDrawList forced2;
	fillShadowData(off, kNear, kFar, {vols.data(), vols.size()}, true, kForceOff);
	fillShadowData(forced2, kNear, kFar, {vols.data(), vols.size()}, true, 2);

	// ① 钉到第 2 级 ⟹ {0, 0, 1e9, 1e9}：低于本级的分界恒"已越过"、本级及以远恒"未越过"。
	CHECK(forced2.sceneData.shadowSplits
		== glm::vec4(0.0f, 0.0f, kExpectNoFar, kExpectNoFar));
	// 门关那份必须**不是**这个形态（否则 ① 的"不同"没有对照物）
	CHECK_FALSE(off.sceneData.shadowSplits == forced2.sceneData.shadowSplits);

	// ② 覆写只吃 splits：几何其余字段逐位相同（= "同一套级联几何"这句话的机器形态）
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		CHECK(forced2.sceneData.shadowViewProj[c] == off.sceneData.shadowViewProj[c]);
	CHECK(forced2.sceneData.shadowTexelSizes == off.sceneData.shadowTexelSizes);
	CHECK(forced2.sceneData.shadowParams == off.sceneData.shadowParams);
	CHECK(forced2.sceneData.shadowAtlas == off.sceneData.shadowAtlas);

	// ③ 门关 = 逐位不变：splits 仍是逐级 cascadeSplitDistance + 末级哨兵，且**没有 0 槽**
	//（5a-6 那条"留 0 → 推到零矩阵槽 → NaN 全黑"的红线在门关档必须继续成立）。
	for (uint32_t c = 0; c + 1u < kActiveShadowCascades; ++c) {
		expectNear(off.sceneData.shadowSplits[c],
			cascadeSplitDistance(kNear, kFar, c, kActiveShadowCascades));
		CHECK(off.sceneData.shadowSplits[c] > 0.0f);
	}
	CHECK(off.sceneData.shadowSplits[kActiveShadowCascades - 1] == kExpectNoFar);
}

}  // namespace RGL
