module;
#include <glm/glm.hpp>  // 顶点算术（GMF include，同 scene_context 惯例；rlstd 不带 glm）
export module aa_target;

import rlstd;
import gltf_loader;               // GltfModel / GltfPrimitive / BoundingSphere
import model_types_generated;     // ModelVertex（48B 契约布局，model_schema.lua 单一真相源）

namespace RGL {

// 细线条抗锯齿靶（批 12.6 T4，docs/taa_plan.md §6）——**程序化纯函数几何**，
// 内存合成一份 `GltfModel` 走模型上传现成路（uploadModelMeshes + registerScene）。
//
// 为什么走模型路而**不是**内建几何路（cube 那条）：`cube.mesh.slang` 的 mesh 输出
// 是**写死的 8 顶点 / 12 三角形**（`addIndexedMesh` 的 groupCount 因此恒 1，asset_table
// 注释即点名"不套 ceil 公式"）。细线靶要几百个四边形，走那条路会被静默截断成前 12
// 个三角形（实测：整屏只剩画板一角，黑屏排查半天）。模型路的 groupCount =
// ceil(tri/kTrianglesPerGroup) 本来就是任意复杂度几何的路 ⇒ 零新 shader、零文件依赖。
//
// 为什么需要这个靶：内建 cube 是"大平面 + 深背景"，对 TAA 几乎无信号——门关/门开
// 截图在平面上逐位相同，TAA 有效性就没有像素判据（"0 差异"的两种解释堵不住，
// skill rgl-test-discipline 阳性锚点）。本靶提供**亚像素宽度的高对比边缘**：无 TAA
// 时这些边缘随亚像素机位移动逐帧跳变（闪烁在场 = 阳性锚点），开 TAA 后时域累积把
// 逐帧差压低一个量级。判据本体 = `imgqa temporal`。
//
// ## 版面（局部坐标，**面朝 +Z ⇒ 相机沿 −Z 看它**；米制）
//
// 上半 = 白底画板（z=-0.003）+ 画在其上的黑色图元（z=0 前层，靠深度分层压过白底）：
//   ① 垂直细杆宽度阶梯：12 根黑杆，宽 0.0015→0.015 线性爬升（同一张图上同时覆盖
//      亚像素/1px/超像素三档，"跌破 1px"不必换机位就能看见）；
//   ② 对角细杆 ±45°：水平/垂直边会被像素网格天然对齐，斜线才暴露采样相位差；
//   ③ 同心细环 + 放射线：切线方向各角度全覆盖 = 各向异性检验。
// 下半 = 透视收缩栅栏（无白底，白杆压黑背景）：5 排 × 13 根等间距细杆沿 −Z 排开，
// 远处自然跌破 1px（时域累积后应稳定，不再"变灰/断线"）。
//
// **上下两半在屏幕上也分开**（画板投在上半、栅栏投在下半）——早期版本把三组元素全
// 摆在 x=0 一条线上，沿 −Z 看过去全投到同一列像素，整屏只剩一根杆（实测踩过）。
// 故"元素在 X 上摊开 + 两半 y 保号分离"由单测钉死（tests/test_aa_target.cppm）。
//
// 推荐机位（判据配置即用此值，imgqa temporal 同源）：`position = (0, 0, 2)`、
// `target = (0, 0, -1)`、fov 45°、1280×720 ⇒ 画板平面 ≈435 px/m ⇒ ①的最细杆 0.65px。
//
// 两个 primitive 按颜色分（黑白两值高对比，材质 baseColor 给色，unlit ⇒ 像素判据
// 只反映几何采样，无光照变量）。forward / deferred 两条路都画得出（deferred 经
// deferred_geometry 落 G-buffer，受光 ⇒ 判据仍跑 forward，理由同上）。

/** 细线靶的全部常量（单测按名钉死，改动 = 显式决定）。 */
namespace aaTarget {

export inline constexpr std::string_view kModelKey = "builtin:aa_target";  ///< UploadedAssets 去重表里的合成模型键

export inline constexpr float kPlateMinX = -1.0f;    ///< 白底画板左边界
export inline constexpr float kPlateMaxX = 1.0f;     ///< 白底画板右边界
export inline constexpr float kPlateMinY = 0.05f;    ///< 白底画板下边界（画板只占上半屏）
export inline constexpr float kPlateMaxY = 0.62f;    ///< 白底画板上边界
export inline constexpr float kPlateZ = -0.003f;     ///< 白底深度（图元在 z=0 前层压过它）

export inline constexpr uint32_t kLadderRods = 12;    ///< ① 垂直细杆根数
export inline constexpr float kLadderX0 = -0.95f;     ///< ① 首杆中心 x
export inline constexpr float kLadderDx = 0.05f;      ///< ① 杆间距
export inline constexpr float kLadderY = 0.34f;       ///< ① 杆中心 y
export inline constexpr float kLadderHeight = 0.42f;  ///< ① 杆高
export inline constexpr float kLadderWMin = 0.0015f;  ///< ① 首杆宽（0.65px @推荐机位 = 亚像素）
export inline constexpr float kLadderWMax = 0.015f;   ///< ① 末杆宽（6.5px = 超像素）

export inline constexpr uint32_t kCrossSets = 2;      ///< ② 方向组数（+45° / −45°）
export inline constexpr uint32_t kCrossRods = 6;      ///< ② 每组杆数
export inline constexpr float kCrossLength = 0.30f;   ///< ② 杆长
export inline constexpr float kCrossWidth = 0.004f;   ///< ② 杆宽（1.7px）
export inline constexpr float kCrossSpacing = 0.05f;  ///< ② 组内垂直间距
export inline constexpr glm::vec2 kCrossCenter{0.62f, 0.34f};  ///< ② 中心

export inline constexpr glm::vec2 kRingCenter{0.0f, 0.34f};  ///< ③ 环心
export inline constexpr uint32_t kRingCount = 6;             ///< ③ 同心环数
export inline constexpr float kRingRadius0 = 0.05f;          ///< ③ 首环半径
export inline constexpr float kRingDr = 0.035f;              ///< ③ 环半径间距
export inline constexpr uint32_t kRingSegments = 48;         ///< ③ 每环分段
export inline constexpr float kRingWidth = 0.004f;           ///< ③ 环带宽 = 放射线宽
export inline constexpr uint32_t kSpokeCount = 16;           ///< ③ 放射线数
export inline constexpr float kSpokeInner = 0.03f;           ///< ③ 放射线内径
export inline constexpr float kSpokeOuter = 0.24f;           ///< ③ 放射线外径

export inline constexpr uint32_t kFenceRows = 5;      ///< 栅栏排数（沿 −Z）
export inline constexpr uint32_t kFenceRods = 13;     ///< 每排杆数（沿 X 摊开）
export inline constexpr float kFenceZ0 = -0.6f;       ///< 首排深度
export inline constexpr float kFenceDz = 1.6f;        ///< 排间距（末排距机位 9m ≈97 px/m）
export inline constexpr float kFenceX0 = -0.84f;      ///< 每排首杆 x
export inline constexpr float kFenceDx = 0.14f;       ///< 杆间距
export inline constexpr float kFenceY = -0.30f;       ///< 杆中心 y（投在下半屏）
export inline constexpr float kFenceHeight = 0.30f;   ///< 杆高
export inline constexpr float kFenceWidth = 0.008f;   ///< 杆宽（首排 2.7px → 末排 0.78px）

}  // namespace aaTarget

/** 四边形（4 角 + 颜色位）——合成期的中间表示，单测也按它读（三角形两两成 quad）。 */
export struct TargetQuad {
	std::array<glm::vec3, 4> c{};
	bool white = true;
};

/** 合成产物：四边形列表（**顺序即产物顺序**：白件在前黑件在后，两两三角形可复原）。 */
export struct TargetShape {
	std::vector<TargetQuad> quads;
};

/** 合成本体（确定性算术，无随机、无外部状态）。 */
TargetShape buildTargetShape() {
	TargetShape shape;
	auto& q = shape.quads;
	const auto push = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, bool white) {
		q.push_back(TargetQuad{{{a, b, c, d}}, white});
	};
	// 面朝 +Z 的轴对齐细杆（中心 cx,cy,cz；宽 w，高 h，深度 z）
	const auto bar = [&](float cx, float cy, float z, float w, float h, bool white) {
		const float hw = 0.5f * w, hh = 0.5f * h;
		push({cx - hw, cy - hh, z}, {cx + hw, cy - hh, z}, {cx + hw, cy + hh, z},
			{cx - hw, cy + hh, z}, white);
	};
	// 中心 (cx,cy)、深度 z、沿方向 (dx,dy) 的斜条：长 len、宽 w
	const auto slanted = [&](float cx, float cy, float z, float dx, float dy, float len,
		float w, bool white) {
		const float inv = 1.0f / std::sqrt(dx * dx + dy * dy);
		const float ux = dx * inv, uy = dy * inv;                ///< 沿杆方向
		const float nx = -uy * (0.5f * w), ny = ux * (0.5f * w);  ///< 法向 × 半宽
		const float hx = ux * (0.5f * len), hy = uy * (0.5f * len);
		push({cx - hx - nx, cy - hy - ny, z}, {cx - hx + nx, cy - hy + ny, z},
			{cx + hx + nx, cy + hy + ny, z}, {cx + hx - nx, cy + hy - ny, z}, white);
	};

