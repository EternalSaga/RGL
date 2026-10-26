module;
/**
 * test_shadow_caster_aabb —— 债-149 的守卫：**投影体 AABB 不许把「本帧坐标系的零点」算进去**
 *
 * ## 被测对象，以及为什么必须有这条文件
 * `rgl/render_list_impl.cppm::buildShadowVolumes` 把投影体 AABB 的累加初值写成 `{0.0f}` 时，
 * 本帧坐标系的零点恒被算进包围盒 ⟹ 场景整个偏到零点一侧时 AABB 多出一条从 0 拉到场景的长
 * 尾巴 ⟹ 尾巴按 `|d·轴|` 折进光空间正交盒的深度跨度 `tMax − tMin` ⟹ 采样端那个 **NDC 域**
 * 常数深度偏置（`kShadowDepthBias`）的世界当量 = `bias × 跨度` 随之线性膨胀 ⟹ 超过场景能给出
 * 的最大遮挡深度差 ⟹ **阴影整块消失**（实机：合成场景偏 300 m 处阴影贡献恰为 0 像素）。
 * 修复（初值取第一个候选）2026-10-26 入账，但台账行自己写着：「**还没有一条会红的单测**」。
 * 本文件补那条。没有它，这次修复就只是「闭式预测 + 一次实机差分」——本项目反复实测的对手正是
 * 这种看起来已完工的形态。
 *
 * ## 🔴 变异红线声明（哪个变异让我红；本批**全部真跑**，读数见交付报告）
 *  - **M1（任务书点名的那条）** 把累加初值改回改动前那一份：`casterMin/Max` 保持 `{0.0f}`
 *    且循环吃 `cands` 而不是 `cands.subspan(1)`（= 让零点参与 min/max）
 *    → 红：①-A 闭式对账（每个 Δ × 每一级都超上界）、🔴 ①-B 整组候选平移不变、
 *      🔴 ①-C 换坐标系零点不变、①-D 属性测试、🔴 ②-B 偏置世界当量不变。
 *  - **M2（弱化：只吃第一个候选，忘累加其余）** 删掉 `for (const auto& cd : cands.subspan(1))`
 *    的循环体 → 红：①-A 的**下界**半边（AABB 变小 ⟹ 跨度掉到闭式凸包之下）+ ①-D 的下界。
 *    ⚠ 上界半边**不红**（AABB 只会更小）——这就是「对称判据的每一半边各要一条用例」的形态，
 *    只测上界会放行 M2。
 *  - **M3（上界半边专用）** `casterMax = cands[0].center + cands[0].radius` 改成 `… - radius`
 *    → 🔴 **实测只红 ①-D**（不是预期的 ①-A/①-B/①-C）：8 球壳里第一个候选的角被其余 7 个角
 *      盖住 ⟹ 该变异在主配置上是**空操作**，只有随机集里 `n = 1` 的样本抓得到。原样记在这。
 *  - **M4（激励有区分力的自证，反向半边）** 把判据候选集整体搬回**跨过零点**（= 出厂形态）
 *    → 「守卫的自证」那条红（`L_old − L` 掉到 0），而其余用例**全绿**
 *      ⟹ 证明 M1 的红来自判据本身，不是「输入恰好让旧代码也对」。
 *
 * ## 变异实跑记录（2026-10-26，harness = 显式建 `rgl_tests` 目标 + 取构建自身返回码 +
 * `shutil.copyfile` 还原不带 mtime + 每条还原后重跑基线断言全绿；四条变异 RC 全 0）
 *  - **M1 → 红 6 条 / 216 断言**：①-A、①-B、①-C、①-D、②-A、②-B。逐级跨度实测
 *    Δ=0 处 67.92 m（闭式上界 15.89 m）、Δ=30 处 97.92 m ⟹ **漂移恰为 Δ 本身**
 *    （= 债-149 的「随偏移线性增长」在毫秒级单测里复现，量级 = 界的 6 万倍）。
 *  - **M2 → 红 6 条**：①-A（**下界**半边：实测 3.62 m < 界 15.87 m）、①-B、①-D、①-E、②-A、②-C。
 *    ⚠ ①-C **不红**——「只吃第一个候选」对平移仍是不变量，这条只能由下界半边抓。
 *  - **M3 → 只红 ①-D 一条**。⚠ 如实报：在 8 球壳配置下 `casterMax` 少一个候选角会被其余 7 个
 *    角**盖住**（glm::max 取大 ⟹ 变异是空操作），只有随机集里 `n = 1` 的样本抓得到。
 *    ⟹ 这条是「手挑输入恰好让变异无效」的现场证据，也是 ①-D 存在的理由。
 *  - **M4 → 红「守卫的自证」+ ①-D 的前提计数**，其余 8 条**全绿** ⟹ 判据的红确实来自
 *    「候选集偏在零点一侧」这个前提，而不是别处。
 *
 * ## 期望值出处（🔴 不抄被测码的算式）
 *  - **闭式（盒的 support function 定义）**：轴对齐盒 `B = ∏[lo_k, hi_k]` 在单位向量 d 上的
 *    投影长度 `L = Σ_k |d_k|·(hi_k − lo_k)`。两行推出：`max_{p∈B} dot(p,d) = Σ_k max(d_k·lo_k,
 *    d_k·hi_k)`、`min` 同理取 min，相减即得。这条式子里**没有「累加初值」这个概念**。
 *  - **闭式（盒的 Z 界 = 两段的凸包）**：出处 = `ShadowVolume::tMin/tMax` 的字段注释 +
 *    `buildShadowVolume` 函数头「沿光轴取并：受光面占 [−r, r]，投影体占其世界 AABB 在光轴上的
 *    投影，取两者之并再各加余量」那句。记候选区间相对**未吸附**盒心的中点偏移 `m0`、凸包长度
 *    `H(m) = max(r, m + L/2) − min(−r, m − L/2)`，则
 *    ```
 *    D = H(吸附后) + 2·pad,   |H(吸附后) − H(m0)| ≤ g/2   （H 对 m 是 1-Lipschitz，
 *                                                            吸附位移 ≤ 半纹素）
 *    ⟹  H(m0) − g/2 + 2·pad ≤ D ≤ H(m0) + g/2 + 2·pad
 *    ```
 *    🔴 **两端都不含「候选集到零点多远」这个量** —— 这就是「跨度不许知道零点在哪」的闭式表达。
 *    特殊情形「候选区间含住切片区间」时 `H = L` 且与吸附相位无关 ⟹ `D = L + 2·pad` **取等号**
 *    （主用例刻意做成这一形态，判据因此收得进一枚纹素，而不是「小于某个绝对数」）。
 *  - **实机**：②-A 的毫米数与 `docs/还债总账.md` 债-150 行的出厂读数（跨度 16.2 m ⟹ 当量
 *    24 mm）交叉核对 —— 本文件的候选集刻意做成同量级跨度（闭式 L = 15.88 m）。
 *
 * ## 与相邻测试文件的分工（别读成重复覆盖）
 *  - `tests/test_shadow_texel_absolute.cppm`（批 5 第 0.5/1 刀）测的是**盒心吸附**（XY 落位与
 *    跨格跳变），它喂给 `buildShadowVolume` 的 `casterMin/Max` 是**测试自己给的**，压根不经过
 *    本文件测的那段累加 ⟹ 那段初值它一条都抓不到。
 *  - `tests/test_shadow_geometry.cppm` 测 `buildShadowVolume` 本体（给定 AABB 时盒怎么建）。
 *  - 本文件测的是**AABB 从哪来**。债-149 的病灶只在 `buildShadowVolumes` 这一层，所以接缝
 *    必须是它（本批抬成 render_list 导出接缝，同款先例 = `computeCascadeSpheres` /
 *    `emitShadowCasters`；生产调用点仍只有一个，抬接缝本身零行为变化）。
 *
 * ## 覆盖边界（如实报）
 *  - 只测 `tMax − tMin` 与它的物理当量，不测盒心落位（那条在 texel_absolute 的「落位硬界」）。
 *  - 平移量 / 原点量级上限取 **300 m**：再大就撞上**另一条**已知缺陷（门关态把大坐标存进
 *    float 的 `Bounds::center`，ULP(1e6) = 0.0625 m = 123 个最细级纹素），那条的判据是图像级
 *    （`just imgqa shadow-edge-rebase`），混进来只会让本文件的容差失去意义。
 *  - 空候选集那一格**保留**旧行为（AABB 退化成零点那个点）：定义处注释写着「没有任何 draw
 *    要画，等价于无阴影」。本文件只钉住这个现状（反向用例），不判它对错。
 *  - `pad = r·0.02` 是 `buildShadowVolume` 的实现细节而不是外部契约，本文件把它当**已知常数**
 *    吃进闭式（`kPadScale`）。哪天改 pad，①-A 的紧式那半边会红并要求同步改这里——这条耦合是
 *    有意的，写在注释里而不是藏起来。
 */
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // glm::lookAt / perspective / radians
#include <algorithm>                     // std::max / std::min / std::swap
#include <cmath>                         // std::abs / std::round
#include <initializer_list>              // modules 坑：花括号列表不从 import 带进来
#include <limits>                        // std::numeric_limits（空集 AABB 的初值）
#include <string>                        // doctest 在本构建把 const char* 变量打成指针
#include <vector>

