module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <cmath>

/**
 * test_aa_target —— 批 12.6 T4 细线抗锯齿靶（rgl/aa_target.cppm，rlbase 纯函数几何）
 *
 * 被测 = 程序化合成的"细线条靶"（四边形表 + 由它合成的 GltfModel）。它是 TAA 阳性
 * 锚点的**载体**：若靶本身不细（边长远大于像素）或不摊开（全部投影到同一列），无
 * TAA 时也不会有闪烁 → "开 TAA 后逐帧差下降"的判据退化成恒真（skill
 * rgl-test-discipline：判据工具自己要有自检）。本文件钉四件事：**数据自洽** /
 * **确实细** / **确实摊开且亚像素** / **合成出的模型与形状表一致**。
 *
 * ## 期望值出处（独立于被测实现）
 *  - 四边形数 = 画板 1 + 阶梯 12 + 对角 2×6 + 环 6×48 + 放射 16 + 栅栏 5×13 = 394；
 *  - "细"判据 = 除画板外每四边形最短边 ≤ 0.02m（推荐机位 8.7px；绝对阈值，不取实现常量）；
 *  - "亚像素"判据 = **独立投影公式** px/m = H / (2·d·tan(fov/2))（推荐机位常量），
 *    与 aa_target.cppm 的顶点算术无共同出处；
 *  - 模型侧 = 去索引展开的三角形数 = 四边形数 × 2（每 quad 两三角，非从顶点数反推）。
 */
export module test_aa_target;

import rlstd;
import test_support;
import gltf_loader;            // GltfModel / GltfPrimitive（aa_target 的产物类型）
import model_types_generated;  // ModelVertex（顶点字段读取）
import aa_target;