	// ⓪ 白底画板（唯一的大面；黑色图元靠 z 分层压在上面）。它同时给判据一个
	// "靶在哪"的大区域——没有它，黑底黑杆的归属只能靠机位几何反推。
	push({aaTarget::kPlateMinX, aaTarget::kPlateMinY, aaTarget::kPlateZ},
		{aaTarget::kPlateMaxX, aaTarget::kPlateMinY, aaTarget::kPlateZ},
		{aaTarget::kPlateMaxX, aaTarget::kPlateMaxY, aaTarget::kPlateZ},
		{aaTarget::kPlateMinX, aaTarget::kPlateMaxY, aaTarget::kPlateZ}, true);

	// ① 垂直细杆宽度阶梯（黑杆压白底；线性宽度爬升，亚像素→超像素同图可见）
	for (uint32_t i = 0; i < aaTarget::kLadderRods; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(aaTarget::kLadderRods - 1u);
		const float w = aaTarget::kLadderWMin + t * (aaTarget::kLadderWMax - aaTarget::kLadderWMin);
		bar(aaTarget::kLadderX0 + static_cast<float>(i) * aaTarget::kLadderDx, aaTarget::kLadderY,
			0.0f, w, aaTarget::kLadderHeight, false);
	}
	// ② 对角细杆 ±45°（两组各 kCrossRods 根，沿各自法向平移排开）
	constexpr float kDiag = 0.70710678f;
	for (uint32_t s = 0; s < aaTarget::kCrossSets; ++s) {
		const float sgn = (s == 0u) ? 1.0f : -1.0f;
		for (uint32_t i = 0; i < aaTarget::kCrossRods; ++i) {
			const float off = (static_cast<float>(i)
				- 0.5f * static_cast<float>(aaTarget::kCrossRods - 1u)) * aaTarget::kCrossSpacing;
			// 方向 (sgn, 1) 的法向 = (-1, sgn)/√2
			slanted(aaTarget::kCrossCenter.x - off * kDiag,
				aaTarget::kCrossCenter.y + off * sgn * kDiag, 0.0f, sgn, 1.0f,
				aaTarget::kCrossLength, aaTarget::kCrossWidth, false);
		}
	}
	// ③ 同心细环（圆环带：内外半径逐段接四边形）+ 放射线
	for (uint32_t r = 0; r < aaTarget::kRingCount; ++r) {
		const float r0 = aaTarget::kRingRadius0 + static_cast<float>(r) * aaTarget::kRingDr;
		const float r1 = r0 + aaTarget::kRingWidth;
		for (uint32_t s = 0; s < aaTarget::kRingSegments; ++s) {
			const float a0 = 6.28318531f * static_cast<float>(s) / static_cast<float>(aaTarget::kRingSegments);
			const float a1 = 6.28318531f * static_cast<float>(s + 1u) / static_cast<float>(aaTarget::kRingSegments);
			const auto pt = [&](float rad, float a) {
				return glm::vec3{aaTarget::kRingCenter.x + rad * std::cos(a),
					aaTarget::kRingCenter.y + rad * std::sin(a), 0.0f};
			};
			push(pt(r0, a0), pt(r1, a0), pt(r1, a1), pt(r0, a1), false);
		}
	}
	for (uint32_t s = 0; s < aaTarget::kSpokeCount; ++s) {
		const float a = 6.28318531f * static_cast<float>(s) / static_cast<float>(aaTarget::kSpokeCount);
		const float rm = 0.5f * (aaTarget::kSpokeInner + aaTarget::kSpokeOuter);
		slanted(aaTarget::kRingCenter.x + rm * std::cos(a),
			aaTarget::kRingCenter.y + rm * std::sin(a), 0.0f, std::cos(a), std::sin(a),
			aaTarget::kSpokeOuter - aaTarget::kSpokeInner, aaTarget::kRingWidth, false);
	}
	// ④ 透视收缩栅栏（白杆压黑背景；沿 −Z 逐排后退 → 投影宽度逐排跌破 1px）
	for (uint32_t row = 0; row < aaTarget::kFenceRows; ++row) {
		const float z = aaTarget::kFenceZ0 - static_cast<float>(row) * aaTarget::kFenceDz;
		for (uint32_t i = 0; i < aaTarget::kFenceRods; ++i)
			bar(aaTarget::kFenceX0 + static_cast<float>(i) * aaTarget::kFenceDx, aaTarget::kFenceY,
				z, aaTarget::kFenceWidth, aaTarget::kFenceHeight, true);
	}
	return shape;
}