export module test_shadow_caster_aabb;

import rlstd;
import test_support;        // testing::Lcg / expectNear（写死种子，禁真随机）
import scene_context;       // Bounds / Light / ShadowVolume / ViewProjection / quantizeShadowDistance
import scene_data;          // kActiveShadowCascades / kShadowDepthBias
import render_graph;        // shadowAtlasColumns / shadowTileSize
import render_list;         // 🔴 buildShadowVolumes（本批抬出的接缝）/ computeCascadeSpheres / ShadowCandidate
import frame_draw_list;     // DrawCall / ShadowDrawArgs / ShadowMeshletDrawArgs
import coordinate_rebase;   // toRelative（全仓唯一「绝对 → 相对」换算入口）

namespace RGL {
namespace {

using testing::Lcg;

/** 属性测试种子（写死；换种子 = 换一组输入，不是「重试」）。 */
constexpr uint32_t kSeedAabb = 20261126u;

// ─── 出厂配置读数（与 tests/test_shadow_texel_absolute.cppm 同一组；改配置 = 本文件读数
//     跟着重立，这是有意的）─────────────────────────────────────────────────────

/** `configs/camera.lua` 的 `fovDegrees = 45.0`。 */
constexpr float kFovYDeg = 45.0f;
/** `configs/runtime.lua` 的 `headless_size = { 1280, 720 }` ⟹ 渲染网格宽高比 16:9。 */
constexpr float kAspect = 16.0f / 9.0f;
/** 级联深度界的出厂读数（出处 = `docs/还债总账.md` 债-139 行的实机记录）。 */
constexpr float kNearRaw = 0.05f;
constexpr float kFarRaw = 1.713496f;
/** `configs/scene.lua` 的 `light.direction`（消费时归一化）。 */
const glm::vec3 kSunDir{0.5f, -0.75f, -0.45f};

/** 出厂主光（只 direction 参与光空间基）。 */
const Light kSun{.index = 0u, .direction = kSunDir, .color = glm::vec3{1.0f, 0.98f, 0.95f},
                 .intensity = 12.0f, .ambient = glm::vec3{0.25f}, .position = glm::dvec3{0.0},
                 .range = 0.0f, .type = 0u};

/** 图集单块边长（= `buildShadowVolumes` 里那个 snap 格）。 */
const uint32_t kTile = shadowTileSize(shadowAtlasColumns(kActiveShadowCascades));

/**
 * `buildShadowVolume` 给 Z 界加的余量比例（`pad = r · kPadScale`）。
 * ⚠ 它是实现细节不是契约，出处见文件头「覆盖边界」倒数第二条。
 */
constexpr double kPadScale = 0.02;

/**
 * 闭式判据容差里除「吸附相位 ≤ 半纹素」之外的**浮点残量**（米）。
 *
 * 出处 = 实测：Δ = 300 m 时生产在 float32 里做 `dot(casterFar − snappedCenter, d)`，量级
 * ~300 m 的数相减 ⟹ 每步 ULP = 3.05e-5 m，闭式对账的实测残量 ~1e-4 m。取 5e-4 m（5 倍余量，
 * 且**小于**最细级的一枚纹素 5.1e-4 m）。而 M1 的信号是 50…300 m，比这条容差大五个数量级。
 */
constexpr double kFloatSlackM = 5.0e-4;

/**
 * 判据候选集离坐标系统零点的**基准距离**（米，沿光轴）。
 * 取 60 m 的理由：① 保证「整个偏到零点一侧」（实测 AABB 三根轴离零点最小 24 m）；
 * ② M1 在 Δ = 0 处就已经把跨度撑到闭式值的四倍（68 m vs 15.9 m），不依赖 Δ 阶梯也能红。
 */
constexpr double kOneSidedBase = 60.0;

/** ①-B / ①-C / ②-B 的自变量阶梯（米）。上限 300 的理由见文件头「覆盖边界」。 */
constexpr double kDeltaLadder[4] = {0.0, 30.0, 150.0, 300.0};

/** 出厂机位（绝对层，与 test_shadow_texel_absolute 同一组）。 */
const glm::dvec3 kEyeAbs{1.1, 0.75, 1.05};
const glm::dvec3 kTargetAbs{0.14, 0.12, 0.02};

/** 归一化光向（闭式里的那个 d）。 */
glm::dvec3 lightDir() { return glm::dvec3{glm::normalize(kSun.direction)}; }

// ─── 闭式算子（期望值出处，全部只吃**测试自己的输入**，不调被测码）───────────────

/** 轴对齐盒（double，判据侧专用；生产的 AABB 是 float）。 */
struct ClosedBox {
	glm::dvec3 lo;
	glm::dvec3 hi;
};

/**
 * 候选集的轴对齐包围盒。出处 = **定义**：球 = 中心 ± 半径 的轴对齐盒，候选集的盒 = 它们的并
 * （轴对齐 ⟹ 逐分量 min/max）。空集返回一个反向哨兵盒（调用方守 candCount）。
 */
ClosedBox casterBox(std::span<const ShadowCandidate> cands) {
	ClosedBox b{glm::dvec3{std::numeric_limits<double>::max()},
	            glm::dvec3{std::numeric_limits<double>::lowest()}};
	for (const ShadowCandidate& c : cands) {
		const glm::dvec3 ctr{glm::dvec3{c.center}};
		const glm::dvec3 r{c.radius};
		b.lo = glm::min(b.lo, ctr - r);
		b.hi = glm::max(b.hi, ctr + r);
	}
	return b;
}

/** 「把零点也算进包围盒」= 债-149 的失效形态（本文件用它自证激励有区分力）。 */
ClosedBox unionWithZero(const ClosedBox& b) {
	return ClosedBox{glm::min(b.lo, glm::dvec3{0.0}), glm::max(b.hi, glm::dvec3{0.0})};
}

/**
 * 轴对齐盒在单位向量 d 上的**投影长度**（闭式，出处 = 盒的 support function 定义）：
 * `max_{p∈B} dot(p,d) = Σ_k max(d_k·lo_k, d_k·hi_k)`、`min` 取 min，相减 ⟹
 * `L = Σ_k |d_k|·(hi_k − lo_k)`。
 */
double extentAlong(const ClosedBox& b, const glm::dvec3& d) {
	const glm::dvec3 s = b.hi - b.lo;
	return std::abs(d.x) * s.x + std::abs(d.y) * s.y + std::abs(d.z) * s.z;
}

/**
 * 两个区间 `[−r, r]`（切片球在光轴上的投影）与 `[m − L/2, m + L/2]`（候选 AABB 的投影，
 * 中点偏移 m）的**凸包长度**。
 *
 * 为什么是凸包而不是「并的长度」：生产算的是 `max(r, 上端) − min(−r, 下端)`，两段不相交时
 * 中间的空档**也算进跨度**（并的长度不算）。把「并」写成判据会在候选集远离受光面时误报。
 */
double hullLength(double m, double L, double r) {
	return std::max(r, m + 0.5 * L) - std::min(-r, m - 0.5 * L);
}

/** 零点是否落在盒内（含边界）。 */
bool zeroInside(const ClosedBox& b) {
	return b.lo.x <= 0.0 && b.hi.x >= 0.0 && b.lo.y <= 0.0 && b.hi.y >= 0.0 && b.lo.z <= 0.0
	       && b.hi.z >= 0.0;
}

/** 盒到坐标系统零点的最小距离（三根轴上最小的那个「离零点还有多远」）。 */
double minAbsComponent(const ClosedBox& b) {
	return std::min({std::abs(b.lo.x), std::abs(b.hi.x), std::abs(b.lo.y), std::abs(b.hi.y),
	                 std::abs(b.lo.z), std::abs(b.hi.z)});
}

// ─── 一次生产调用 ───────────────────────────────────────────────────────────────

/** 一个场景（绝对层）：机位 + 投影体候选。整组平移 = 相机与候选**一起**搬（相对几何不变）。 */
struct Scene {
	glm::dvec3 eye;
	glm::dvec3 target;
	std::vector<ShadowCandidate> cands;
};

/** 一次 `buildShadowVolumes` 的产物 + 同一次调用里算好的闭式量（避免两处输入漂移）。 */
struct Reading {
	std::array<Bounds, kActiveShadowCascades> slices;
	std::array<ShadowVolume, kActiveShadowCascades> volumes;
	std::size_t candCount = 0;
	/** 闭式 L：候选 AABB 在光轴上的投影长度（吃**实际传给生产的那份 float 候选**）。 */
	std::array<double, kActiveShadowCascades> extentL{};
	/** 闭式 H(m0)：候选区间相对**未吸附**切片球心的凸包长度，逐级。 */
	std::array<double, kActiveShadowCascades> hull0{};
	/** 该级纹素（闭式 `2r / kTile`，与生产同定义）。 */
	std::array<double, kActiveShadowCascades> texel{};
	/** 候选区间是否**含住**切片区间（含住 ⟹ H = L 且与吸附相位无关 ⟹ 可用紧式）。 */
	std::array<bool, kActiveShadowCascades> contains{};
};

/** 造一个候选球（只填本文件用得上的五个字段，簇路径字段保持默认 = 不切）。 */
ShadowCandidate makeCand(const glm::dvec3& centerAbs, float radius, uint32_t slot) {
	ShadowCandidate c;
	c.center = glm::vec3{centerAbs};
	c.radius = radius;
	c.objectSlot = slot;
	c.pipeline = 7u + slot;  // 任意有效值：本文件不判 draw，只判跨度
	c.groups = 1u;
	return c;
}

/** 以 center 为中心的 8 球壳（半轴 h、球半径 ρ）⟹ 候选 AABB = center ± (h + ρ)。 */
std::vector<ShadowCandidate> shell8(const glm::dvec3& center, const glm::dvec3& h, float rho) {
	std::vector<ShadowCandidate> out;
	uint32_t slot = 0;
	for (const double sx : {-1.0, 1.0})
		for (const double sy : {-1.0, 1.0})
			for (const double sz : {-1.0, 1.0})
				out.push_back(makeCand(center + glm::dvec3{h.x * sx, h.y * sy, h.z * sz}, rho,
				                       slot++));
	return out;
}

/** 判据用候选集的盒半轴 + 球半径（闭式 L = 2·Σ|d_k|·(h_k+ρ) = 15.88 m）。 */
const glm::dvec3 kShellHalf{5.0, 3.5, 4.5};
constexpr float kShellRho = 0.5f;

/** 「跨过零点」的候选集（= 出厂场景的形态，债-149 的修复对它**是空操作**）。 */
std::vector<ShadowCandidate> straddlingCands() {
	return shell8(kTargetAbs, glm::dvec3{1.5, 0.75, 1.5}, 0.3f);
}

/**
 * 场景整体沿光轴搬 base 米（相机 + 候选一起搬 ⟹ 相对几何与出厂那一帧同构）。
 * 候选集仍以 target 为中心 ⟹ 它**含住**四级切片球的投影区间（①-A 的紧式因此可用）。
 */
Scene sceneAtBase(double base) {
	const glm::dvec3 shift = base * lightDir();
	return Scene{.eye = kEyeAbs + shift, .target = kTargetAbs + shift,
	             .cands = shell8(kTargetAbs + shift, kShellHalf, kShellRho)};
}

/**
 * 跑一帧：切片球来自生产（`computeCascadeSpheres`），候选按 `origin` 换算进相对系，
 * 然后打 `buildShadowVolumes` 接缝。
 * 🔴 相对坐标的减法一律走 `toRelative`（全仓唯一入口），本文件不自己减原点。
 */
Reading runFrame(const Scene& s, const glm::dvec3& origin) {
	const glm::vec3 eyeRel = toRelative(s.eye, origin);
	const glm::vec3 tgtRel = toRelative(s.target, origin);
	ViewProjection vp;
	vp.view = glm::lookAt(eyeRel, tgtRel, glm::vec3{0.0f, 1.0f, 0.0f});
	vp.proj = glm::perspective(glm::radians(kFovYDeg), kAspect, kNearRaw, kFarRaw);
	vp.nearPlane = kNearRaw;
	vp.farPlane = kFarRaw;

	std::vector<ShadowCandidate> rel;
	rel.reserve(s.cands.size());
	for (const ShadowCandidate& c : s.cands) {
		ShadowCandidate rc = c;
		rc.center = toRelative(glm::dvec3{c.center}, origin);
		rel.push_back(rc);
	}

	const uint32_t slots = std::max<uint32_t>(1u, static_cast<uint32_t>(rel.size()));
	std::vector<ShadowDrawArgs> args(static_cast<std::size_t>(kActiveShadowCascades) * slots);
	std::vector<ShadowMeshletDrawArgs> marg(args.size()), pargs(args.size());
	std::array<std::vector<DrawCall>, kActiveShadowCascades> draws;

	Reading out;
	out.candCount = rel.size();
	out.slices = computeCascadeSpheres(vp, quantizeShadowDistance(kNearRaw),
	                                   quantizeShadowDistance(kFarRaw));
	out.volumes = buildShadowVolumes(kSun, out.slices, rel, slots, args, marg, pargs, draws,
	                                 origin);

	const glm::dvec3 d = lightDir();
	const ClosedBox box = casterBox(rel);
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const double r = out.slices[c].radius;
		out.texel[c] = 2.0 * r / static_cast<double>(kTile);
		out.extentL[c] = extentAlong(box, d);
		const glm::dvec3 mid = 0.5 * (box.lo + box.hi) - glm::dvec3{out.slices[c].center};
		const double m0 = glm::dot(mid, d);
		out.hull0[c] = hullLength(m0, out.extentL[c], r);
		// 含住 ⟺ 候选区间比切片区间两头各宽 ≥ 一枚纹素（再留 1 mm 浮点余量）
		out.contains[c] = (m0 - 0.5 * out.extentL[c] < -r - out.texel[c] - 1.0e-3)
		                    && (m0 + 0.5 * out.extentL[c] > r + out.texel[c] + 1.0e-3);
	}
	return out;
}

/** 一级盒的深度跨度 `tMax − tMin`（米）。 */
double spanOf(const ShadowVolume& v) {
	return static_cast<double>(v.tMax) - static_cast<double>(v.tMin);
}

/** 把跨度换算成「NDC 域常数偏置的世界当量」，单位毫米（债-150 那条耦合的本体）。 */
double biasWorldMm(double spanM) {
	return static_cast<double>(kShadowDepthBias) * spanM * 1000.0;
}

/** 闭式上下界（通式）：`H(m0) ± 半纹素 + 2pad`，两侧都查。 */
void checkClosedFormBand(const Reading& r, uint32_t c, double slack = kFloatSlackM) {
	const double D = spanOf(r.volumes[c]);
	const double half = 0.5 * r.texel[c];
	const double pad = kPadScale * static_cast<double>(r.slices[c].radius);
	INFO("级 " << c << "：D = " << D << " 凸包 H = " << r.hull0[c] << " L = " << r.extentL[c]
	           << " r = " << r.slices[c].radius << " 纹素 = " << r.texel[c]);
	CHECK(D >= r.hull0[c] - half - 2.0 * pad - slack);
	CHECK(D <= r.hull0[c] + half + 2.0 * pad + slack);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// 自证：前提 + 激励有区分力（这条不读生产产物，M1 下也必须绿）
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * 前提守卫 + 激励有区分力（**纯测试侧算术**，一次都不碰生产）。
 *
 * 为什么单独一条：本项目已被「注入量根本没有区分力 / 判据在失效模式上饱和」咬过四次
 * （docs/相对坐标地基_plan.md §3 批 5「判据失明」四例）。本文件全部判据都建立在「候选集整个
 * 偏到零点一侧」这个前提上，而**前提不成立时 M1 也不会红**——那种「守卫其实什么都没守」的
 * 形态只能靠这条自证拦住。
 * 判据：① 判据对象三根轴都不跨零点、且离零点 ≥ 20 m；② 反向对照（出厂形态）确实跨零点；
 * ③ 「把零点算进 AABB」会让候选集在光轴上的投影长度**至少多出 50 m**（= M1 的信号下限）。
 */
TEST_CASE("债-149 守卫的自证：候选集确实整个偏在零点一侧，且旧形态会让跨度多出上百米") {
	const glm::dvec3 d = lightDir();

	// ① 判据对象：偏到一侧
	const Scene s = sceneAtBase(kOneSidedBase);
	const ClosedBox box = casterBox(s.cands);
	REQUIRE(!s.cands.empty());  // 空集合上的「都满足」恒真 = 假绿
	CHECK(!zeroInside(box));
	CHECK(minAbsComponent(box) >= 20.0);
	MESSAGE("【前提】判据候选集的 AABB x/y/z = [" << box.lo.x << ", " << box.hi.x << "] / ["
	        << box.lo.y << ", " << box.hi.y << "] / [" << box.lo.z << ", " << box.hi.z
	        << "]，离零点最小 " << minAbsComponent(box) << " m");

	// ② 反向对照：出厂形态确实跨过零点（⟹ 修复对它是空操作 = 交付锚不动的原因）
	CHECK(zeroInside(casterBox(straddlingCands())));

	// ③ 激励有区分力：旧形态（零点进盒）在光轴上多出来的长度
	const double L = extentAlong(box, d);
	const double lOld = extentAlong(unionWithZero(box), d);
	MESSAGE("【区分力】闭式紧 AABB 投影长度 L = " << L << " m；把零点算进去 ⟹ " << lOld
	        << " m（多出 " << lOld - L << " m）");
	CHECK(L > 10.0);           // 跨度本身要够大，②-A 的毫米数才与台账同量级
	CHECK(L < 25.0);           // 且离台账的 16.2 m 在同一量级（②-A 交叉核对的前提）
	CHECK(lOld - L >= 50.0);   // M1 的信号下限：比本文件任何一条容差大五个数量级

	// ④ 反向半边（变异红线 M4 的落点）：搬回跨过零点 ⟹ 区分力消失
	const ClosedBox straddle = casterBox(straddlingCands());
	CHECK(extentAlong(unionWithZero(straddle), d) - extentAlong(straddle, d) < 1.0e-12);
}

/**
 * 量具自检：闭式算子自己要先没写错（否则下面所有读数都可能是量具的 bug）。
 * 两条独立判据：support function 闭式与「八个角点暴力取极值」逐数对账（本文件没吃过的另一条
 * 路）；凸包算子按定义手算三形态（内含 / 分离 / 部分重叠）。
 */
TEST_CASE("债-149 守卫的量具自检：support function 闭式与八角点暴力枚举逐数相同") {
	const glm::dvec3 d = lightDir();
	Lcg rng{kSeedAabb};
	int checked = 0;
	for (int trial = 0; trial < 24; ++trial) {
		const ClosedBox b{glm::dvec3{rng.uniform(-40.0, -5.0), rng.uniform(-40.0, -5.0),
		                             rng.uniform(-40.0, -5.0)},
		                  glm::dvec3{rng.uniform(5.0, 40.0), rng.uniform(5.0, 40.0),
		                             rng.uniform(5.0, 40.0)}};
		// 暴力：八个角点里取 dot 的最大/最小（与 Σ|d_k|·size_k 不同路的一条）
		double lo = 1e300, hi = -1e300;
		for (const double sx : {0.0, 1.0})
			for (const double sy : {0.0, 1.0})
				for (const double sz : {0.0, 1.0}) {
					const glm::dvec3 corner{sx == 0.0 ? b.lo.x : b.hi.x,
					                       sy == 0.0 ? b.lo.y : b.hi.y,
					                       sz == 0.0 ? b.lo.z : b.hi.z};
					const double t = glm::dot(corner, d);
					lo = std::min(lo, t);
					hi = std::max(hi, t);
				}
		INFO("trial = " << trial);
		testing::expectNear(extentAlong(b, d), hi - lo, 1e-12, 1e-12);
		++checked;
	}
	CHECK(checked == 24);  // 空转守卫：一条都没比过就不许绿

	// 凸包算子按定义手算：内含 ⟹ 只剩 2r；分离 ⟹ 空档算进去；部分重叠
	// （手算逐条：max(r, m+L/2) − min(−r, m−L/2)，例 m=2,L=4,r=1 ⟹ max(1,4) − min(−1,0) = 5）
	testing::expectNear(hullLength(0.0, 1.0, 2.0), 4.0, 1e-12, 0.0);
	testing::expectNear(hullLength(10.0, 1.0, 2.0), 12.5, 1e-12, 0.0);
	testing::expectNear(hullLength(2.0, 4.0, 1.0), 5.0, 1e-12, 0.0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ①-A 闭式对账：跨度 = 候选 AABB 在光轴上的投影长度 + 2pad，与离零点距离无关
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * 🔴 断言 ① 的正半边：逐级跨度必须落在**闭式凸包**的界内，而该界不含「离零点多远」。
 *
 * 期望值出处 = 文件头那两条闭式（support function + 凸包 + 吸附位移 ≤ 半纹素）。
 * 主用例的候选集刻意做成「含住切片区间」的形态 ⟹ 凸包退化成 `L`、且与吸附相位无关
 * ⟹ 判据收在**一枚纹素之下**（而不是「小于某个绝对数」那种在 Δ 小时恒成立的假判据）。
 * 反向半边（下界）由变异 M2 验：AABB 只吃第一个候选时跨度会**掉到下界之下**。
 */
TEST_CASE("①-A 闭式对账：跨度 = 候选 AABB 光轴投影长度 + 2pad（含住形态取等号），与离零点距离无关") {
	int tightCases = 0;
	for (const double delta : kDeltaLadder) {
		const Scene s = sceneAtBase(kOneSidedBase + delta);
		const Reading r = runFrame(s, glm::dvec3{0.0});
		REQUIRE(r.candCount == 8u);  // 前提：候选非空
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
			INFO("Δ = " << delta << " 级 " << c);
			checkClosedFormBand(r, c);
			const double D = spanOf(r.volumes[c]);
			const double pad = kPadScale * static_cast<double>(r.slices[c].radius);
			if (r.contains[c]) {
				// 紧式：D = L + 2pad，与吸附相位无关 ⟹ 容差只吃浮点残量
				testing::expectNear(D, r.extentL[c] + 2.0 * pad, 0.0,
				                    0.5 * r.texel[c] + kFloatSlackM);
				++tightCases;
			}
			MESSAGE("【读数 ①-A】Δ = " << delta << " 级 " << c << "：跨度 D = " << D
			        << " m，闭式 L = " << r.extentL[c] << " m，凸包 H = " << r.hull0[c]
			        << " m，2pad = " << 2.0 * pad << " m，纹素 = " << r.texel[c]
			        << " m，含住 = " << (r.contains[c] ? "是" : "否")
			        << " ⟹ 对闭式 (L+2pad) 的残差 = "
			        << (D - (r.extentL[c] + 2.0 * pad)) / r.texel[c] << " 纹素");
		}
	}
	CHECK(tightCases == 4 * static_cast<int>(kActiveShadowCascades));  // 紧式真的被走到过
}

// ═══════════════════════════════════════════════════════════════════════════════
// ①-B 反向半边（本条的灵魂）：整组候选沿光轴平移 Δ，跨度不变
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * 🔴🔴 断言 ① 的灵魂（债-149 的失效形态被这一条直接钉死）：把**整组候选**（连同相机，
 * 相对几何不变）沿光轴正方向平移 Δ，逐级跨度必须**不变**，容差 ≤ 1 枚该级纹素。
 *
 * 为什么必须是这条而不是「跨度小于某个绝对数」：绝对数在 Δ 小的时候恒成立 = 假判据。
 * 旧代码下跨度随 Δ **线性增长**（台账实测 16.2 m → 164.3 m；本文件的 Δ 阶梯上会读到
 * ~68 m → ~360 m），新代码下它必须恒定。这条不变量对「场景在哪」完全不敏感，正是债-150
 * 行末写的那句「光空间盒深度跨度对场景整体平移不变」。
 * 容差 1 纹素的出处：盒心按纹素格吸附，Δ 不是格距整数倍时吸附相位最多挪半纹素，凸包长度对
 * 相位是 1-Lipschitz ⟹ 半纹素；再留半纹素给 float 落库。
 */
TEST_CASE("🔴 ①-B 整组候选沿光轴平移 Δ ⟹ 逐级跨度不变（≤ 1 纹素）") {
	const Reading base = runFrame(sceneAtBase(kOneSidedBase), glm::dvec3{0.0});
	REQUIRE(base.candCount == 8u);
	int compared = 0;
	for (const double delta : kDeltaLadder) {
		if (delta == 0.0)
			continue;
		const Reading r = runFrame(sceneAtBase(kOneSidedBase + delta), glm::dvec3{0.0});
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
			INFO("Δ = " << delta << " 级 " << c);
			const double drift = spanOf(r.volumes[c]) - spanOf(base.volumes[c]);
			CHECK(std::abs(drift) <= 1.0 * r.texel[c]);
			// 前提：切片球半径与平移无关（不是「盒坏了」而是「场景搬了」）
			testing::expectNear(static_cast<double>(r.slices[c].radius),
			                    static_cast<double>(base.slices[c].radius), 1e-4, 0.0);
			MESSAGE("【读数 ①-B】Δ = " << delta << " m 级 " << c << "：跨度 "
			        << spanOf(r.volumes[c]) << " m vs 基准 " << spanOf(base.volumes[c])
			        << " m ⟹ 漂移 = " << drift / r.texel[c] << " 纹素（界 1）");
			++compared;
		}
	}
	CHECK(compared == 3 * static_cast<int>(kActiveShadowCascades));  // 空转守卫
}

// ═══════════════════════════════════════════════════════════════════════════════
// ①-C 债-149 的字面形态：同一场景换「坐标系零点」，跨度不变
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * 🔴 断言 ① 的第二种喂法（自变量与 ①-B 不同，钉的是同一条缺陷的另一半）：
 * **场景一动不动，只换渲染原点**（= 门开态那套坐标系的零点），逐级跨度必须不变。
 *
 * 为什么 ①-B 不够：①-B 里相机跟着一起搬，切片球也搬；本条里**只有原点搬**，切片球与候选
 * 在相对系里整体平移而绝对几何一动不动。债-149 结案批写得很清楚——病灶形态不是「绝对量参与
 * 判据」而是「**坐标系零点参与判据**：门关零点 = 世界原点、门开零点 = 渲染原点，两者都不是
 * 场景所在的地方」。决定性实验就是「场景不动、把渲染原点强行写死 300 / 1e6 ⟹ 阴影 0 像素」，
 * 本条是它的单测版。
 * 原点取 +X 方向（与 ①-B 的光轴方向不共线 ⟹ 两根自变量各自独立，`|d·轴|` 的折算系数也不同）。
 */
TEST_CASE("🔴 ①-C 场景不动、只换坐标系零点（渲染原点沿 +X）⟹ 逐级跨度不变（≤ 1 纹素）") {
	const Scene s = sceneAtBase(kOneSidedBase);
	const Reading base = runFrame(s, glm::dvec3{0.0});
	REQUIRE(base.candCount == 8u);
	int compared = 0;
	for (const double delta : kDeltaLadder) {
		if (delta == 0.0)
			continue;
		const Reading r = runFrame(s, glm::dvec3{delta, 0.0, 0.0});
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
			INFO("origin.x = " << delta << " 级 " << c);
			const double drift = spanOf(r.volumes[c]) - spanOf(base.volumes[c]);
			CHECK(std::abs(drift) <= 1.0 * r.texel[c]);
			MESSAGE("【读数 ①-C】原点 = +" << delta << " m·X 级 " << c << "：跨度 "
			        << spanOf(r.volumes[c]) << " m vs 基准 " << spanOf(base.volumes[c])
			        << " m ⟹ 漂移 = " << drift / r.texel[c] << " 纹素（界 1）");
			++compared;
		}
	}
	CHECK(compared == 3 * static_cast<int>(kActiveShadowCascades));  // 空转守卫
}

// ═══════════════════════════════════════════════════════════════════════════════
// ①-D 属性测试：随机候选集（写死种子）也落在闭式界内，两种形态都在
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * 断言 ① 的一般形态（不要求「含住」）：随机造 32 组「整个偏在零点一侧」的候选集 + 随机
 * 离零点距离，逐级跨度落在闭式凸包**双侧**界内。
 *
 * 为什么还要这条：①-A/B/C 的候选集是**手挑的**（8 球壳、含住形态），它绿可能只是因为那组
 * 输入恰好让两种写法同值。随机集把「候选 AABB 的形状 / 离零点的距离 / 与切片球的相对位置」
 * 三样一起扫开。随机性 = `Lcg` + 写死种子（失败可重放）。
 * 界是**双侧**的：下界拦 M2 那种「AABB 变小」，上界拦 M1/M3 那种「AABB 变大」。
 * 退化守卫：样本里必须**同时**出现「含住」与「不含住」两种形态，否则这条读起来像覆盖了
 * 一般情形、实际只覆盖了一种。
 */
TEST_CASE("①-D 属性测试：随机偏侧候选集 × 随机离零点距离，逐级跨度落在闭式凸包双侧界内") {
	Lcg rng{kSeedAabb + 7u};
	const glm::dvec3 d = lightDir();
	int cases = 0, tight = 0, loose = 0, precondition = 0;
	for (int trial = 0; trial < 32; ++trial) {
		const double base = kOneSidedBase + rng.uniform(-25.0, 60.0);
		Scene s = sceneAtBase(base);
		// 在「以 target 为中心的 8 球壳」上再抖一下形状（球数 / 半径 / 位置一起变）
		const int n = 1 + static_cast<int>(rng.uniform01() * 6.0);
		std::vector<ShadowCandidate> cands;
		for (int i = 0; i < n; ++i) {
			const glm::dvec3 jitter{rng.uniform(-9.0, 9.0), rng.uniform(-7.0, 7.0),
			                        rng.uniform(-8.0, 8.0)};
			cands.push_back(makeCand(s.target + jitter, static_cast<float>(rng.uniform(0.2, 1.5)),
			                         static_cast<uint32_t>(i)));
		}
		s.cands = cands;
		const Reading r = runFrame(s, glm::dvec3{0.0});
		REQUIRE(r.candCount == static_cast<std::size_t>(n));
		if (!zeroInside(casterBox(s.cands)))
			++precondition;
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
			INFO("trial = " << trial << " base = " << base << " 级 " << c);
			checkClosedFormBand(r, c);
			(r.contains[c] ? tight : loose) += 1;
			++cases;
		}
	}
	// 空转/退化守卫：32 组 × 4 级都真的比过，且两种形态都出现过
	CHECK(cases == 32 * static_cast<int>(kActiveShadowCascades));
	CHECK(precondition == 32);  // 前提：每一组也确实偏在零点一侧
	CHECK(tight > 0);
	CHECK(loose > 0);
	MESSAGE("【读数 ①-D】比过 " << cases << " 个 (组 × 级)：含住 " << tight << " / 不含住 "
	        << loose << "（两种形态都在 ⟹ 界不是只在一种输入下成立）；离零点距离扫了 "
	        << (kOneSidedBase - 25.0) << "…" << (kOneSidedBase + 60.0) << " m，"
	        << "闭式界对 L 的依赖只走 Σ|d_k|·size_k，不含距离项");
	(void)d;
}

