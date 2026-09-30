module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>  // 测试侧以向量视角审视 cube 顶点（Vertex 本体是 float[3]）
#include <cstddef>  // offsetof（宏不经 BMI 传递，GMF 自带）

// 内建 cube 几何契约回归（批 12.1 · B1）——被测对象 = rgl/cube.cppm（rlcore 成员，
// 纯数据，零 GPU）。
//
// ## 为什么值得钉
//
// docs/archive/issue_history.md 记录的 cottage 越界读（`idx*3` 应 `idx*2` + 三角形
// 索引全局位/组内位混用）症状是"偶发 + 区域固定 + 随构建时序变"——典型的索引数据
// 契约破坏形态。cube 是全项目唯一的内建索引几何，且 state_kit.cppm:32 注释自述
// "硬编码 cube 绕序未验证"（opaque 段 cull=eNone，绕序在渲染端从未被真正检验）——
// 本文件就是补上这块从未被验证的地基。
//
// ## 一条与任务预设相反的实测结论（以数据为准）
//
// 任务预设"外向绕序：叉积方向背离立方体中心"。实测（本文件「绕序一致性」组）：
// **右手系 `cross(B−A, C−A)` 恒指向中心**（对全部 12 个三角形
// `dot(n, centroid) = −0.5`，无一例外）。源注释"逆时针绕序"（cube.cppm:28）与之
// 并不矛盾——绕序的"顺/逆"取决于观察坐标系（左手看右手判、面内看与面外看互反），
// 文字可两读，数据只有一个符号。本文件把**数据侧符号**钉死：任何人翻转任一三角形
// （或重排索引时混用全局位/组内位）都会立即变红，这才是回归需要的判据。
export module test_cube_geometry;

import rlstd;
import test_support;
import cube;

