module;
/**
 * test_rebase_consumers —— 相对坐标批 4 第三刀：把「自动跟 / 不用改」那张表变成判据
 *
 * ## 为什么有本文件
 * 批 4 前两刀接完 view / eyePos / 灯 / 实例矩阵 / TAA / velocity 后，剩下的消费者全部被判成
 * 「不用改」或「自动跟」。那份判定**当时只是主 agent 的推理，一条判据都没有**。而「不用改」的
 * 清单里只要有一条其实是「漏改而恰好没测出来」，就是最贵的那种假绿（门关态原点恒 0 ⟹ 漏改
 * 在门关**结构上测不出来**，见 docs/相对坐标地基_plan.md §3 批 4 判据表第 ② 条的理由段）。
 * 本文件把其中四条变成闭式断言。
 *
 * ## 判据 1（本文件最要紧的一条）：texelSizes 整链与相机姿态/位置**逐位无关**
 * 链条 = `frustumBoundingSphere` 的 radius → `buildShadowVolume` 的 `xyRadius` →
 * `fillShadowData` 写进 scene data 的 `shadowTexelSizes` → `rebaseSnapGranularity`。
 * 🔴 它是批 5「原点 snap 粒度不依赖原点、所以不存在帧序循环」这个设计前提的**唯一凭据**：
 * 粒度若依赖相机位置，就依赖原点（原点由相机算出），而原点又由粒度 snap 出来 ⟹ 循环定义。
 * 半径的闭式解里**根本没有 view 这个入参**（5a-5/5a-6 换闭式解就是为了消掉姿态依赖），
 * 但**下游整条链当时无人守**——`buildShadowVolume` 里那个世界锚定 snap 一旦哪天改成回写
 * `xyRadius`（看着像"收紧包围盒"的合理优化），粒度就重新沾上世界位置，而全仓不会有任何东西红。
 *
 * ## 期望值出处（独立于被测实现）
 *  - 判据 1 的期望 = **第一条记录**（同档内第一个 view 的产物）+ 「与 view 无关」这个几何事实
 *    （闭式解 r = √((f−zc)² + rf²) 的自变量只有 n/f/fov/aspect，出处 = scene_context 该函数
 *    注释里那两行推导）。反向半边（跨档必须变）的期望 = `quantizeShadowDistance` 的档位定义。
 *  - 判据 2 的期望 = **视锥的几何定义**（对称透视六面：左右面过眼点、法线 =
 *    (∓R + tanθx·F)/√(1+tan²θx)，上下面同形，近远面 = dot(d,F) ∓ n/f）。这是从"什么叫在视锥内"
 *    推出来的，**不是** `extractFrustum` 的 Gribb-Harman 行组合式子——两边同抄就验不出符号约定。
 *  - 判据 3 的期望 = **基准姿态的可见簇下标序列本身**（逐元素比，出处 = skill「对账伙伴必须钉
 *    序/映射，不能只钉聚合量」）+ 一条「两侧不同空间必须立刻分家」的错位锚点（出处 = 任务书
 *    点名的静默错位形态）。本条不再叠一层闭式：判据 2 已经在闭式上对过账，这里重复一遍
 *    只会多一个会漂的副本。
 *  - 判据 4 的期望 = 「back-to-front 顺序只依赖相对位置」这个几何事实 + 键的 float32 舍入
 *    误差上界（闭式：2·max|d|·ULP(|绝对量|)，出处 = IEEE 的 ULP 定义）。生产式本身
 *    （`tf.world[3] − camera.position` 两项同空间）是契约，出处 = 批 4 任务书「不用改」表。
 *
 * ## 判据 4 的实测修正（本批对任务书那张表的一条）
 * 任务书写的是「断言场景与相机一起平移 Δ 前后排序顺序逐位相同」。**这条不是无条件成立的**：
 * 键在 float 域算（`float(tf.world[3]) − float(camera.position)` 再点积），两个绝对量一起变大时
 * 这个差的舍入误差按 ULP(|绝对量|) 一起长大，一旦超过「最近一对透明体的键差」，顺序就会翻。
 * 实测（写死种子、200 个透明体、最小键差 7.568e-3）：|Δ|≈1.4e2 时误差界 2.93e-3 < 键差 ⟹ 翻 0 对；
 * |Δ|≈1.1e5 时界 1.50 ⟹ 翻 1 对；|Δ|≈3.7e6 时界 48.0 ⟹ 翻 25 对，且**被翻的对全部**落在界内。
 * ⟹ 本文件把它拆成两条：射程内（界 < 键差）逐位相同、射程外只翻近邻对。
 * ⚠ 这不是漏改：本键**不吃渲染原点**，rebase 不会让它更坏。但它因此也**拿不到 rebase 的精度收益**
 * ——债-85 触发点（|camera| ≈ 7.6e5）之上近平局的透明体会随相机位置重排。要不要改成 double 域
 * 算差再比是独立一笔，不在本批。
 *
 * ## 变异红线声明（每条「改坏什么 → 哪条用例红」，全部**实测**，读数 → 交付报告）
 * 回滚只用不带 -p 的 cp；构建返回码单独取（clang 22 抖动重跑），绝不把「没编过」读成「测试绿」。
 * 括号里是实测的红断言数；「旧」= 本文件之外已有的用例。
 *  - `frustumBoundingSphere` 的 radius 沾上 |view[3]|（= 5a-7 那个「8 角世界 AABB」旧法的等价
 *      失效）→ 「texelSizes 整链平移不变」红 56 条（旧：同档两机位那条也红 4 条）
 *  - `buildShadowVolume` 把吸附后的量回写进 `xyRadius`（看着像收紧包围盒）
 *      → 「texelSizes 整链平移不变」红 51 条
 *  - `fillShadowData` 的 texels 循环漏填末级
 *      → 「texelSizes 整链平移不变」红 3 条（形状半边：四级全 > 0 + 粒度 ∈ [max, 2·max)）
 *      ⚠ **这条缺口是变异跑出来的**：只验「跨 view 不变」时漏填一级**照样全绿**，
 *      于是补了形状半边（四级全 > 0 且逐级递增）。别把那段读成冗余。
 *  - `rebaseSnapGranularity` 退化成常数（不再吃 texelSizes）
 *      → 「判据 1 的反向半边」红 2 条 + 「texelSizes 整链」红 1 条
 *  - `rebaseSnapGranularity` 取第一个非零而非最粗
 *      → 「texelSizes 整链」红 1 条（粒度 ∉ [max, 2·max) 那条抓住它；反向半边**不红**，
 *        因为两组 texel 的第一个非零值恰好同档 —— 别把反向半边读成覆盖了「取最粗」）
 *  - `extractFrustum` 的归一化口径漂（距离不再是世界单位）→ 「CPU 剔除可见集」红 16 条
 *  - `extractFrustum` 的 near 面改用 [-1,1] 约定（row[3] + row[2]）
 *      → 「判据 2 的约定样本」红 3 条。⚠ **这条缺口也是跑出来的**：随机球的深度都在 1…100，
 *      而该变异只把近截面从 n 挪到 ≈ n/2（挪 0.05），原有三条用例全绿 ⟹ 补了贴边界的约定样本。
 *  - `sphereInFrustum` 的 `<` 改成 `<=`（保守判据变严格）
 *      → **本文件不红**，全库红 2 条（`test_meshlet_cull` 的「相切」与「零半径球」用例）。
 *      如实记：相切边界已由那里逐位钉死，本文件不重复钉——重复一遍 = 多一个会漂的副本。
 *  - `cullClustersLinear` 丢掉对象矩阵的平移列（= 场景平移不跟的漏改形态）
 *      → 「判据 3 的错位锚点」红 2 条（**平移不变那条不红**：两侧一起丢平移仍然自洽，
 *        抓住它的是空间口径锚点，不是不变性）
 *  - `toRelativeModel` 忽略原点（「接了但没接上」）
 *      → 本文件红 2 条用例 / 8 条断言（meshlet 平移不变 6 条 + 错位锚点 2 条）
 *  - 判据 4 的排序键是**测试侧转录**：改生产式（`tf.world[3]` → 相对矩阵）**本文件不红**。
 *      覆盖边界写在该转录的注释里，守生产点仍只有出厂截图锚（批 3 已登记同一形态）。
 */
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // glm::lookAt / perspective（测试侧独立构造机位）
#include <cmath>                         // std::sqrt / std::tan / std::isfinite
#include <cstring>                       // memcmp（「逐位」按字面判）
#include <initializer_list>              // modules 坑：花括号列表不从 import 带进来