// ═══════════════════════════════════════════════════════════════════════════════
// ①-E 反向用例：出厂形态（跨过零点）与空候选集
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * 反向用例（「不该触发」那一半）：候选集**跨过零点**时，加不加零点逐位同值。
 *
 * 这条同时给出两件事：① 债-149 的修复对出厂场景是空操作 ⟹ 门关交付锚 `efa70587…` 逐位不变
 * 的**函数级解释**（截图锚只说「图没变」，这条说「为什么没变」）；② 如果哪天有人把初值改成
 * 别的非候选值（例如 `{1e9f}` 或「切片球心」），本条会红——它对「初值取什么」不是瞎的。
 * 判据 = 跨度落在同一个闭式界内（闭式吃的是**紧 AABB**，与加不加零点无关）。
 */
TEST_CASE("①-E 反向用例：出厂形态（候选集跨过零点）跨度仍是同一个闭式值；空候选集退化成 2r+2pad") {
	const Scene factory{.eye = kEyeAbs, .target = kTargetAbs, .cands = straddlingCands()};
	const Reading r = runFrame(factory, glm::dvec3{0.0});
	REQUIRE(r.candCount == 8u);
	CHECK(zeroInside(casterBox(factory.cands)));
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		checkClosedFormBand(r, c);
		MESSAGE("【读数 ①-E】出厂形态 级 " << c << "：跨度 = " << spanOf(r.volumes[c])
		        << " m，闭式 L = " << r.extentL[c] << " m（候选集跨过零点 ⟹ 旧代码与新代码同值）");
	}

	// 空候选集：定义处注释「AABB 退化成一个点：盒只剩受光面那层」——本条钉住这个现状。
	// ⚠ 实测**不是** 2r + 2pad：退化出来的那个点是**坐标系统的零点**，它在光轴上的投影
	//   m = −dot(切片球心, d) 一般落在 [−r, r] **之外**（出厂机位下 c0 读到 |m| ≈ 0.45 m
	//   vs r = 0.26 m）⟹ 盒被这个点撑大到凸包长度。这正是「零点参与判据」在空集上**仍然存在**
	//   的形态，只是没有候选就没有任何 draw、画面上等价于无阴影。本条只记录现状、不判对错
	//   （真要改得先想清楚空集时盒该在哪），但把它写成断言——哪天有人动这里，读数会出声。
	const Scene empty{.eye = kEyeAbs, .target = kTargetAbs, .cands = {}};
	const Reading e = runFrame(empty, glm::dvec3{0.0});
	REQUIRE(e.candCount == 0u);
	const glm::dvec3 d = lightDir();
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const double r = static_cast<double>(e.slices[c].radius);
		const double pad = kPadScale * r;
		// 闭式：候选区间退化成一个点 m = dot(零点 − 切片球心, d)（L = 0）
		const double m = -glm::dot(glm::dvec3{e.slices[c].center}, d);
		testing::expectNear(spanOf(e.volumes[c]), hullLength(m, 0.0, r) + 2.0 * pad, 1e-5,
		                    kFloatSlackM);
		MESSAGE("【读数 ①-E】空候选集 级 " << c << "：跨度 = " << spanOf(e.volumes[c])
		        << " m，2r+2pad = " << 2.0 * r + 2.0 * pad << " m ⟹ 被零点撑大 "
		        << spanOf(e.volumes[c]) - (2.0 * r + 2.0 * pad) << " m（凸包闭式对上了）");
	}
}