namespace RGL {
namespace {

using testing::expectNear;

double edgeLen(const glm::vec3& a, const glm::vec3& b) {
	return static_cast<double>(glm::length(a - b));
}

double quadMinEdge(const TargetQuad& q) {
	return std::min({edgeLen(q.c[0], q.c[1]), edgeLen(q.c[1], q.c[2]), edgeLen(q.c[2], q.c[3]),
						 edgeLen(q.c[3], q.c[0])},
		[](double a, double b) { return a < b; });
}

glm::vec3 quadCenter(const TargetQuad& q) {
	return (q.c[0] + q.c[1] + q.c[2] + q.c[3]) * 0.25f;
}

/** 推荐机位常量（与 aa_target.cppm 头注释、imgqa temporal 配置同一份数字）。 */
constexpr double kCamZ = 2.0;       ///< 相机 (0,0,2) 看 −Z
constexpr double kFovRad = 0.78539816339744831;  ///< 45°
constexpr double kViewHeight = 720.0;

/** 深度 d 处的 px/m（独立闭式解：视锥高度 = 2·d·tan(fov/2)）。 */
double pxPerMeter(double d) {
	return kViewHeight / (2.0 * d * std::tan(kFovRad * 0.5));
}

/** 按颜色取 primitive（合成契约：0 = 白件、1 = 黑件）。 */
const GltfPrimitive& primOf(const GltfModel& m, size_t i) {
	REQUIRE(m.primitives.size() > i);
	return m.primitives[i];
}

}  // namespace

TEST_CASE("aa_target：形状表自洽（quad 数/黑白分组/坐标有限）+ 确定性单例") {
	const auto& s = aaTargetShape();
	// 元素计数：画板 + 阶梯 + 对角（2 组）+ 环（6 环 × 48 段）+ 放射 + 栅栏（5 排 × 13）
	const size_t quads = 1u + aaTarget::kLadderRods + aaTarget::kCrossSets * aaTarget::kCrossRods
		+ aaTarget::kRingCount * aaTarget::kRingSegments + aaTarget::kSpokeCount
		+ aaTarget::kFenceRows * aaTarget::kFenceRods;
	REQUIRE(quads == 394u);  // 常量表被改动 = 显式决定（本行同步）
	REQUIRE(s.quads.size() == quads);
	size_t white = 0;
	for (const auto& q : s.quads) {
		white += q.white ? 1u : 0u;
		for (const auto& c : q.c)
			for (uint32_t a = 0; a < 3u; ++a)
				CHECK(std::isfinite(c[a]));
	}
	// 白件 = 画板 + 栅栏；黑件 = 画板上的三组图元
	CHECK(white == 1u + aaTarget::kFenceRows * aaTarget::kFenceRods);
	CHECK(quads - white == aaTarget::kLadderRods + aaTarget::kCrossSets * aaTarget::kCrossRods
		+ aaTarget::kRingCount * aaTarget::kRingSegments + aaTarget::kSpokeCount);
	// Meyers 单例：两次调用同一对象（进程内只合成一次）
	CHECK(&aaTargetShape() == &aaTargetShape());
	CHECK(&aaTargetModel() == &aaTargetModel());
}

TEST_CASE("aa_target：除画板外每一片都真的「细」（阳性锚点的载体判据）") {
	const auto& s = aaTargetShape();
	// 阈值**取绝对值不取 kLadderWMax**：拿实现常量当阈值 = 常量与判据同源自证
	// （把杆改粗到 0.05 也照样绿）。0.02m 的出处 = 推荐机位下 8.7px，再宽就不叫
	// "细线条靶"了（判据出处独立于实现，skill rgl-test-discipline）。
	constexpr double kMaxThin = 0.02;
	size_t fat = 0;
	for (size_t i = 0; i < s.quads.size(); ++i) {
		INFO("quad #" << i);
		if (quadMinEdge(s.quads[i]) > kMaxThin) {
			++fat;
			CHECK(i == 0u);  // 胖的只准是画板
		}
	}
	CHECK(fat == 1u);
	CHECK(quadMinEdge(s.quads[0]) > 0.4);  // 画板确实是大面（不是又一个细条）
}

TEST_CASE("aa_target：亚像素/超像素两档都在场（独立投影闭式解，非实现反推）") {
	// 推荐机位（相机 (0,0,2)、fov 45°、720p）下的 px/m：画板平面 d=2 → ≈435。
	CHECK(pxPerMeter(kCamZ) > 400.0);
	CHECK(pxPerMeter(kCamZ) < 470.0);
	// 阶梯：最细杆必须**亚像素**（否则无闪烁 = 判据无信号），最粗杆必须明显超像素
	// （否则"跌破 1px"这一档没有对照，看不出时域累积到底修没修好）。
	const double ladderPx = pxPerMeter(kCamZ);
	CHECK(aaTarget::kLadderWMin * ladderPx < 1.0);
	CHECK(aaTarget::kLadderWMax * ladderPx > 4.0);
	// 栅栏：首排超像素、末排跌破 1px（"等间距细杆沿 −Z 延伸，远处自然跌破 1px"）
	const double nearRow = kCamZ - aaTarget::kFenceZ0;
	const double farRow = kCamZ - (aaTarget::kFenceZ0
		- static_cast<double>(aaTarget::kFenceRows - 1u) * aaTarget::kFenceDz);
	CHECK(aaTarget::kFenceWidth * pxPerMeter(nearRow) > 1.5);
	CHECK(aaTarget::kFenceWidth * pxPerMeter(farRow) < 1.0);
}

TEST_CASE("aa_target：摊开在整屏宽度上、上下两半不重叠（旧版塌成一根杆的回归）") {
	const auto& s = aaTargetShape();
	// ① 摊开：细线元素（除画板）的 x 中心跨度必须覆盖大半个画板宽度。
	//    旧版把三组元素全摆在 x=0，沿 −Z 看过去全投到同一列 → 整屏一根杆（实测踩过）。
	double minX = 1e9, maxX = -1e9;
	for (size_t i = 1; i < s.quads.size(); ++i) {
		const glm::vec3 c = quadCenter(s.quads[i]);
		minX = std::min(minX, static_cast<double>(c.x));
		maxX = std::max(maxX, static_cast<double>(c.x));
	}
	CHECK(maxX - minX > 1.6);
	// ② 上下两半分开：画板上的图元在 z=0 前层且 y>0，栅栏 y<0 且 z<0。
	//    沿 −Z 的投影保号 ⇒ 两半在屏幕上不可能重叠（重叠 = 归属没法谈）。
	for (size_t i = 1; i < s.quads.size(); ++i) {
		const glm::vec3 c = quadCenter(s.quads[i]);
		INFO("quad #" << i << " center=(" << c.x << "," << c.y << "," << c.z << ")");
		if (c.z == 0.0f)
			CHECK(c.y > 0.0f);  // 画板图元：上半
		else
			CHECK(c.y < 0.0f);  // 栅栏：下半
	}
	// ③ 深度分层成立：画板在图元**之后**（相机在 +Z），否则黑杆会被白底盖掉。
	const glm::vec3 plate = quadCenter(s.quads[0]);
	CHECK(plate.z < 0.0f);
	CHECK(plate.z > aaTarget::kFenceZ0);  // 画板比栅栏近
}

TEST_CASE("aa_target：阶梯杆互不粘连（每根是独立岛，闪烁才可归属到单根）") {
	const auto& s = aaTargetShape();
	// 阶梯 = 画板之后紧接的 kLadderRods 个 quad（合成顺序即产物顺序）。
	std::vector<std::pair<double, double>> centerHalfWidth;  // (x 中心, 半宽)
	for (uint32_t i = 0; i < aaTarget::kLadderRods; ++i) {
		const TargetQuad& q = s.quads[1u + i];
		const double x = static_cast<double>(quadCenter(q).x);
		const double half = 0.5 * std::abs(static_cast<double>(q.c[1].x - q.c[0].x));
		centerHalfWidth.emplace_back(x, half);
		expectNear(static_cast<double>(quadCenter(q).y), aaTarget::kLadderY, 1e-5, 1e-6);
	}
	for (size_t i = 1; i < centerHalfWidth.size(); ++i) {
		const auto& [x0, h0] = centerHalfWidth[i - 1];
		const auto& [x1, h1] = centerHalfWidth[i];
		INFO("rod " << i);
		// 中心间距 > 两侧半宽之和 ⇒ 相邻杆之间留白（粘连 = 一根粗杆，档位信息丢失）
		CHECK(x1 - x0 > h0 + h1);
	}
	// 宽度单调爬升（"阶梯"之名所在）
	for (size_t i = 1; i < centerHalfWidth.size(); ++i)
		CHECK(centerHalfWidth[i].second > centerHalfWidth[i - 1].second);
}

TEST_CASE("aa_target：合成的 GltfModel 与形状表一致（模型路消费的是同一份真相）") {
	const auto& m = aaTargetModel();
	const auto& s = aaTargetShape();
	REQUIRE(m.primitives.size() == 2u);  // 白件 / 黑件
	size_t whiteQuads = 0;
	for (const auto& q : s.quads)
		whiteQuads += q.white ? 1u : 0u;
	// 去索引展开：每 quad = 2 三角形 = 6 顶点（三角形数从 quad 数算，不从顶点数反推）
	CHECK(primOf(m, 0).triangleCount == whiteQuads * 2u);
	CHECK(primOf(m, 1).triangleCount == (s.quads.size() - whiteQuads) * 2u);
	for (const auto& p : m.primitives) {
		CHECK(p.vertices.size() == p.triangleCount * 3u);
		CHECK(p.triangleCount > 0u);       // 0 = 不渲染（asset_table 契约）
		CHECK(p.unlit == true);            // 判据要"无光照变量"，pbr 会把闪烁与受光混在一起
		CHECK(p.alphaModeBlend == false);  // 透明走另一条队列 = 时域判据混入混合
		CHECK(p.baseColor.a == 1.0f);
		CHECK(p.bounds.radius > 0.0f);
		for (const auto& v : p.vertices) {
			CHECK(glm::length(v.normal) == doctest::Approx(1.0));  // 法线单位长（deferred 路要用）
			CHECK(v.tangent.w == 1.0f);
		}
	}
	// 黑白两值高对比（baseColor 是颜色唯一出处，无纹理）
	CHECK(primOf(m, 0).baseColor == glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
	CHECK(primOf(m, 1).baseColor == glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
	// 世界聚合球**装得下**两个 primitive 的全部顶点（SceneBounds 靠它定 near/far；
	// 判据 = 逐顶点包含，不用"球心距 + 半径"三角不等式上界——聚合球比那个上界更紧）
	for (const auto& p : m.primitives)
		for (const auto& v : p.vertices) {
			const double d = static_cast<double>(glm::length(v.position - m.bounds.center));
			CHECK(d <= m.bounds.radius + 1e-3);
			const double dp = static_cast<double>(glm::length(v.position - p.bounds.center));
			CHECK(dp <= p.bounds.radius + 1e-3);
		}
}

}  // namespace RGL