/** 四边形 → 2 三角形追加进去索引顶点流（法线恒 +Z、uv 恒 0、切线恒 +X）。
 *  （module linkage：不导出、**非**匿名 namespace——export 函数的函数内 static
 *  初始化器引用 TU-local 实体会触发 -WTU-local-entity-exposure，实测踩中两次） */
void appendQuad(std::vector<ModelVertex>& out, const TargetQuad& q) {
	const auto vert = [](glm::vec3 p) {
		ModelVertex v{};
		v.position = p;
		v.normal = glm::vec3(0.0f, 0.0f, 1.0f);
		v.uv = glm::vec2(0.0f);
		v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
		return v;
	};
	out.insert(out.end(), {vert(q.c[0]), vert(q.c[1]), vert(q.c[2]),
		vert(q.c[0]), vert(q.c[2]), vert(q.c[3])});
}

/** 顶点集 → 保守包围球（球心 = 顶点盒中心，半径 = 到最远顶点距离；同 loader 口径）。 */
BoundingSphere boundsOf(const std::vector<ModelVertex>& v) {
	BoundingSphere b;
	if (v.empty())
		return b;
	glm::vec3 lo = v[0].position, hi = v[0].position;
	for (const auto& p : v)
		for (uint32_t a = 0; a < 3u; ++a) {
			lo[a] = std::min(lo[a], p.position[a]);
			hi[a] = std::max(hi[a], p.position[a]);
		}
	b.center = 0.5f * (lo + hi);
	for (const auto& p : v)
		b.radius = std::max(b.radius, glm::length(p.position - b.center));
	return b;
}