// ═══════════════════════════════════════════════════════════════════════════════
// ② 偏置的世界当量：把债-150 的耦合钉成可见的数
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * ②-A 记录性断言：出厂跨度下 `kShadowDepthBias × D` 的世界当量是多少**毫米**。
 *
 * 期望值出处（两条，互不相关）：
 *  ① 闭式：`D = L + 2pad`（①-A 已证）⟹ 当量 = `bias × (L + 2pad)`，L 由本文件的候选集定义
 *     （15.88 m），不来自被测码；
 *  ② 实机：`docs/还债总账.md` 债-150 行「合成场景出厂跨度 16.2 m ⟹ 当量 24 mm」。本文件的
 *     候选集刻意做成同量级跨度，两条应当对上（界取 ±25 %，覆盖刻意留的取值余量）。
 * 🔴 为什么这条不是恒真式：「`bias × D` 对 D 是线性的」确实是恒真，**本条不测乘法**，测的是
 * `D` 这个由生产级联几何 + 候选 AABB 算出来的数落在哪，以及它与一次实机读数的量级关系。
 * 有人调 `kShadowDepthBias` / 改级联深度界 / 改 AABB 初值，本条都会红。
 */
TEST_CASE("②-A 记录：出厂跨度下 NDC 偏置的世界当量 = 24 mm 量级（与债-150 台账实机读数交叉核对）") {
	const Reading r = runFrame(sceneAtBase(kOneSidedBase), glm::dvec3{0.0});
	REQUIRE(r.candCount == 8u);
	int pinned = 0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		INFO("级 " << c);
		const double D = spanOf(r.volumes[c]);
		const double mm = biasWorldMm(D);
		// ① 闭式期望（容差 = bias × 半纹素，再加 1e-2 mm 的浮点余量）
		const double pad = kPadScale * static_cast<double>(r.slices[c].radius);
		const double closedMm = biasWorldMm(r.extentL[c] + 2.0 * pad);
		testing::expectNear(mm, closedMm, 0.0,
		                    biasWorldMm(0.5 * r.texel[c]) + 1.0e-2);
		// ② 与台账实机读数（24.3 mm @ 16.2 m 跨度）交叉核对
		CHECK(mm >= 24.3 * 0.75);
		CHECK(mm <= 24.3 * 1.25);
		MESSAGE("【读数 ②-A】级 " << c << "：跨度 D = " << D << " m ⟹ NDC 偏置 "
		        << kShadowDepthBias << " 的世界当量 = " << mm << " mm（闭式 " << closedMm
		        << " mm，债-150 台账 24.3 mm）");
		++pinned;
	}
	CHECK(pinned == static_cast<int>(kActiveShadowCascades));  // 空转守卫
}