export module test_rebase_consumers;

import rlstd;
import test_support;      // testing::Lcg / expectNear
import scene_context;     // frustumBoundingSphere / buildShadowVolume / extractFrustum / sphereInFrustum / Light / ViewProjection
import scene_data;        // kActiveShadowCascades
import render_graph;      // shadowAtlasColumns / shadowTileSize
import render_list;       // computeCascadeSpheres / fillShadowData / shadowTexelSize
import frame_draw_list;   // FrameDrawList（fillShadowData 的出参）
import coordinate_rebase; // rebaseSnapGranularity / toRelativeModel
import meshlet_cull;      // cullClustersLinear（★0.5-a CPU 参考簇剔除器）
import meshlet_asset;     // MeshletAsset / MeshletDiskCluster

namespace RGL {
namespace {

using testing::Lcg;

/** 门关原点 = 逐位 (0,0,0)（相对坐标批 5 第 1 刀给 `buildShadowVolume` 加的 origin 入参）。
 *  本文件的用例全部测**门关态**的几何规则，显式传它而不是留默认值：漏传不会让任何判据红，
 *  而「门关逐位不变」这条判据的语义就是「原点这一维真的没参与」。 */
const glm::dvec3 kNoOrigin{0.0, 0.0, 0.0};

/** 属性测试种子（写死；换种子 = 换一组输入，不是「重试」）。 */
constexpr uint32_t kSeed = 20261025u;

/** 本文件自取的投影参数（fov 60° / aspect 16:9）。🔴 **不是出厂档**——出厂 `configs/camera.lua`
 *  的 `fovDegrees = 45.0`（2026-10-25 批 5 量具批实测纠正，原注释误称「交付档」）。本文件全部
 *  用例断言的是**平移不变性**，对任意 fov 成立 ⟹ 用 60° 不影响结论，且刻意与出厂档取不同值，
 *  免得判据悄悄只在对出厂 fov 下才成立。 */
constexpr float kFovYDeg = 60.0f;
constexpr float kAspect = 16.0f / 9.0f;

/** 主光（太阳向下，与 configs/shadow.lua 同形）。判据 1 只吃它的 direction。 */
constexpr Light kSun{.index = 0u, .direction = glm::vec3{0.35f, -1.0f, 0.25f},
	.color = glm::vec3{1.0f, 0.98f, 0.95f}, .intensity = 3.0f, .ambient = glm::vec3{0.03f},
	.position = glm::dvec3{0.0}, .range = 0.0f, .type = 0u};

/**
 * 一组「同档 near/far + 不同 view 姿态与位置」的样本集。
 *
 * 位置刻意跨 1e-1 … 1e6 五个量级：门关态原点恒 0 时 eye 就在 1e6 量级（交付机位 |camera|≈625，
 * 债-85），门开态 eye 会塌到原点附近。粒度必须对**这两种世界**给出同一个数，否则批 5 的
 * 「先算粒度再定原点」变成循环定义。姿态含 pitch ±60（阴影侧最怕的俯视/仰视）。
 */
struct PoseSample {
	glm::vec3 eye;
	glm::vec3 target;
};

std::vector<PoseSample> makePoses() {
	std::vector<PoseSample> out;
	Lcg rng{kSeed};
	for (int i = 0; i < 40; ++i) {
		// 距离取 2 的幂档 × 随机系数：0.1 … 2e6 五个量级
		const double dist = std::pow(10.0, rng.uniform(-1.0, 6.3));
		const double yaw = rng.uniform(-3.14, 3.14);
		const double pitch = rng.uniform(-1.05, 1.05);  // ±60°，避开 lookAt 的 up∥视线退化
		const glm::vec3 dir{static_cast<float>(std::cos(pitch) * std::sin(yaw)),
			static_cast<float>(std::sin(pitch)), static_cast<float>(std::cos(pitch) * std::cos(yaw))};
		// target 不取原点：eye 在原点附近时 target 也一起走（否则 40 组里大半是同一个 view）
		const glm::vec3 tgt{static_cast<float>(rng.uniform(-30.0, 30.0)),
			static_cast<float>(rng.uniform(-30.0, 30.0)), static_cast<float>(rng.uniform(-30.0, 30.0))};
		out.push_back(PoseSample{tgt + dir * static_cast<float>(dist), tgt});
	}
	return out;
}

/** 由 PoseSample 造 view（up = 世界 Y；样本集已避开 up∥视线）。 */
glm::mat4 viewOf(const PoseSample& p) {
	return glm::lookAt(p.eye, p.target, glm::vec3{0.0f, 1.0f, 0.0f});
}

/** 交付档 proj（fov/aspect 固定；near/far 由调用点给，级联子矩阵另走 projWithDepthRange）。 */
glm::mat4 projOf(float nearZ, float farZ) {
	return glm::perspective(glm::radians(kFovYDeg), kAspect, nearZ, farZ);
}

/**
 * 整链条形：给定 view + **量化后**的深度界，跑
 * `computeCascadeSpheres → buildShadowVolume → fillShadowData`，返回写进 scene data 的
 * `shadowTexelSizes` 与 `rebaseSnapGranularity` 的产物。
 *
 * 🔴 这条链是**生产链的形状**，不是简化版：`buildShadowVolume` 的入参 casterMin/Max 也一起
 * 平移（调用方给），这样「吸附后的半宽回写进 xyRadius」那种变异才会被世界位置暴露出来。
 */
struct TexelChain {
	glm::vec4 texels{0.0f};
	double granularity = 0.0;
};

TexelChain texelChain(const glm::mat4& view, float shadowNear, float shadowFar,
	const glm::vec3& casterMin, const glm::vec3& casterMax) {
	ViewProjection vp;
	vp.view = view;
	vp.proj = projOf(shadowNear, shadowFar);
	vp.nearPlane = shadowNear;
	vp.farPlane = shadowFar;
	const auto spheres = computeCascadeSpheres(vp, shadowNear, shadowFar);
	const uint32_t snapGrid = shadowTileSize(shadowAtlasColumns(kActiveShadowCascades));
	std::array<ShadowVolume, kActiveShadowCascades> volumes{};
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		volumes[c] = buildShadowVolume(kSun, spheres[c], casterMin, casterMax, snapGrid, kNoOrigin);
	FrameDrawList list;
	fillShadowData(list, shadowNear, shadowFar, {volumes.data(), volumes.size()}, true, -1);
	TexelChain out;
	out.texels = list.sceneData.shadowTexelSizes;
	out.granularity = rebaseSnapGranularity(std::span<const float>{&out.texels[0], 4});
	return out;
}

/** vec4 逐位相同（memcmp：+0.0 与 −0.0 判不同——「逐位」两字不许读成「值相等」）。 */
bool bitwiseEqual(const glm::vec4& a, const glm::vec4& b) {
	return std::memcmp(&a, &b, sizeof(glm::vec4)) == 0;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 判据 1：texelSizes 整链与相机姿态/位置逐位无关（批 5 粒度设计的唯一凭据）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("🔴 texelSizes 整链平移不变：同档 near/far 换任意 view 姿态/位置，texels 与粒度逐位不变") {
	// 这条钉的是批 5 的设计前提：原点 snap 粒度**不依赖原点**，所以「先算粒度 → 再定原点」
	// 不是循环定义。链条走完 fillShadowData（scene data 字节）才停，不在半径处停 ——
	// 半径早就有人守（test_shadow_geometry「姿态无关性」），**下游整链此前无人守**。
	// 期望值出处 = 第一个 view 的产物 + 「闭式半径的自变量里没有 view」这个几何事实。
	const float nearQ = quantizeShadowDistance(0.05f);   // 交付档自适应 near 的下限
	const float farQ = quantizeShadowDistance(1.713496f);  // 债-139 实测机位的量化远界
	REQUIRE(nearQ > 0.0f);
	REQUIRE(farQ > nearQ);

	const auto poses = makePoses();
	REQUIRE(poses.size() == 40);  // 空集合守卫：循环断言在空集上恒真

	// 投影体 AABB 也跟着一起平移（每个样本换一个位置，量级到 1e6）
	Lcg rng{kSeed + 1u};
	const TexelChain first = texelChain(viewOf(poses[0]), nearQ, farQ,
		glm::vec3{-4.0f, -1.0f, -3.0f}, glm::vec3{5.0f, 9.0f, 6.0f});
	REQUIRE(first.texels.x > 0.0f);      // 前提：链子真产出了纹素（全 0 会让下面全部断言假绿）
	REQUIRE(std::isfinite(first.granularity));

	// 🔴 **形状半边**（M3 变异实测逼出来的：只验「跨 view 不变」时，「四级里漏填一级」
	// 那种实现照样逐位不变 ⟹ 全绿）。这里钉的是 coordinate_rebase::rebaseSnapGranularity
	// 注释所依赖的那条事实：`fillShadowData` 把**四级 texels 全部填满**（0 在现网不出现），
	// 且级联半径逐级递增 ⟹ 纹素逐级递增、最粗级在末槽。
	// 期望值出处 = 闭式：级联 c 的远界 cascadeSplitDistance(n,f,c) 随 c 严格递增，
	// 而半径是远界的单调函数（r ≈ f/2·(1+cornerScale²) 量级）⟹ 纹素同序。
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		CHECK(first.texels[c] > 0.0f);  // 未填的槽 = 0 ⟹ 本条红（M3 的抓手）
		if (c + 1u < kActiveShadowCascades)
			CHECK(first.texels[c] < first.texels[c + 1u]);
	}
	// 粒度 = 「向上取到 2 的幂」作用在**最粗级纹素**（= 数组最大值）上的闭式契约：
	// g ∈ [max, 2·max)。缺一级时 max 变小、g 跟着变小，这两条一起红。
	const float coarsest = first.texels[kActiveShadowCascades - 1];
	CHECK(first.granularity >= coarsest);
	CHECK(first.granularity < 2.0 * coarsest);

	int checked = 0;
	for (size_t i = 1; i < poses.size(); ++i) {
		const glm::vec3 shift{static_cast<float>(rng.uniform(-1e6, 1e6)),
			static_cast<float>(rng.uniform(-1e3, 1e3)), static_cast<float>(rng.uniform(-1e3, 1e3))};
		const TexelChain got = texelChain(viewOf(poses[i]), nearQ, farQ,
			glm::vec3{-4.0f, -1.0f, -3.0f} + shift, glm::vec3{5.0f, 9.0f, 6.0f} + shift);
		INFO("i = " << i << " eye = " << poses[i].eye.x << "," << poses[i].eye.y << ","
					<< poses[i].eye.z << " texels = " << got.texels.x << "," << got.texels.y
					<< "," << got.texels.z << "," << got.texels.w);
		// 🔴 逐位（memcmp），不是容差：粒度是 2 的幂格距的输入，容差会被谈到通过为止
		CHECK(bitwiseEqual(got.texels, first.texels));
		CHECK(got.granularity == first.granularity);
		++checked;
	}
	CHECK(checked == 39);  // 空转守卫：上面那个循环必须真跑满
}

TEST_CASE("判据 1 的反向半边：跨 near/far 档时粒度**必须变**（否则上一条是「粒度与一切无关」的假绿）") {
	// 先例 = test_shadow_geometry 里「同档两机位 ⟹ 各级级联半径与纹素逐位相同」那条的反向写法。
	// 没有这条，「换 view 粒度不变」会被后来人读成「粒度是个常数」——而常数粒度正是债-85 ②
	// 要拦的形态（不随最粗级纹素走）。
	const float nearQ = quantizeShadowDistance(0.05f);
	const glm::mat4 view = viewOf(makePoses()[0]);
	const glm::vec3 lo{-4.0f, -1.0f, -3.0f};
	const glm::vec3 hi{5.0f, 9.0f, 6.0f};

	const float farA = quantizeShadowDistance(1.713496f);
	const float farB = quantizeShadowDistance(2.5f);
	REQUIRE(farA != farB);  // 前提：两组确实跨档（同档的话本用例失去判据力）

	const TexelChain a = texelChain(view, nearQ, farA, lo, hi);
	const TexelChain b = texelChain(view, nearQ, farB, lo, hi);
	CHECK(a.granularity != b.granularity);
	CHECK(std::isfinite(a.granularity));
	CHECK(std::isfinite(b.granularity));
	// 闭式方向：远界更大 ⟹ 最粗级纹素更大 ⟹ 粒度不减（量化只向上取 2 的幂）
	CHECK(b.granularity > a.granularity);
	// 且两者都是 2 的幂（rebaseSnapGranularity 的格点嵌套前提，出处 = quantizeShadowDistance
	// 的档位定义：frexp 尾数 0.5 ⟺ 2^k）
	for (const double g : {a.granularity, b.granularity}) {
		int exponent = 0;
		CHECK(std::frexp(g, &exponent) == 0.5);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// 判据 2：CPU 剔除（视锥 6 面）的可见集逐球平移不变
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/**
 * 对称透视视锥的**独立第二实现**（期望值出处 = 「什么叫在视锥内」的几何定义）。
 *
 * 🔴 刻意**不抄** `extractFrustum` 的 Gribb-Harman 行组合式子：两边同抄一个实现，符号约定
 * 写反也照样"对得上"（skill rgl-test-discipline「断言独立于实现」）。这里从相机基向量直接
 * 写六个面的内法线：左右面过眼点、与视轴夹 halfAngle，近远面垂直视轴。
 * 归一化后 `dot(n, p−eye) + offset` 就是**世界单位带符号距离**，与 `extractFrustum`
 * 归一化后的 `plane·(x,1)` 同口径（两边都是真距离，才能直接对账判据）。
 */
struct ClosedFrustum {
	glm::dvec3 eye, fwd, right, up;
	double nearD, farD, tanX, tanY;
};

/** 由机位与投影参数造闭式视锥（up 固定世界 Y，与生产相机同形）。 */
ClosedFrustum makeClosed(const glm::dvec3& eye, const glm::dvec3& target, double nearD,
	double farD, double fovYRad, double aspect) {
	const glm::dvec3 fwd = glm::normalize(target - eye);
	const glm::dvec3 worldUp{0.0, 1.0, 0.0};
	const glm::dvec3 right = glm::normalize(glm::cross(fwd, worldUp));
	const double tanY = std::tan(fovYRad * 0.5);
	return ClosedFrustum{eye, fwd, right, glm::cross(right, fwd), nearD, farD, tanY * aspect, tanY};
}

/** 球心到六面的带符号距离（内为正）。顺序 left/right/bottom/top/near/far 与 Frustum::planes 同词表。 */
void closedDistances(const ClosedFrustum& c, const glm::dvec3& p, double out[6]) {
	const glm::dvec3 d = p - c.eye;
	const double along = glm::dot(d, c.fwd);
	const double side = glm::dot(d, c.right);
	const double vert = glm::dot(d, c.up);
	const double kx = 1.0 / std::sqrt(1.0 + c.tanX * c.tanX);  // 未归一化法线 (±R + tanX·F) 的长度
	const double ky = 1.0 / std::sqrt(1.0 + c.tanY * c.tanY);
	out[0] = (side + along * c.tanX) * kx;   // left
	out[1] = (-side + along * c.tanX) * kx;  // right
	out[2] = (vert + along * c.tanY) * ky;   // bottom
	out[3] = (-vert + along * c.tanY) * ky;  // top
	out[4] = along - c.nearD;                // near
	out[5] = c.farD - along;                 // far
}

/** 闭式保守判据：任一面的带符号距离 < −r 才剔（跨界与内含一律留）。 */
bool closedVisible(const ClosedFrustum& c, const glm::dvec3& center, double radius) {
	double dist[6];
	closedDistances(c, center, dist);
	for (const double v : dist)
		if (v < -radius)
			return false;
	return true;
}

/** 球心到「最近边界 + 半径」的余量绝对值的最小值 = 本样本离判定翻转有多远。 */
double closedMargin(const ClosedFrustum& c, const glm::dvec3& center, double radius) {
	double dist[6];
	closedDistances(c, center, dist);
	double worst = 1e300;
	for (const double v : dist)
		worst = std::min(worst, std::abs(v + radius));
	return worst;
}

/** 判据 2/3 共用的相机参数（fov 60° / aspect 16:9 / near 0.1 / far 60）。
 *  ⚠ 弧度常量写死而不调 glm::radians：本常量曾把 60° 误填成 π/6（= 30°），症状是闭式第二
 *  实现与 `extractFrustum` 在四个侧面上系统性分家 —— 看着像被测码错了，实际是判据自己带病。
 *  ⟹ 配套一条**常量自检**（见「闭式视锥第二实现自己的自检」用例），别只靠人眼复核。 */
constexpr double kFovYRad = 1.0471975511965976;  // 60° = π/3
constexpr double kNearD = 0.1;
constexpr double kFarD = 60.0;

/** 一批剔除输入：node-local 球经对象矩阵到世界后喂给被测剔除器。 */
struct SphereBatch {
	std::vector<glm::vec3> centers;  // 世界球心（未平移那份）
	std::vector<float> radii;
};

/**
 * 造一批**离边界有安全余量**的球（写死种子）。
 *
 * 为什么要余量：float32 下 `extractFrustum` 的行组合与闭式的基向量点积是两条不同的舍入路径，
 * 恰好在边界上的样本两边给出不同判定**不是 bug**。所以判据只在「余量 ≫ 浮点噪声」的样本上求值，
 * 并把被丢弃的样本数打出来——丢弃过半 = 输入构造退化，本用例失去判据力（前提守卫）。
 */
SphereBatch makeSpheres(const glm::dvec3& eye, const glm::dvec3& target, uint32_t seed,
	int wanted, int& rejectedNearBoundary) {
	const ClosedFrustum cf = makeClosed(eye, target, kNearD, kFarD, kFovYRad, kAspect);
	SphereBatch b;
	Lcg rng{seed};
	rejectedNearBoundary = 0;
	int attempts = 0;
	while (static_cast<int>(b.centers.size()) < wanted && attempts < wanted * 60) {
		++attempts;
		const glm::dvec3 c{rng.uniform(-40.0, 40.0), rng.uniform(-25.0, 25.0),
			rng.uniform(-40.0, 40.0)};
		const double r = rng.uniform(0.15, 1.8);
		if (closedMargin(cf, c, r) < 0.05) {
			++rejectedNearBoundary;
			continue;  // 边界带内：两条舍入路径本就可能分家，不进判据
		}
		b.centers.push_back(glm::vec3{c});
		b.radii.push_back(static_cast<float>(r));
	}
	return b;
}

/** 跑一遍被测 CPU 剔除，返回逐球可见位。 */
std::vector<bool> runCull(const Frustum& f, const SphereBatch& b, const glm::vec3& delta) {
	std::vector<bool> out;
	out.reserve(b.centers.size());
	for (size_t i = 0; i < b.centers.size(); ++i)
		out.push_back(sphereInFrustum(f, b.centers[i] + delta, b.radii[i]));
	return out;
}

}  // namespace

TEST_CASE("🔴 闭式视锥第二实现自己的自检：六个面上的带符号距离对上已知几何") {
	// 判据 2/3 全靠 `closedVisible` 当期望值来源，而它**第一次就带病**：`kFovYRad` 被我写成
	// π/6（= 30°）而不是 π/3（60°），症状是 59 个球与 `extractFrustum` 分家 —— 读起来像
	// 「被测码有问题」，实际是判据自己错了（skill「验证工具自己错」的现形，且这次是实测踩到）。
	// ⟹ 期望值不许只靠人眼复核，必须有独立锚：下面每条都是**不调用闭式函数也能验**的距离值。
	// 规范机位 = 眼点原点、看向 −Z、up +Y、fovY 60°、aspect 1、near 1、far 100。
	const ClosedFrustum c = makeClosed(glm::dvec3{0.0}, glm::dvec3{0.0, 0.0, -1.0}, 1.0, 100.0,
		kFovYRad, 1.0);
	// 常量自检（本变异的直接靶）：fovY = 60° ⟹ tanY = tan(30°) = 1/√3；aspect 1 ⟹ tanX 同值
	testing::expectNear(c.tanY, 0.57735026918962573, 1e-12);
	testing::expectNear(c.tanX, 0.57735026918962573, 1e-12);
	CHECK(c.tanY != 0.26794919243112270);  // 反向：tan(15°)（= 误填 π/6 会得到的值）必须不是它

	double d[6];
	// 视轴上的点 (0,0,−50)：near = 49、far = 50，四侧面因轴对称而全等
	closedDistances(c, glm::dvec3{0.0, 0.0, -50.0}, d);
	testing::expectNear(d[4], 49.0, 1e-12);
	testing::expectNear(d[5], 50.0, 1e-12);
	for (int k = 0; k < 4; ++k)
		testing::expectNear(d[k], 50.0 * c.tanX / std::sqrt(1.0 + c.tanX * c.tanX), 1e-12);
	// 上边界上的点：y = 50·tanY 与 z = −50 的组合必须让 top 面距离**恰为 0**（点在面上）
	closedDistances(c, glm::dvec3{0.0, 50.0 * c.tanY, -50.0}, d);
	testing::expectNear(d[3], 0.0, 0.0, 1e-12);
	testing::expectNear(d[2], 100.0 * c.tanY / std::sqrt(1.0 + c.tanY * c.tanY), 1e-12);
	// 眼点：四侧面过眼点 ⟹ 距离 0；near 面外 1 个单位 ⟹ −1
	closedDistances(c, glm::dvec3{0.0}, d);
	testing::expectNear(d[4], -1.0, 1e-12);
	for (int k = 0; k < 4; ++k)
		testing::expectNear(d[k], 0.0, 0.0, 1e-12);
	// 归一化口径自检：沿视轴多走 1 单位，near 面距离必须**恰好**增加 1（单位增益
	// = 「plane·(x,1) 是世界单位带符号距离」这条口径的判据；漏归一化会给出 |A| 倍增益）
	closedDistances(c, glm::dvec3{0.0, 0.0, -51.0}, d);
	const double prev = d[4];
	closedDistances(c, glm::dvec3{0.0, 0.0, -52.0}, d);
	testing::expectNear(d[4] - prev, 1.0, 1e-12);
}

TEST_CASE("判据 2 的约定样本：近/远截面的**深度界**逐球对上（RH_ZO 的 near 在 n，不在 n/2）") {
	// 为什么单独一条：`makeSpheres` 的随机球深度都在 1…100，而「near 面用错 clip 约定」这类
	// 变异（`row[2]` ↔ `row[3]+row[2]`）只把近截面从 n 挪到 ≈ n/2 —— **挪动量 0.05**，
	// 深场景里没有一个球落在缝里，于是整条判据 2 全绿（实测 M6 就是这个读数）。
	// ⟹ 约定类判据必须有**贴着约定边界**的样本，随机采样采不到它（skill「对空集合的断言要加
	// 前提守卫」的同族：断言长在机制无能为力的输入上 = 恒真）。
	// 期望值出处 = 闭式：RH_ZO 的近平面在沿视轴 n 处（`extractFrustum` 取 clip 第 2 行，
	// 归一化后距离 = along − n）；错用 [-1,1] 约定会把它挪到 clip.z + clip.w = 0
	// ⟹ along = −B/(1−A) = n·f/(f−n)·… 实测 ≈ n/2（下面用「深度 0.07 必须判外」直接钉）。
	const glm::dvec3 eye{2.0, 6.0, 14.0};
	const glm::dvec3 target{0.0, 0.0, -2.0};
	const glm::mat4 proj = projOf(static_cast<float>(kNearD), static_cast<float>(kFarD));
	const glm::mat4 view = glm::lookAt(glm::vec3{eye}, glm::vec3{target}, glm::vec3{0.0f, 1.0f, 0.0f});
	const Frustum f = extractFrustum(proj * view);
	const ClosedFrustum closed = makeClosed(eye, target, kNearD, kFarD, kFovYRad, kAspect);

	const glm::dvec3 fwd = glm::normalize(target - eye);
	const auto atDepth = [&](double along) {
		return glm::dvec3{eye} + fwd * along;
	};
	// 深度 0.07（在 n/2 与 n 之间）、半径 0.005：真近截面 ⟹ 面外 0.03 ≫ r ⟹ **必须剔**；
	// 错约定（近截面 0.05）⟹ 面内 0.02 ≫ r ⟹ 判可见。两边余量都是 float 噪声（~1e-7）的
	// 五个数量级，不是靠容差糊过去的边界。
	for (const double along : {0.07, 0.08, 0.09}) {
		const glm::dvec3 c = atDepth(along);
		CHECK(sphereInFrustum(f, glm::vec3{c}, 0.005f) == false);
		CHECK(closedVisible(closed, c, 0.005) == false);
	}
	// 反向半边：深度 0.12 / 0.2（真在 n 之外）⟹ 必须判内。缺了它，「恒剔」的实现也能过上面三条。
	for (const double along : {0.12, 0.2, 1.0}) {
		const glm::dvec3 c = atDepth(along);
		CHECK(sphereInFrustum(f, glm::vec3{c}, 0.005f) == true);
		CHECK(closedVisible(closed, c, 0.005) == true);
	}
	// 远截面同一条：深度 far + 1 必须判外、far − 1 必须判内（far 侧的约定 = row[3] − row[2]）
	CHECK(sphereInFrustum(f, glm::vec3{atDepth(kFarD + 1.0)}, 0.005f) == false);
	CHECK(sphereInFrustum(f, glm::vec3{atDepth(kFarD - 1.0)}, 0.005f) == true);
	// 相机背后的点：near 面距离 = along − n < 0 且 |·| ≫ r ⟹ 必须剔（这条也是「near 面被写成
	// 恒真」那种错法的抓手）
	CHECK(sphereInFrustum(f, glm::vec3{eye - fwd * 5.0}, 0.1f) == false);
}

TEST_CASE("🔴 CPU 剔除可见集平移不变：场景与相机一起平移 Δ，逐球判定完全相同（含闭式对账）") {
	// 「CPU 剔除自动跟」此前只是推理：`extractFrustum(proj·view)` 现取、view 已接原点，
	// 所以视锥跟着相机走 ⟹ 一起平移不该改变任何球的可见性。本条把它变成判据。
	// 🔴 **逐球**断言，不只断言总数：总数相同而判定错位（A 换成 B）是可能的，那正是
	// skill「对账伙伴模块：判据必须钉序/映射，不能只钉聚合量」点名的形态。
	// 期望值出处 = 闭式视锥六面（上面那套独立实现），不抄被测码。
	const glm::dvec3 eye{2.0, 6.0, 14.0};
	const glm::dvec3 target{0.0, 0.0, -2.0};
	int rejected = 0;
	const SphereBatch batch = makeSpheres(eye, target, kSeed + 7u, 240, rejected);
	REQUIRE(batch.centers.size() == 240);
	CHECK(rejected < 240);  // 前提守卫：被丢弃的边界样本不得过半（否则输入构造退化）

	const glm::mat4 proj = projOf(static_cast<float>(kNearD), static_cast<float>(kFarD));
	const glm::mat4 view = glm::lookAt(glm::vec3{eye}, glm::vec3{target}, glm::vec3{0.0f, 1.0f, 0.0f});
	const Frustum base = extractFrustum(proj * view);
	const ClosedFrustum closed = makeClosed(eye, target, kNearD, kFarD, kFovYRad, kAspect);

	// ① 被测实现与闭式第二实现对账（基准姿态）
	const std::vector<bool> baseVis = runCull(base, batch, glm::vec3{0.0f});
	int visibleCount = 0;
	for (size_t i = 0; i < batch.centers.size(); ++i) {
		const bool expect = closedVisible(closed, glm::dvec3{batch.centers[i]}, batch.radii[i]);
		INFO("i = " << i << " c = " << batch.centers[i].x << "," << batch.centers[i].y << ","
					<< batch.centers[i].z << " r = " << batch.radii[i]);
		CHECK(baseVis[i] == expect);
		if (baseVis[i])
			++visibleCount;
	}
	// ② 阳性锚点：可见与不可见**两类都必须有**。缺了就退化成「全都可见 ⟹ 平移前后当然相同」
	// 的空判据（任务书点名的假绿形态）。
	CHECK(visibleCount > 0);
	CHECK(visibleCount < static_cast<int>(batch.centers.size()));

	// ③ 主判据：一起平移 Δ（三个方向，含 1e5 量级的大平移）⟹ 逐球判定不变
	for (const glm::vec3 delta : {glm::vec3{137.0f, -91.5f, 42.25f},
			 glm::vec3{1.0e5f, -3.0e4f, 7.0e3f}, glm::vec3{-0.5f, 0.25f, -0.125f}}) {
		const glm::mat4 moved = glm::lookAt(glm::vec3{glm::dvec3{eye} + glm::dvec3{delta}},
			glm::vec3{glm::dvec3{target} + glm::dvec3{delta}}, glm::vec3{0.0f, 1.0f, 0.0f});
		const Frustum shifted = extractFrustum(proj * moved);
		const std::vector<bool> movedVis = runCull(shifted, batch, delta);
		const ClosedFrustum closedMoved = makeClosed(
			glm::dvec3{eye} + glm::dvec3{delta}, glm::dvec3{target} + glm::dvec3{delta}, kNearD,
			kFarD, kFovYRad, kAspect);
		for (size_t i = 0; i < batch.centers.size(); ++i) {
			INFO("delta = " << delta.x << "," << delta.y << "," << delta.z << " i = " << i);
			CHECK(movedVis[i] == baseVis[i]);  // 🔴 逐球，不是总数
			// 闭式第二实现同样不许漂（它不吃 float 矩阵，是这条判据的独立对照）
			CHECK(closedVisible(closedMoved, glm::dvec3{batch.centers[i]} + glm::dvec3{delta},
				batch.radii[i]) == baseVis[i]);
		}
	}
}

TEST_CASE("判据 2 的区分力守卫：只平移场景、相机不动 ⟹ 判定**必须**变（否则上一条是空判据）") {
	// 「相同」这件事需要一个对照物：如果无论怎么摆判定都不变，那 ③ 的绿毫无内容。
	// 这条把「场景动了相机没动」这个明显的错法摆上去，要求它**确实**分家。
	const glm::dvec3 eye{2.0, 6.0, 14.0};
	const glm::dvec3 target{0.0, 0.0, -2.0};
	int rejected = 0;
	const SphereBatch batch = makeSpheres(eye, target, kSeed + 7u, 240, rejected);
	REQUIRE(batch.centers.size() == 240);

	const glm::mat4 proj = projOf(static_cast<float>(kNearD), static_cast<float>(kFarD));
	const glm::mat4 view = glm::lookAt(glm::vec3{eye}, glm::vec3{target}, glm::vec3{0.0f, 1.0f, 0.0f});
	const std::vector<bool> baseVis = runCull(extractFrustum(proj * view), batch, glm::vec3{0.0f});

	const glm::vec3 delta{1.0e5f, -3.0e4f, 7.0e3f};
	const std::vector<bool> sceneOnly = runCull(extractFrustum(proj * view), batch, delta);
	int differ = 0;
	for (size_t i = 0; i < batch.centers.size(); ++i)
		if (sceneOnly[i] != baseVis[i])
			++differ;
	CHECK(differ > 0);  // 一个都没变 = 剔除器根本没在吃视锥位置
}

// ═══════════════════════════════════════════════════════════════════════════
// 判据 3：meshlet 簇剔除（CPU 参考实现）的可见簇集合逐元素平移不变
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/**
 * 合成簇集夹具：只填 `clusters` 视图（剔除器只读它）。
 *
 * 禁拷贝禁移动：`asset.clusters` 指向 `storage`，拷贝会留悬空视图（同 test_meshlet_cull 的理由）。
 */
struct ClusterSet {
	std::vector<MeshletDiskCluster> storage;
	MeshletAsset asset;
	explicit ClusterSet(const std::vector<std::pair<glm::vec3, float>>& specs) {
		storage.reserve(specs.size());
		for (const auto& s : specs) {
			MeshletDiskCluster c{};
			c.group = 0;
			c.refined = -1;
			c.triCount = 3;
			c.vertCount = 3;
			c.center[0] = s.first.x;
			c.center[1] = s.first.y;
			c.center[2] = s.first.z;
			c.radius = s.second;
			storage.push_back(c);
		}
		asset.header.clusterCount = static_cast<uint32_t>(storage.size());
		asset.clusters = std::span<const MeshletDiskCluster>(storage);
	}
	ClusterSet(const ClusterSet&) = delete;
	ClusterSet& operator=(const ClusterSet&) = delete;
};

/**
 * 「场景 + 相机一起平移 Δ、渲染原点也跟着走 Δ」这一姿态下的**生产同形**输入对。
 *
 * 生产链：`state.matrices[slot] = toRelativeModel(spin.matrix, tf.absolute, origin.position)`
 * → scene data 对象矩阵表 → shader `readObjectMatrix`；视锥 = `extractFrustum(proj·view)`，
 * view = `cameraView(camera, origin.position)`。CPU 参考实现将来要与 GPU 逐元素对账 ⟹
 * 它必须吃**同一对**（相对矩阵 + 相对视锥）。本函数就是那对输入的离线版。
 */
struct CullInputPair {
	glm::mat4 objectMatrix{1.0f};
	Frustum frustum;
};

CullInputPair makeCullInput(const glm::dmat4& absolute, const glm::dvec3& eyeAbs,
	const glm::dvec3& targetAbs, const glm::dvec3& origin) {
	CullInputPair p;
	p.objectMatrix = toRelativeModel(glm::mat4{1.0f}, absolute, origin);
	const glm::mat4 view = glm::lookAt(toRelative(eyeAbs, origin), toRelative(targetAbs, origin),
	                                   glm::vec3{0.0f, 1.0f, 0.0f});
	p.frustum = extractFrustum(projOf(static_cast<float>(kNearD), static_cast<float>(kFarD)) * view);
	return p;
}

}  // namespace

TEST_CASE("🔴 meshlet 簇剔除可见集逐元素平移不变：场景+相机+原点一起平移 Δ，可见簇序列完全相同") {
	// 「meshlet 剔除路径自动跟」此前是任务书里标了「未确认」的那一行。GPU 侧
	// `meshlet_cull.comp.slang` 读 `readObjectMatrix`（scene data 里那份**相对**矩阵）+
	// `cullPush.planes`（= 与 CPU 剔除同源的**相对**视锥）⟹ 两侧同空间，自动跟成立（审计读数
	// → 交付报告）。CPU 参考实现 `cullClustersLinear` 目前**零生产调用点**，本条把它将来要吃的
	// 那对输入的空间口径钉成判据：喂错空间（绝对矩阵 + 相对视锥）它必须立刻分家。
	// 🔴 断言的是**逐元素相同的簇下标序列**，不是数量相同（skill：对账伙伴必须钉序/映射）。
	const glm::dvec3 eye{2.0, 6.0, 14.0};
	const glm::dvec3 target{0.0, 0.0, -2.0};

	// 源 glb 单位下的簇球（±1 量级），实例矩阵把它们摆到世界并放大 6 倍
	std::vector<std::pair<glm::vec3, float>> specs;
	Lcg rng{kSeed + 11u};
	for (int i = 0; i < 160; ++i)
		specs.push_back({glm::vec3{static_cast<float>(rng.uniform(-1.0, 1.0)),
			                      static_cast<float>(rng.uniform(-1.0, 1.0)),
			                      static_cast<float>(rng.uniform(-1.0, 1.0))},
			static_cast<float>(rng.uniform(0.02, 0.12))});
	const ClusterSet clusters{specs};
	REQUIRE(clusters.asset.clusters.size() == 160);

	// 实例的绝对矩阵：平移到世界 (5, -3, 8)、均匀缩放 6
	glm::dmat4 absolute{1.0};
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			absolute[col][row] = static_cast<double>(col == row ? 1.0 : 0.0);
	absolute[0] = glm::dvec4{6.0, 0.0, 0.0, 0.0};
	absolute[1] = glm::dvec4{0.0, 6.0, 0.0, 0.0};
	absolute[2] = glm::dvec4{0.0, 0.0, 6.0, 0.0};
	absolute[3] = glm::dvec4{5.0, -3.0, 8.0, 1.0};

	const CullInputPair base = makeCullInput(absolute, eye, target, glm::dvec3{0.0});
	const ClusterCullResult baseR = cullClustersLinear(clusters.asset, base.objectMatrix, base.frustum);
	// 阳性锚点：可见与不可见两类都必须有（否则「序列相同」可能只是因为全可见/全不可见）
	CHECK(baseR.visibleClusterCount > 0);
	CHECK(baseR.visibleClusterCount < 160u);

	for (const glm::dvec3& d : {glm::dvec3{137.0, -91.5, 42.25}, glm::dvec3{1.0e6, -3.0e5, 7.0e4},
			 glm::dvec3{-0.5, 0.25, -0.125}}) {
		// 场景（实例绝对矩阵）+ 相机 + 渲染原点**一起**平移 d
		glm::dmat4 moved = absolute;
		moved[3] = glm::dvec4{absolute[3].x + d.x, absolute[3].y + d.y, absolute[3].z + d.z, 1.0};
		const CullInputPair mv = makeCullInput(moved, eye + d, target + d, d);
		const ClusterCullResult got = cullClustersLinear(clusters.asset, mv.objectMatrix, mv.frustum);
		INFO("delta = " << d.x << "," << d.y << "," << d.z << " 可见簇数 base="
						<< baseR.visibleClusterCount << " moved=" << got.visibleClusterCount);
		// 🔴 逐元素（vector<uint32_t> 的 == 就是逐元素同序比）
		CHECK(got.visibleClusters == baseR.visibleClusters);
		CHECK(got.visibleTriangles == baseR.visibleTriangles);
	}
}

TEST_CASE("判据 3 的错位锚点：CPU 参考实现喂**绝对**矩阵 + 相对视锥 ⟹ 可见簇集合必须分家") {
	// 这条是判据 3 的反向半边，也是「两侧不同空间 = 静默错位」这个失效形态的**唯一**机器判据
	// （先例 → skill rgl-test-discipline「对账伙伴模块」：两边都错但互相印证是最贵的一种）。
	// 门关态原点恒 0 时绝对 == 相对，本条**不红**；它守的是门开那天。
	const glm::dvec3 eye{2.0, 6.0, 14.0};
	const glm::dvec3 target{0.0, 0.0, -2.0};
	std::vector<std::pair<glm::vec3, float>> specs;
	Lcg rng{kSeed + 11u};
	for (int i = 0; i < 160; ++i)
		specs.push_back({glm::vec3{static_cast<float>(rng.uniform(-1.0, 1.0)),
			                      static_cast<float>(rng.uniform(-1.0, 1.0)),
			                      static_cast<float>(rng.uniform(-1.0, 1.0))},
			static_cast<float>(rng.uniform(0.02, 0.12))});
	const ClusterSet clusters{specs};

	glm::dmat4 absolute{1.0};
	absolute[0] = glm::dvec4{6.0, 0.0, 0.0, 0.0};
	absolute[1] = glm::dvec4{0.0, 6.0, 0.0, 0.0};
	absolute[2] = glm::dvec4{0.0, 0.0, 6.0, 0.0};
	absolute[3] = glm::dvec4{5.0, -3.0, 8.0, 1.0};

	const glm::dvec3 origin{1.0e5, -3.0e4, 7.0e4};
	const CullInputPair rel = makeCullInput(absolute, eye, target, origin);
	const ClusterCullResult relR = cullClustersLinear(clusters.asset, rel.objectMatrix, rel.frustum);

	// 错法：视锥跟着原点走了，对象矩阵却还是绝对的（= 漏改形态）
	glm::mat4 absAsFloat{1.0f};
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			absAsFloat[col][row] = static_cast<float>(absolute[col][row]);
	const ClusterCullResult wrong = cullClustersLinear(clusters.asset, absAsFloat, rel.frustum);
	CHECK_FALSE(wrong.visibleClusters == relR.visibleClusters);
	// 且错法确实把整个实例推出了视锥（不是「随机换个集合」，而是有方向地全丢）
	CHECK(wrong.visibleClusterCount == 0u);
	CHECK(relR.visibleClusterCount > 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// 判据 4：透明排序键的**顺序**平移不变（实测：只在 float32 精度余量内成立）
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/**
 * 透明段排序键（生产式的**逐行转录**，出处 = `render_list_impl.cppm` 的
 * `glm::vec3 d = glm::vec3(tf.world[3]) - glm::vec3(camera.position); … glm::dot(d, d)`）。
 *
 * ⚠ **覆盖边界如实报**：本文件没有 `renderListSystem` 的调用点判据（它要 entt registry +
 * 资产表 + Resources，不是无设备单测能起的）。这条转录钉的是**这个键的数学性质**，
 * 「生产点真用了它」仍只有出厂截图锚在守（与批 3 登记的「生产点变异单测不红」同一形态）。
 * 🔴 改生产式必须同步改这里（test_shadow_geometry 的 selectCascade 同款契约）。
 */
float transparentSortKeySq(const glm::mat4& world, const glm::dvec3& cameraPosition) {
	const glm::vec3 d = glm::vec3(world[3]) - glm::vec3(cameraPosition);
	return glm::dot(d, d);
}

/** 透明段的 back-to-front 装配：按 key 降序**稳定**排序，返回 entity 下标序列。 */
std::vector<int> sortTransparent(const std::vector<glm::mat4>& worlds, const glm::dvec3& camera) {
	std::vector<int> idx(worlds.size());
	for (size_t i = 0; i < idx.size(); ++i)
		idx[i] = static_cast<int>(i);
	std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
		return transparentSortKeySq(worlds[a], camera) > transparentSortKeySq(worlds[b], camera);
	});
	return idx;
}

/**
 * 键的 float32 舍入误差上界（闭式推导，出处 = IEEE 的 ULP 定义，不抄被测码）。
 *
 * `d = float(w) − float(c)`：两个入参各被舍过一道，量级 M 处相邻 float 间距 = 2^(e−23)
 * （M ∈ [2^(e−1), 2^e)）⟹ |d 的误差| ≤ 2^(e−23)。`key = dot(d,d)` 对 |d| 的导数是 2|d|
 * ⟹ 绝对误差上界 = 2·max|d|·ULP(M)。**减法本身不额外放大**（误差全来自两个入参的预舍）。
 * ⟹ 本判据的射程由这个界决定：键差 ≫ 界的对顺序必不变；键差 ≪ 界的对顺序可翻。
 */
double keyErrorBound(const std::vector<glm::mat4>& worlds, const glm::dvec3& camera) {
	double magnitude = glm::length(camera);
	double maxDist = 0.0;
	for (const glm::mat4& w : worlds) {
		const glm::dvec3 t{glm::dvec3{w[3]}};
		magnitude = std::max(magnitude, glm::length(t));
		maxDist = std::max(maxDist, glm::length(t - camera));
	}
	int exponent = 0;
	std::frexp(static_cast<float>(magnitude), &exponent);
	const double ulp = std::ldexp(1.0, exponent - 24);
	return 2.0 * maxDist * ulp;
}

/** 样本集里**最小非零**键差（闭式枚举全部对：200 个 = 19900 对，毫秒级）。 */
double minKeyGap(const std::vector<glm::mat4>& worlds, const glm::dvec3& camera) {
	double gap = 1e300;
	for (size_t a = 0; a < worlds.size(); ++a)
		for (size_t b = a + 1; b < worlds.size(); ++b) {
			const double g = std::abs(static_cast<double>(transparentSortKeySq(worlds[a], camera)) -
				static_cast<double>(transparentSortKeySq(worlds[b], camera)));
			if (g > 0.0 && g < gap)
				gap = g;
		}
	return gap;
}

/** 顺序向量 → 「entity → 名次」。 */
std::vector<int> rankOf(const std::vector<int>& order) {
	std::vector<int> r(order.size());
	for (size_t i = 0; i < order.size(); ++i)
		r[static_cast<size_t>(order[i])] = static_cast<int>(i);
	return r;
}

/** 造判据 4 的世界矩阵集（写死种子；平移列量级 ±60，与交付场景同量级）。 */
std::vector<glm::mat4> makeTransparentWorlds() {
	std::vector<glm::mat4> worlds;
	Lcg rng{kSeed + 23u};
	for (int i = 0; i < 200; ++i) {
		glm::mat4 w{1.0f};
		w[3] = glm::vec4{static_cast<float>(rng.uniform(-60.0, 60.0)),
			static_cast<float>(rng.uniform(-40.0, 40.0)), static_cast<float>(rng.uniform(-60.0, 60.0)),
			1.0f};
		worlds.push_back(w);
	}
	return worlds;
}

/** 场景跟着平移 Δ（float 落库形态，与生产 `tf.world` 一致）。 */
std::vector<glm::mat4> translateWorlds(const std::vector<glm::mat4>& worlds, const glm::dvec3& d) {
	std::vector<glm::mat4> out;
	out.reserve(worlds.size());
	for (const glm::mat4& w : worlds) {
		glm::mat4 m = w;
		m[3] = w[3] + glm::vec4{glm::vec3{d}, 0.0f};
		out.push_back(m);
	}
	return out;
}

}  // namespace

TEST_CASE("🔴 透明排序键顺序平移不变（射程内）：键差 ≫ float32 舍入界时，一起平移 Δ 顺序逐位相同") {
	// 「透明排序键不用改」的依据是它吃**两个绝对量**（`tf.world[3]` 与 `camera.position`），
	// 差在实数域平移不变。断言取**顺序**而不是键值：键值会因 float 舍入微变，顺序才是契约。
	// 🔴 **实测修正（本批对任务书那张表的一条）**：这条**不是无条件成立**的。键在 float 域算，
	// 两个绝对量都变大时 `float(w) − float(c)` 的舍入误差按 ULP(|绝对量|) 一起长大，一旦超过
	// 「最近一对透明体的键差」，顺序就会翻。射程由 `keyErrorBound` 给出，见下一条。
	const std::vector<glm::mat4> worlds = makeTransparentWorlds();
	const glm::dvec3 cam{3.0, 5.0, 12.0};
	const std::vector<int> baseOrder = sortTransparent(worlds, cam);

	// 前提守卫 ①：排序**确实在重排**（恒等序 = 输入构造退化，本用例失去判据力）
	bool reordered = false;
	for (size_t i = 0; i < baseOrder.size(); ++i)
		if (baseOrder[i] != static_cast<int>(i))
			reordered = true;
	CHECK(reordered);
	// 前提守卫 ②（区分力守卫，出处 = 上一批「注入量必须有区分力」那条纪律）：本样本集的最小
	// 键差必须远大于误差界，否则下面的「顺序相同」不是判据而是巧合。
	const double gap = minKeyGap(worlds, cam);
	REQUIRE(gap > 0.0);
	CHECK(keyErrorBound(worlds, cam) * 2.0 < gap);

	int inRange = 0;
	for (const glm::dvec3& d : {glm::dvec3{137.0, -91.5, 42.25}, glm::dvec3{-0.5, 0.25, -0.125},
			 glm::dvec3{1000.0, -750.0, 500.0}}) {
		const std::vector<glm::mat4> moved = translateWorlds(worlds, d);
		const double bound = keyErrorBound(moved, cam + d);
		INFO("delta = " << d.x << "," << d.y << "," << d.z << " minKeyGap = " << gap
						<< " keyErrorBound = " << bound);
		// 射程条件 = 闭式的**数学正确条件**：误差界 < 最小键差 ⟹ 任何一对的键序都不可能被舍入
		// 翻过来。界 ≫ 键差的平移量（本批实测 |Δ| ≈ 1e3 起）不进本条，由下一条专门验它的形态。
		if (!(bound < gap))
			continue;
		CHECK(sortTransparent(moved, cam + d) == baseOrder);
		++inRange;
	}
	CHECK(inRange == 2);  // 空转守卫：三个平移量里恰有两个落在射程内（第三个是射程外的对照）
}

TEST_CASE("🔴 透明排序键的射程边界（实测发现）：|绝对量| 大到 ULP 吃掉键差时顺序确实会翻，且翻的全是键差小于误差界的近邻对") {
	// 这条把上一条的「例外」变成判据本身。实测读数（写死种子 kSeed+23、200 个透明体）：
	//   最小键差 = 7.568e-3
	//   |Δ| ≈ 1.4e2 ⟹ 误差界 2.93e-3 < 键差 ⟹ 翻转 0 对
	//   |Δ| ≈ 1.1e5 ⟹ 误差界 1.50   ≫ 键差 ⟹ 翻转 1 对（被翻那对的键差恰 = 7.568e-3）
	//   |Δ| ≈ 3.7e6 ⟹ 误差界 48.0        ⟹ 翻转 25 对（最宽的翻转对键差 12.9，仍 < 48.0）
	// ⟹ 断言两半边：① 大平移下顺序**确实**变了（否则上一条的「相同」毫无内容）；
	//    ② 每个被翻的对的键差都 ≤ 误差界 —— 即失效**只**发生在 float 舍入能解释的地方，
	//    不是排序逻辑本身随位置漂。② 才是这条的真判据：它把「顺序漂了」这件事的归因钉死。
	const std::vector<glm::mat4> worlds = makeTransparentWorlds();
	const glm::dvec3 cam{3.0, 5.0, 12.0};
	const std::vector<int> baseOrder = sortTransparent(worlds, cam);
	const std::vector<int> baseRank = rankOf(baseOrder);
	const double gap = minKeyGap(worlds, cam);
	REQUIRE(gap > 0.0);

	int casesWithInversion = 0;
	for (const glm::dvec3& d : {glm::dvec3{1.0e5, -3.0e4, 7.0e4}, glm::dvec3{1.0e6, 2.0e6, -3.0e6}}) {
		const std::vector<glm::mat4> moved = translateWorlds(worlds, d);
		const std::vector<int> gotOrder = sortTransparent(moved, cam + d);
		const std::vector<int> gotRank = rankOf(gotOrder);
		const double bound = keyErrorBound(moved, cam + d);
		int inversions = 0;
		double widestFlippedGap = 0.0;
		for (size_t a = 0; a < worlds.size(); ++a)
			for (size_t b = a + 1; b < worlds.size(); ++b) {
				if ((baseRank[a] < baseRank[b]) == (gotRank[a] < gotRank[b]))
					continue;
				++inversions;
				widestFlippedGap = std::max(widestFlippedGap,
					std::abs(static_cast<double>(transparentSortKeySq(worlds[a], cam)) -
						static_cast<double>(transparentSortKeySq(worlds[b], cam))));
			}
		INFO("delta = " << d.x << "," << d.y << "," << d.z << " 翻转对数 = " << inversions
						<< " 最宽翻转对键差 = " << widestFlippedGap << " 误差界 = " << bound);
		CHECK(inversions > 0);                        // 半边①：确实变了
		CHECK(widestFlippedGap <= bound * 2.0);       // 半边②：变的都是近邻对（界留 2 倍余量）
		++casesWithInversion;
	}
	CHECK(casesWithInversion == 2);
	// 🔴 结论如实说：本键**不吃渲染原点**（批 4 判它「不用改」是对的，rebase 不会让它更坏），
	// 但它因此也**拿不到 rebase 的精度收益** —— 债-85 触发点（|camera| ≈ 7.6e5）之上，
	// 近平局的透明体会随相机位置重排。要不要改成 double 域算差再比是独立一笔，不在本批。
}

TEST_CASE("判据 4 的两条锚点：只移场景不移相机、以及混用相对/绝对空间，顺序都必须分家") {
	// 「顺序相同」需要对照物才不是空判据。两条错法：
	//  ① 场景动了相机没动（明显的漏改形态）；
	//  ② 键的一侧换成**相对**矩阵、另一侧仍是绝对相机位（= 批 4 之后最容易顺手写出的混空间）。
	//    生产用 `tf.world`（绝对派生）而不是 `modelMatrix`（相对），这条就是那个选择的机器理由。
	const std::vector<glm::mat4> worlds = makeTransparentWorlds();
	const glm::dvec3 cam{3.0, 5.0, 12.0};
	const std::vector<int> baseOrder = sortTransparent(worlds, cam);
	const glm::dvec3 d{137.0, -91.5, 42.25};  // 射程内的平移量（否则「分家」可能来自 float 噪声）

	// ① 只移场景（相机不动）⟹ 相对位置变了 ⟹ 顺序必须变
	CHECK(sortTransparent(translateWorlds(worlds, d), cam) != baseOrder);

	// ② 混空间：键吃**相对**平移列 + **绝对**相机位（原点 = d）
	std::vector<glm::mat4> mixed;
	for (const glm::mat4& w : worlds) {
		const glm::mat4 rel = toRelative(glm::dmat4{w}, d);
		glm::mat4 m{1.0f};
		m[3] = rel[3];
		mixed.push_back(m);
	}
	CHECK(sortTransparent(mixed, cam) != baseOrder);
	// 而混空间的**正确配对**（相对 + 相对相机位）顺序与基准相同 —— 键本身没错，
	// 错的是两侧不同空间。两半边各钉一条，防「只验一半」。
	CHECK(sortTransparent(mixed, cam - d) == baseOrder);
}

}  // namespace RGL