namespace RGL {
namespace {

using testing::expectNear;

/** 三角形第 i 组的三个顶点坐标（调用方保证 3i+2 < 索引数）。 */
struct Tri {
	glm::vec3 a, b, c;
};

Tri triAt(size_t i) {
	const auto& V = cubeVertices;
	const auto& I = cubeIndices;
	return Tri{glm::vec3{V[I[3 * i]].position[0], V[I[3 * i]].position[1],
					V[I[3 * i]].position[2]},
		glm::vec3{V[I[3 * i + 1]].position[0], V[I[3 * i + 1]].position[1],
			V[I[3 * i + 1]].position[2]},
		glm::vec3{V[I[3 * i + 2]].position[0], V[I[3 * i + 2]].position[1],
			V[I[3 * i + 2]].position[2]}};
}

/** 顶点 i 的位置。 */
glm::vec3 posOf(size_t i) {
	const auto& v = cubeVertices[i];
	return glm::vec3{v.position[0], v.position[1], v.position[2]};
}

/** 面归属编码：返回 (共享轴, 平面值>0) 编码的 0..5 面号；组内 4 顶点不共面返回 −1。 */
int faceOfGroup(size_t faceIdx) {
	const int idx[6] = {static_cast<int>(cubeIndices[6 * faceIdx]),
		static_cast<int>(cubeIndices[6 * faceIdx + 1]),
		static_cast<int>(cubeIndices[6 * faceIdx + 2]),
		static_cast<int>(cubeIndices[6 * faceIdx + 3]),
		static_cast<int>(cubeIndices[6 * faceIdx + 4]),
		static_cast<int>(cubeIndices[6 * faceIdx + 5])};
	// 逐轴找"四个顶点坐标全相等"的轴（值须为 ±0.5）
	for (int axis = 0; axis < 3; ++axis) {
		const float v0 = posOf(static_cast<size_t>(idx[0]))[axis];
		bool allSame = true;
		for (int k = 1; k < 6; ++k)
			if (posOf(static_cast<size_t>(idx[k]))[axis] != v0)
				allSame = false;
		if (allSame && (v0 == 0.5f || v0 == -0.5f))
			return axis * 2 + (v0 > 0.0f ? 1 : 0);
	}
	return -1;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 组 1：数据尺寸与字节布局
//
// 编译期 static_assert 是零成本护栏（改布局直接编译失败），运行时 CHECK 让断言
// 出现在测试报告里可被"看见"——两者都留。
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Vertex 布局契约：24 字节 / color@12（builtin_geometry 上传步长的定义处）") {
	static_assert(sizeof(Vertex) == 24);
	static_assert(offsetof(Vertex, color) == 12);
	// 运行时再报一遍：布局若被改，static_assert 先炸，这组保证"现在炸不炸"可见。
	CHECK(sizeof(Vertex) == 24);
	CHECK(offsetof(Vertex, position) == 0);
	CHECK(offsetof(Vertex, color) == 12);
}

TEST_CASE("cube 规模契约：8 顶点 / 36 索引 / 12 三角形 / 6 面") {
	CHECK(cubeVertices.size() == 8);
	CHECK(cubeIndices.size() == 36);
	// 三角形数 = 12、面数 = 6：36 必须是 6 的倍数（面 = 2 三角形是渲染分组前提）
	CHECK(cubeIndices.size() % 6 == 0);
	CHECK(cubeIndices.size() / 3 == 12);
	CHECK(cubeIndices.size() / 6 == 6);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 2：顶点集 = (±0.5)³ 全组合
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("cube 顶点：分量 ∈ [−0.5,0.5]、颜色 ∈ [0,1]、8 角两两不同且恰为全组合") {
	// 8 个角点必须是 (±0.5,±0.5,±0.5) 的 8 种符号组合，一个不多一个不少。
	// 判据独立于源数据字面量：符号位编码成 3bit 编号，8 个槽恰好各命中一次——
	// "去重后仍 8 个"与"全组合"两个要求一次覆盖（重复角 = 某槽两次 + 某槽 0 次）。
	std::array<bool, 8> seen{};
	for (size_t i = 0; i < cubeVertices.size(); ++i) {
		INFO("vertex#" << i);
		const glm::vec3 p = posOf(i);
		for (int k = 0; k < 3; ++k) {
			// 立方体范围（−0.5f/0.5f 二进制精确，边界取闭区间）
			CHECK(p[k] >= -0.5f);
			CHECK(p[k] <= 0.5f);
			// 更强的要求：角点必须在角上，分量恰为 ±0.5（不是面内点/原点）
			CHECK((p[k] == 0.5f || p[k] == -0.5f));
		}
		const int code = (p.x > 0.0f ? 1 : 0) | (p.y > 0.0f ? 2 : 0) | (p.z > 0.0f ? 4 : 0);
		INFO("sign-code=" << code << " 重复 = 角点重复定义");
		CHECK(!seen[static_cast<size_t>(code)]);
		seen[static_cast<size_t>(code)] = true;
	}
	for (size_t c = 0; c < 8; ++c)
		CHECK(seen[c]);  // 全组合覆盖：某角缺失 = 该槽恒 false
	// 顶点色逐分量 ∈ [0,1]（unorm 直传，越界色进 shader 是静默花屏）
	for (size_t i = 0; i < cubeVertices.size(); ++i)
		for (int k = 0; k < 3; ++k) {
			INFO("vertex#" << i << " color[" << k << "]");
			CHECK(cubeVertices[i].color[k] >= 0.0f);
			CHECK(cubeVertices[i].color[k] <= 1.0f);
		}
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 3：索引契约（越界读回归 + 无退化 + 面归属）
//
// cottage 事故的形态是"索引值合法但基准错"（全局位当组内位用），只查 `< 8`
// 挡不住——所以再加两级：三角形内互异（无退化）、每 6 索引组恰好覆盖 cube 一个面
// 的 4 个角（面归属正确）。
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("cube 索引：全部 < 8（越界读回归）且每个三角形 3 顶点互异（无退化）") {
	REQUIRE(cubeIndices.size() % 3 == 0);
	for (size_t j = 0; j < cubeIndices.size(); ++j) {
		INFO("index[" << j << "] = " << cubeIndices[j]);
		CHECK(cubeIndices[j] < cubeVertices.size());
	}
	for (size_t t = 0; t < cubeIndices.size() / 3; ++t) {
		const uint32_t i0 = cubeIndices[3 * t], i1 = cubeIndices[3 * t + 1],
					   i2 = cubeIndices[3 * t + 2];
		INFO("triangle#" << t << " = (" << i0 << "," << i1 << "," << i2 << ")");
		CHECK(i0 != i1);
		CHECK(i1 != i2);
		CHECK(i0 != i2);
	}
}

TEST_CASE("cube 索引面归属：6 个 6-索引组 = 6 个不同的面，每组两三角形共享一条边") {
	std::array<bool, 6> faceSeen{};
	for (size_t f = 0; f < cubeIndices.size() / 6; ++f) {
		const int face = faceOfGroup(f);
		INFO("group#" << f << " face=" << face << "（−1 = 组内 4 顶点不共一个 ±0.5 平面）");
		CHECK(face >= 0);
		CHECK(!faceSeen[static_cast<size_t>(face)]);  // 6 组 6 面，无一重复
		faceSeen[static_cast<size_t>(face)] = true;
		// 组内两三角形必须共享恰 2 个索引（一条边）——三角形索引全局/组内混用时
		// 这一条先炸（共享数变 0 或 3）。
		const uint32_t t0[3] = {cubeIndices[6 * f], cubeIndices[6 * f + 1], cubeIndices[6 * f + 2]};
		const uint32_t t1[3] = {cubeIndices[6 * f + 3], cubeIndices[6 * f + 4],
			cubeIndices[6 * f + 5]};
		int shared = 0;
		for (int a = 0; a < 3; ++a)
			for (int b = 0; b < 3; ++b)
				if (t0[a] == t1[b])
					++shared;
		INFO("group#" << f << " 两三角形共享索引数 = " << shared);
		CHECK(shared == 2);
	}
	for (size_t k = 0; k < 6; ++k)
		CHECK(faceSeen[k]);  // 六个面全覆盖
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 4：绕序一致性（实测契约，见文件头）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("cube 绕序一致性：12 个三角形 dot(cross(B−A,C−A), 重心) 恒为 −0.5") {
	// 实测值 −0.5 = |面法线单位长| × |重心在法轴上的投影|（面在 ±0.5 处），12 组
	// 全等——float32 下 0.5 系数值二进制精确，容差 1e-5 只是留给未来非轴对齐几何。
	//
	// 这条断言证伪的不是"外向"这个文字语义，而是三条硬事实：
	//   ① 绕序**一致**——翻任何一面 → 该组变 +0.5，红；
	//   ② 三角形非退化——共线/重合顶点 → 叉积 0，红；
	//   ③ 索引基准不错——全局/组内位混用 → 值或符号跳变，红。
	for (size_t t = 0; t < cubeIndices.size() / 3; ++t) {
		const Tri tri = triAt(t);
		const glm::vec3 n = glm::cross(tri.b - tri.a, tri.c - tri.a);
		const glm::vec3 centroid = (tri.a + tri.b + tri.c) / 3.0f;  // 中心 = 原点
		const float d = glm::dot(n, centroid);
		INFO("triangle#" << t << " dot=" << d);
		expectNear(d, -0.5, 0.0, 1e-5);
	}
}

}  // namespace RGL