/** 合成产物 → GltfModel（黑白两 primitive；unlit、无纹理、双面）。 */
GltfModel toGltfModel(const TargetShape& shape) {
	std::vector<ModelVertex> white, black;
	for (const auto& q : shape.quads)
		appendQuad(q.white ? white : black, q);
	std::vector<ModelVertex> all;  // 世界聚合球用（nodeWorld 全单位阵 ⇒ 局部并集 = 世界球）
	all.reserve(white.size() + black.size());
	all.insert(all.end(), white.begin(), white.end());
	all.insert(all.end(), black.begin(), black.end());
	const BoundingSphere aggregate = boundsOf(all);

	GltfModel model;
	const auto addPrim = [&](std::vector<ModelVertex>&& v, glm::vec4 color, int matIndex) {
		GltfPrimitive prim;
		prim.vertices = std::move(v);
		prim.triangleCount = static_cast<uint32_t>(prim.vertices.size() / 3u);
		prim.bounds = boundsOf(prim.vertices);
		prim.baseColor = color;
		prim.materialIndex = matIndex;
		prim.unlit = true;  // 走 scene.lua styles.unlit → model.frag（baseColor × 白纹理）
		prim.doubleSided = true;
		prim.metallic = 0.0f;
		prim.roughness = 1.0f;
		model.primitives.push_back(std::move(prim));
	};
	addPrim(std::move(white), glm::vec4{1.0f}, 0);
	addPrim(std::move(black), glm::vec4{0.0f, 0.0f, 0.0f, 1.0f}, 1);
	model.bounds = aggregate;  // SceneBounds 靠它定自适应 near/far
	return model;
}

/** 细线靶模型（首调合成 + 函数内 static = 线程安全一次性；只读消费）。 */
export inline const GltfModel& aaTargetModel() {
	static const GltfModel model = toGltfModel(buildTargetShape());
	return model;
}

/** 细线靶四边形表（单测读它钉几何不变式；生产路径只用 aaTargetModel）。 */
export inline const TargetShape& aaTargetShape() {
	static const TargetShape shape = buildTargetShape();
	return shape;
}

}  // namespace RGL