/**
 * 🔴 ②-B 反向半边：偏置的世界当量对「场景离坐标系统零点有多远」**不变**。
 *
 * 这是 ①-B/①-C 的物理单位版本，也是债-150 真正要守的东西：`kShadowDepthBias` 住在 NDC 域，
 * 它的物理含义 = `bias × 跨度`。跨度一旦随场景摆位漂（= 债-149 的形态），**「阴影偏置」这个
 * 旋钮的实际毫米数就跟着漂**，而调参的人看的是配置里的常数，不会知道。本条把这件事钉成
 * 「同一个常数在四种摆位 × 两种原点下必须是同一个毫米数」，容差 = `bias × 1 纹素`。
 * M1 下本条必红（会读到 24 mm → 上百 mm）。
 */
TEST_CASE("🔴 ②-B NDC 偏置的世界当量（毫米）对场景摆位与渲染原点都不变") {
	const Reading base = runFrame(sceneAtBase(kOneSidedBase), glm::dvec3{0.0});
	REQUIRE(base.candCount == 8u);
	int compared = 0;
	for (const double delta : kDeltaLadder) {
		if (delta == 0.0)
			continue;
		const auto check = [&](const Scene& s, const glm::dvec3& origin, const std::string& what) {
			const Reading r = runFrame(s, origin);
			for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
				INFO(what << " Δ = " << delta << " 级 " << c);
				const double mm = biasWorldMm(spanOf(r.volumes[c]));
				const double ref0 = biasWorldMm(spanOf(base.volumes[c]));
				const double bound = 2.0 * biasWorldMm(r.texel[c] + kFloatSlackM);
				CHECK(std::abs(mm - ref0) <= bound);
				MESSAGE("【读数 ②-B】" << what << " Δ = " << delta << " 级 " << c << "：当量 = "
				        << mm << " mm（基准 " << ref0 << " mm，界 ±" << bound << " mm）");
				++compared;
			}
		};
		check(sceneAtBase(kOneSidedBase + delta), glm::dvec3{0.0}, "沿光轴平移整组候选");
		check(sceneAtBase(kOneSidedBase), glm::dvec3{delta, 0.0, 0.0}, "换渲染原点");
	}
	CHECK(compared == 6 * static_cast<int>(kActiveShadowCascades));  // 空转守卫
}

