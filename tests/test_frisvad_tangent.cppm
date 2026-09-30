module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <cmath>  // std::isfinite（宏/自由函数不经 BMI 保证传递，GMF 自带）
#include <initializer_list>  // 范围 for 的花括号列表（modules 下不从 import 带进来）

// Frisvad 切线兜底契约回归（批 12.3b）——被测对象 = rgl/gltf_loader.cppm 的
// export inline frisvadTangent（无 TANGENT 属性资产的 loader 兜底，bistro_pack.py
// 同式）。
//
// ## 为什么值得钉（黑面 bug 根因，调试历史 2026-10-01）
//
// 原式 `a = -1/(n.z+1)` 曾被误写 `1/(1-n.z)`：奇点从 n.z=-1（guard 已处理）搬到
// n.z=+1（无人处理）→ 法线恰 (0,0,1) 的顶点得 **NaN 切线** → shader TBN 0×NaN
// 污染整面 → 受光面整面纯黑，而 model_pbr 管线（不读切线）逐字节正确——症状是
// 「同一场景只有法线 +Z 的面黑」，离真因三跳。GPU 端黑面 + 逐字节探针查了两天，
// 但函数本体是纯数学：**6 个轴向法线 + 有限性断言在毫秒级就能钉死**。
//
// ## 阳性锚点（变异红线）
//
// 把公式改回旧式 `1.0f/(1.0f - n.z)` → 「轴向法线」用例的 (0,0,+1) 立刻 NaN 变红；
// 删掉奇点 guard → (0,0,-1) 变红。两条断言都不可由"恒假"或"恒真"的实现通过。

export module test_frisvad_tangent;

import rlstd;
import test_support;
import gltf_loader;

using RGL::frisvadTangent;
using RGL::testing::expectNear;
using RGL::testing::Lcg;

namespace {

// 判据口径：单位性 / 正交性 / 有限性。全部先过有限性——NaN 参与 == 比较恒 false，
// 若先比长度会把 NaN 报成"长度不对"而不是"坏了"（症状误导，本 bug 的调试教训）。
void checkTangent(const glm::vec3& n, const glm::vec3& t) {
	CHECK(std::isfinite(t.x));
	CHECK(std::isfinite(t.y));
	CHECK(std::isfinite(t.z));
	expectNear(glm::length(t), 1.0, 1e-5);
	// 阈值 1e-4 而非更严：float32 在奇点附近（a = 1/(1+z) 达 10⁴ 量级）抵消放大，
	// z=-0.999924 处正交残差实测 ~1.4e-5（数学上精确，纯条件数）。该阈值仍远小于
	// 两个错误公式在此域的真实误差（记忆式 -1/(z+1) 最差 dot=0.89、随机域 ≥0.05），
	// 变异灵敏度不受影响；shader 端 Gram-Schmidt 吸收这部分残差。
	CHECK(std::abs(glm::dot(n, t)) < 1e-4f);
}

}  // namespace

TEST_SUITE("frisvad") {

TEST_CASE("轴向法线：六个 axis-aligned 法线全部得到有限单位切线（含 ±Z 奇点侧）") {
	// (0,0,+1) = 旧式（1/(1-n.z)）的 NaN 触发点 = 黑面 bug 的复现输入。
	const glm::vec3 axes[] = {
		{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
	};
	for (const auto& n : axes) {
		INFO("n = ", n.x, ", ", n.y, ", ", n.z);
		checkTangent(n, frisvadTangent(n));
	}
}

TEST_CASE("guard 内侧连续域：n.z 接近奇点但未及 guard 阈值时仍有限、单位、正交") {
	// 取 -0.99 量级而非 -0.9999999：后者 |a|≈10⁷，点积的浮点抵消误差与判据同量级
	// （「判据噪声淹没信号」——tests 纪律），要钉 guard 存在性请用 (0,0,-1) 轴向用例。
	for (float z : {-0.99f, -0.999f, -0.9f}) {
		const float r = std::sqrt(1.0f - z * z);
		glm::vec3 n{r * 0.6f, r * 0.8f, z};
		n = glm::normalize(n);
		INFO("n.z = ", z);
		checkTangent(n, frisvadTangent(n));
	}
}

TEST_CASE("随机单位法线（Lcg 写死种子）：2000 样本全部有限、单位、正交") {
	// 固定种子（确定性纪律）；球面采样后归一化，覆盖 guard 内外与全部卦限。
	Lcg rng(0x5EEDB10Du);
	for (int i = 0; i < 2000; ++i) {
		const glm::vec3 n = glm::normalize(glm::vec3(
			rng.uniform(-1.0, 1.0), rng.uniform(-1.0, 1.0), rng.uniform(-1.0, 1.0)));
		INFO("i = ", i, "  n = ", n.x, ", ", n.y, ", ", n.z);
		checkTangent(n, frisvadTangent(n));
	}
}

}  // TEST_SUITE("frisvad")