/**
 * ②-C 记录：把 `shadowFar` 调短换性能，偏置的物理当量到底跟着动多少——**分两种跨度形态量**。
 *
 * 🔴 这条不是恒真的乘法：自变量是 `shadowFar`（性能旋钮），因变量是跨度，中间隔着
 * `quantizeShadowDistance → cascadeSplitDistance → frustumBoundingSphere → 候选 AABB 的凸包`
 * 一整条链。跨度是**两段的凸包**：候选 AABB 的投影（与 shadowFar 无关）与切片球的 `[−r, r]`
 * （随 shadowFar 走）。哪一段主导，决定了这枚耦合有多大 —— 于是本条量两种形态：
 *  · **形态 A 候选主导**（合成靶出厂那种：一坨比视锥大的投影体）
 *  · **形态 B 级联球主导**（一小团投影体挂在视锥中段，四级里越粗的级越主导）
 * 判据 = A 的比值必须 ≈ 1（界 ±1 %）、B 至少一级显著小于 1（界 0.99），两侧都有界。
 * ⚠ **本条实测与债-150 台账的措辞不符**（台账写「任何把 shadowFar 调短换性能的改动都会改变
 * 偏置的物理当量」）：在**候选 AABB 主导**的跨度下实测只动 0.03…0.2 %，量级来自形态 B。
 * 读数原样进交付报告，不为了让台账那句话成立而挑形态 B 的数字当代表。
 * 将来按债-150 把偏置改到世界光深域下发时本条会红——那是**正确**的：耦合被拆掉，记录必须重立。
 */
TEST_CASE("②-C 记录：shadowFar 换性能改多少偏置物理当量，取决于跨度由候选 AABB 还是级联球主导") {
	/** 给定场景与 far，取四级跨度（其余配置一律不动 ⟹ 单变量）。 */
	auto spansAtFar = [](const Scene& s, float farRaw) {
		const glm::vec3 eyeRel = toRelative(s.eye, glm::dvec3{0.0});
		const glm::vec3 tgtRel = toRelative(s.target, glm::dvec3{0.0});
		ViewProjection vp;
		vp.view = glm::lookAt(eyeRel, tgtRel, glm::vec3{0.0f, 1.0f, 0.0f});
		vp.proj = glm::perspective(glm::radians(kFovYDeg), kAspect, kNearRaw, farRaw);
		vp.nearPlane = kNearRaw;
		vp.farPlane = farRaw;
		std::vector<ShadowCandidate> rel;
		for (const ShadowCandidate& c : s.cands) {
			ShadowCandidate rc = c;
			rc.center = toRelative(glm::dvec3{c.center}, glm::dvec3{0.0});
			rel.push_back(rc);
		}
		const uint32_t slots = std::max<uint32_t>(1u, static_cast<uint32_t>(rel.size()));
		std::vector<ShadowDrawArgs> args(static_cast<std::size_t>(kActiveShadowCascades) * slots);
		std::vector<ShadowMeshletDrawArgs> marg(args.size()), pargs(args.size());
		std::array<std::vector<DrawCall>, kActiveShadowCascades> draws;
		const auto slices = computeCascadeSpheres(vp, quantizeShadowDistance(kNearRaw),
		                                          quantizeShadowDistance(farRaw));
		const auto vols = buildShadowVolumes(kSun, slices, rel, slots, args, marg, pargs, draws,
		                                     glm::dvec3{0.0});
		std::array<double, kActiveShadowCascades> out{};
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
			out[c] = spanOf(vols[c]);
		return out;
	};

	// 形态 A：候选 AABB 主导（本文件主用例那坨 8 球壳，闭式 L = 15.88 m ≫ 任何一级 2r）
	const Scene casterDominated = sceneAtBase(kOneSidedBase);
	// 形态 B：一小团投影体挂在视锥中段（直径 0.3 m ≪ 粗级 2r）⟹ 越粗的级越由级联球主导。
	// 🔴 相机与形态 A **同一摆位**（唯一自变量是候选团的大小，不是它离零点多远）
	const glm::dvec3 axis = glm::normalize(casterDominated.target - casterDominated.eye);
	const glm::dvec3 midView = casterDominated.eye
	                           + axis * (0.5 * static_cast<double>(quantizeShadowDistance(kFarRaw)));
	const Scene sliceDominated{.eye = casterDominated.eye, .target = casterDominated.target,
	                           .cands = shell8(midView, glm::dvec3{0.15, 0.15, 0.15}, 0.05f)};

	struct Regime {
		const Scene& scene;
		std::string what;
		double loBound;  // 比值的下界断言
		double hiBound;  // 比值的上界断言
	};
	const Regime regimes[2] = {
	    // A：跨度几乎全由候选 AABB 出 ⟹ 调 far 几乎不动当量（界 ±1 %）
	    Regime{.scene = casterDominated, .what = "A 候选 AABB 主导", .loBound = 0.99, .hiBound = 1.0001},
	    // B：越粗的级越由级联球出 ⟹ 当量跟着 far 走（必须至少一级显著小于 1）
	    Regime{.scene = sliceDominated, .what = "B 级联球主导", .loBound = 0.05, .hiBound = 1.0001},
	};
	int compared = 0;
	bool bMoved = false;
	for (const Regime& g : regimes) {
		const auto full = spansAtFar(g.scene, kFarRaw);
		const auto half = spansAtFar(g.scene, 0.5f * kFarRaw);
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
			INFO(g.what << " 级 " << c);
			REQUIRE(full[c] > 0.0);
			const double ratio = biasWorldMm(half[c]) / biasWorldMm(full[c]);
			CHECK(ratio >= g.loBound);
			CHECK(ratio <= g.hiBound);
			bMoved = bMoved || ratio <= 0.99;
			MESSAGE("【读数 ②-C】" << g.what << "：shadowFar " << kFarRaw << " → "
			        << 0.5f * kFarRaw << " m ⟹ 跨度 " << full[c] << " → " << half[c]
			        << " m ⟹ 偏置当量 " << biasWorldMm(full[c]) << " → " << biasWorldMm(half[c])
			        << " mm（比值 " << ratio << "，界 [" << g.loBound << ", " << g.hiBound << "]）");
			++compared;
		}
	}
	CHECK(compared == 2 * static_cast<int>(kActiveShadowCascades));
	// 🔴 反向半边：若两种形态的比值全都 ≈ 1，说明这枚耦合在本文件的输入下**根本量不出来**，
	//    那这条记录就是假判据（同「注入量无区分力」那一族）。bMoved 就是它的自证。
	CHECK(bMoved);
	MESSAGE("【读数 ②-C】结论：耦合真实存在但**不是**「调短一半就减半」——形态 A 几乎不动、"
	        "形态 B 的粗级才跟着走（债-150 拍板修法前，这是它唯一的可见面）");
}

}  // namespace RGL
