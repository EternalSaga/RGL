module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // lookAt / perspective / translate / scale（只造输入，不参与期望值）

/**
 * test_meshlet_cull —— CPU 参考簇剔除器的契约（簇化轴 ★0.5-a）
 * 被测对象 = `rgl/meshlet_cull.cppm`（rlbase，纯 CPU 无设备）。
 *
 * ## 这批为什么值得单独一层测试（而不是等 ★1-a 一起验）
 * GPU 版簇剔除「剔多了」在图像上 = 屏幕少一块，与「材质本来就黑」「几何本来就看不见」**不可区分**。
 * 本模块是它的对账伙伴，所以伙伴自己必须先是对的——而它对的判据只有一条：闭式几何。
 *
 * ## 期望值出处（全部闭式，不取被测码）
 * 每条用例的期望可见集都由「球心到平面的有符号距离」手算：`d = plane·(center,1)`，现行保守判据
 * = **任一平面 d < −r 才剔**（`scene_context.cppm::sphereInFrustum`）。两个专用视锥输入：
 *
 *  - `boxFrustum()`：轴对齐盒 x∈(−1,1)、y∈(−1,1)、z∈(−2,−1)，六面**单位法线朝内**，偏移 ±1/±2
 *    全是二进制精确值 ⟹ 相切那条的浮点比较**逐位**成立（不靠容差）。
 *    刻意**不是**透视视锥：透视的 4 个斜面法线含 tan(fov/2)、且一个点常同时被多个斜面判负，
 *    「只坏某一个平面」的变异会被面间耦合掩盖；盒的六面各自独立 ⟹ 逐面归因干净。
 *    near/far 的**朝内方向与真相机一致**（near 法线 = 视向 −Z，内含侧 z < −near）。
 *  - `cameraFrustum()`：真机位（eye 原点、看向 −Z、fovY 90°、aspect 1、near 1、far 100）经
 *    `extractFrustum(proj·view)` 提取 ⟹ 钉住「产物球经 objectMatrix 到世界后喂进现行视锥约定」
 *    这条端到端链路。归一化后的平面（**独立探针实测**，`/tmp/opencode/frustum_probe.cpp`）：
 *      left/right = (∓0.7071, 0, −0.7071, 0)  bottom/top = (0, ∓0.7071, −0.7071, 0)
 *      near = (0,0,−1,−1)  far = (0,0,1,100.0005)   ⟹ d_near = −z−1、d_far = −z−100(±1e-3)
 *    本文件里所有相机视锥用例的期望距离都取自这组闭式值，且判据余量 ≥ 7×（浮点误差不参与结论）。
 *
 * ## 🔴 六面归因的阳性锚点（每条外侧用例都配一条反向断言）
 * 每条「球在平面 k 外侧 ⟹ 被剔」同时断言「把平面 k 换成**零平面** (0,0,0,0) ⟹ 复活」。
 * 零平面距离恒 0，`0 < −r`（r ≥ 0）恒假 ⟹ 该面永不剔除。这条反向断言挡的是：球其实被**别的**面
 * 剔掉（坐标算错 / 六面不独立）时外侧断言会假绿；配上「摘掉 k 面就复活」才证明**只有 k 面负责**，
 * 「只坏某一个平面」的变异必红。
 *
 * ## 变异红线（**逐条实测**于 2026-10-12，基线 = 323 用例 / 68719 断言全绿）
 * 形态编号 = harness（`/tmp/opencode/mutation2.py`，临时件）里的编号，主 agent 复跑用同一套。
 * 每条都跑满「注入 → 重建（查返回码）→ 跑测试 → 还原 → 重建 → **确认回到 323 全绿**」，
 * 最后那步是闸门：还原没生效就跑下一条 = 拿上一条变异的二进制读数（本批实测踩过 `shutil.copy2`
 * 保留旧 mtime ⟹ ninja 判定无需重编，M8 的 `<=` 留在 librlscene 里污染了后面一整轮）。
 *
 *  | 变异形态 | 红 | 红的是哪些（实测名单） |
 *  |---|---|---|
 *  | M1 **剔除判据整段失效**（`sphereInFrustum(...) && false`） | **13** | 六面外侧 6 + 相机背后 + 零半径球 + 半径缩放（scale=0.01 剔除臂、非均匀臂）+ 三角合计 + 不碰层级 + 视锥约定 |
 *  | M2 **半径缩放写成取第一个轴**（`std::max({...})` → `length(m[0])`） | **1** | 只有「非均匀」那条 |
 *  | M2b 半径缩放取三轴**最小** | **1** | 同上（与 M2 同一条 ⟹ 上界写反成下界也逃不掉） |
 *  | M3 忘了把球心变换到世界 | **1** | scale=0.01「世界单位看得见」 |
 *  | M4 忘了缩放半径（传 `c.radius` 原值） | **2** | scale=0.01「世界单位看不见」+ 非均匀 |
 *  | M5 可见三角合计写成**条数**（`+= 1`） | **2** | 三角合计 + 不碰层级 |
 *  | M6 改成**走层级节点**（先遍历 `asset.nodes`） | **1** | 不碰层级 |
 *  | M70 只坏 left 面 | **4** | 外侧 left + 零半径球 + 三角合计 + 不碰层级 |
 *  | M71/M72 只坏 right / bottom 面 | **1 / 1** | 各自那一面的外侧用例 |
 *  | M73 只坏 top 面 | **4** | 外侧 top + 半径缩放两条 + 三角合计 |
 *  | M74/M75 只坏 near / far 面 | **2 / 2** | 各自那一面的外侧用例 + 「视锥约定」对账条 |
 *  | M8 保守边界写反（`<` → `<=`） | **2** | 相切 + 零半径点在面上（**只有这两条**是 `<`/`<=` 的哨兵） |
 *  | **MC 球心乘法次序翻转**（`M * vec4(c,1)` → `vec4(c,1) * M`）——**主 agent 质检变异** | 注入时 **0**（暴露缺口）→ 补用例后 **1** | 补的「含旋转的对象矩阵」那条 |
 *
 * 🔴 **MC 是本轮唯一"变异存活"的记录，别删**：注入 MC 后 323 条**全绿**。根因不是判据松，是
 * **输入形态缺维度**——其余用例的 `objectMatrix` 全是纯缩放或 `translate·scale`，且簇球心取源单位
 * **原点**，而 `w=1` 的向量在两种乘法次序下都取矩阵第 3 列 ⟹ 两种次序**恒等**（glm 下一般矩阵确实
 * 不同：探针 `M*c`=(5.15,1.40,−2.90) vs `c*M`=(−0.60,−0.35,−1.40)）。真资产的簇球心永不在原点、
 * modelMatrix 带旋转 ⟹ 这是会真发生的 bug。**教训：变异存活先问"我的输入有没有这个维度"，
 * 再问"判据是不是松"**——这次两者都成立，但只有前者是可修的。
 *
 * M1 那 13 条里**没有**「内含」「相切」「相交」「scale=0.01 可见臂」「空簇集」——它们断言的都是
 * 「可见」，而 M1 让一切都可见，自然全绿；这正是六条**外侧**用例必须逐面独立存在的理由。
 * M2 只红 1 条不是弱点，正是「非均匀那条必须**三条轴各一臂**都在」的根据：均匀臂与 x 轴臂单独
 * 都不红，任何「固定取某个轴」的写法都能蒙过单臂。M8 只红 2 条同理——相切与零半径点在面上
 * 是保守判据取等号的**唯一**两个入口，删掉它们，边界写反就没人管了。
 *
 * ## 输入规模纪律
 * 合成簇集 = 1–4 簇，测试自己构造 `MeshletAsset` 的内存视图（🔴 **不**碰 184MB 真产物、
 * **不**依赖构建产物）。选「直接构造视图」而不是「喂合成字节流给 `parseMeshletAsset`」的理由：
 * 剔除器只读 `clusters` 一个视图，字节流构造器的正确性已由 `test_meshlet_asset` 钉死，在这里
 * 复制一份只会多一处要跟着产物格式漂的副本。`header.clusterCount` 一并填成同值，防「实现哪天
 * 改读 header 计数」时本文件静默失真。
 */
export module test_meshlet_cull;

import rlstd;
import meshlet_asset;
import meshlet_cull;
import scene_context;

namespace RGL {
namespace {

/** 单位阵对象矩阵（源单位 == 世界单位；缩放类用例才换掉它）。 */
const glm::mat4 kIdentity{1.0f};

/** 合成簇集的规格：源单位包围球 + 三角数（其余字段剔除器不读，填合法值）。 */
struct ClusterSpec {
	glm::vec3 center{0.0f};
	float radius = 0.0f;
	uint32_t triCount = 1;
};

/**
 * 合成簇集夹具：簇记录存在本对象里，`asset.clusters` 指向它（同生命周期）。
 *
 * 禁拷贝禁移动：两者都会让 `asset.clusters` 悬在旧地址上（`MeshletAsset` 自己也是这么设计的）。
 * 先填满 `storage` 再取 span ⟹ 中途没有 realloc 窗口。
 */
struct ClusterFixture {
	/** 簇描述存储（`asset.clusters` 的指向目标）。 */
	std::vector<MeshletDiskCluster> storage;
	/** 交给被测函数的只读视图。 */
	MeshletAsset asset;

	/** @param specs 逐簇规格（下标即簇号；剔除结果按它升序返回） */
	explicit ClusterFixture(const std::vector<ClusterSpec>& specs) {
		storage.reserve(specs.size());
		for (const ClusterSpec& s : specs) {
			MeshletDiskCluster c{};
			c.group = 0;
			c.refined = -1;
			c.triCount = s.triCount;
			c.vertCount = 3;
			c.center[0] = s.center.x;
			c.center[1] = s.center.y;
			c.center[2] = s.center.z;
			c.radius = s.radius;
			storage.push_back(c);
		}
		asset.header.clusterCount = static_cast<uint32_t>(storage.size());
		asset.clusters = std::span<const MeshletDiskCluster>(storage);
	}
	ClusterFixture(const ClusterFixture&) = delete;
	ClusterFixture& operator=(const ClusterFixture&) = delete;
	ClusterFixture(ClusterFixture&&) = delete;
	ClusterFixture& operator=(ClusterFixture&&) = delete;
};

/**
 * 轴对齐盒视锥：x∈(−1,1)、y∈(−1,1)、z∈(−2,−1)。
 *
 * 六面**单位法线朝内**（`sphereInFrustum` 的前提），偏移 ±1 / ±2 二进制精确。平面序沿用
 * `Frustum::planes` 的词表（left/right/bottom/top/near/far），near/far 朝内方向与真相机一致
 * （near 内含侧 = z < −near）。为什么逐面归因用盒而不用透视视锥 → 文件头「期望值出处」。
 */
Frustum boxFrustum() {
	Frustum f{};
	f.planes[0] = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);    // left：d = x+1
	f.planes[1] = glm::vec4(-1.0f, 0.0f, 0.0f, 1.0f);   // right：d = −x+1
	f.planes[2] = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);    // bottom：d = y+1
	f.planes[3] = glm::vec4(0.0f, -1.0f, 0.0f, 1.0f);   // top：d = −y+1
	f.planes[4] = glm::vec4(0.0f, 0.0f, -1.0f, -1.0f);  // near：d = −z−1
	f.planes[5] = glm::vec4(0.0f, 0.0f, 1.0f, 2.0f);    // far：d = z+2
	return f;
}

/**
 * 把视锥第 k 面换成**零平面** (0,0,0,0)：距离恒 0，`0 < −r`（r ≥ 0）恒假 ⟹ 该面永不剔除。
 * 用途 = 阳性锚点「摘掉 k 面就复活」（见文件头）。
 */
Frustum planeDisabled(const Frustum& f, size_t k) {
	Frustum g = f;
	g.planes[k] = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
	return g;
}

/**
 * 真机位视锥：eye 原点、看向 −Z、up +Y、fovY 90°、aspect 1、near 1、far 100。
 * view 恰为单位阵，但**仍走 lookAt**——本文件要钉的是「现行 `extractFrustum` 产物」这条链路，
 * 不是「我能手搓矩阵」。归一化后的六面 = 文件头那组探针实测值。
 */
Frustum cameraFrustum() {
	const glm::mat4 view = glm::lookAt(glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, -1.0f},
	                                   glm::vec3{0.0f, 1.0f, 0.0f});
	const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 1.0f, 100.0f);
	return extractFrustum(proj * view);
}

/** 跑一次剔除（单簇集便捷入口）。 */
ClusterCullResult cullOne(const ClusterFixture& fx, const glm::mat4& m, const Frustum& f) {
	return cullClustersLinear(fx.asset, m, f);
}

/** 单簇夹具（六面用例的公共形状：一个球、一个对象矩阵、一个视锥）。 */
ClusterFixture oneCluster(glm::vec3 center, float radius) {
	return ClusterFixture{std::vector<ClusterSpec>{ClusterSpec{center, radius, 1}}};
}

}  // namespace

// ─── 内含 / 外侧（六面逐条，每条配「摘掉该面就复活」）─────────────────────────────

TEST_CASE("内含：球完全在视锥内 → 可见（阳性锚点：判据误杀一切时这条先红）") {
	// 闭式：盒内点 (0,0,−1.5) r=0.25 ⟹ 六面距离 = {1,1,1,1,0.5,0.5}，全 > −0.25 ⟹ 可见。
	const ClusterFixture fx = oneCluster(glm::vec3{0.0f, 0.0f, -1.5f}, 0.25f);
	const ClusterCullResult r = cullOne(fx, kIdentity, boxFrustum());
	CHECK(r.visibleClusterCount == 1);
	CHECK(r.visibleClusters == std::vector<uint32_t>{0});
	CHECK(r.visibleTriangles == 1);
}

TEST_CASE("外侧 left：球整体在 left 面外 → 剔除；摘掉 left 面 → 复活（证明只有 left 负责）") {
	// 闭式：球心 (−2,0,−1.5) r=0.25 ⟹ left d = −2+1 = −1 < −0.25；其余五面 = {3,1,1,0.5,0.5}。
	const ClusterFixture fx = oneCluster(glm::vec3{-2.0f, 0.0f, -1.5f}, 0.25f);
	const Frustum f = boxFrustum();
	INFO("平面 left = planes[0]");
	CHECK(cullOne(fx, kIdentity, f).visibleClusterCount == 0);
	CHECK(cullOne(fx, kIdentity, planeDisabled(f, 0)).visibleClusterCount == 1);
}

TEST_CASE("外侧 right：球整体在 right 面外 → 剔除；摘掉 right 面 → 复活") {
	// 闭式：球心 (2,0,−1.5) r=0.25 ⟹ right d = −2+1 = −1 < −0.25；其余五面 ≥ 0.5。
	const ClusterFixture fx = oneCluster(glm::vec3{2.0f, 0.0f, -1.5f}, 0.25f);
	const Frustum f = boxFrustum();
	INFO("平面 right = planes[1]");
	CHECK(cullOne(fx, kIdentity, f).visibleClusterCount == 0);
	CHECK(cullOne(fx, kIdentity, planeDisabled(f, 1)).visibleClusterCount == 1);
}

TEST_CASE("外侧 bottom：球整体在 bottom 面外 → 剔除；摘掉 bottom 面 → 复活") {
	// 闭式：球心 (0,−2,−1.5) r=0.25 ⟹ bottom d = −1 < −0.25；其余五面 ≥ 0.5。
	const ClusterFixture fx = oneCluster(glm::vec3{0.0f, -2.0f, -1.5f}, 0.25f);
	const Frustum f = boxFrustum();
	INFO("平面 bottom = planes[2]");
	CHECK(cullOne(fx, kIdentity, f).visibleClusterCount == 0);
	CHECK(cullOne(fx, kIdentity, planeDisabled(f, 2)).visibleClusterCount == 1);
}

TEST_CASE("外侧 top：球整体在 top 面外 → 剔除；摘掉 top 面 → 复活") {
	// 闭式：球心 (0,2,−1.5) r=0.25 ⟹ top d = −2+1 = −1 < −0.25；其余五面 ≥ 0.5。
	const ClusterFixture fx = oneCluster(glm::vec3{0.0f, 2.0f, -1.5f}, 0.25f);
	const Frustum f = boxFrustum();
	INFO("平面 top = planes[3]");
	CHECK(cullOne(fx, kIdentity, f).visibleClusterCount == 0);
	CHECK(cullOne(fx, kIdentity, planeDisabled(f, 3)).visibleClusterCount == 1);
}

TEST_CASE("外侧 near：球整体在 near 面**之前**（相机与近截面之间）→ 剔除；摘掉 near 面 → 复活") {
	// 闭式：球心 (0,0,−0.5) r=0.25 ⟹ near d = −(−0.5)−1 = −0.5 < −0.25；far d = 1.5、四侧面 = 1。
	// ⚠ 这条测的是「近截面之前」，与「相机背后」是两条不同用例（后者见下面相机视锥那组）。
	const ClusterFixture fx = oneCluster(glm::vec3{0.0f, 0.0f, -0.5f}, 0.25f);
	const Frustum f = boxFrustum();
	INFO("平面 near = planes[4]");
	CHECK(cullOne(fx, kIdentity, f).visibleClusterCount == 0);
	CHECK(cullOne(fx, kIdentity, planeDisabled(f, 4)).visibleClusterCount == 1);
}

TEST_CASE("外侧 far：球整体在 far 面之外 → 剔除；摘掉 far 面 → 复活") {
	// 闭式：球心 (0,0,−3) r=0.25 ⟹ far d = −3+2 = −1 < −0.25；near d = 3−1 = 2、四侧面 = 1。
	const ClusterFixture fx = oneCluster(glm::vec3{0.0f, 0.0f, -3.0f}, 0.25f);
	const Frustum f = boxFrustum();
	INFO("平面 far = planes[5]");
	CHECK(cullOne(fx, kIdentity, f).visibleClusterCount == 0);
	CHECK(cullOne(fx, kIdentity, planeDisabled(f, 5)).visibleClusterCount == 1);
}

// ─── 跨界（保守判据）与零半径 ─────────────────────────────────────────────────────

TEST_CASE("相切：球与 left 面恰好相切 → 可见（保守判据 d < −r 取不到等号；逐位精确不靠容差）") {
	// 闭式：球心 (−1.25,0,−1.5) r=0.25 ⟹ left d = −0.25，−r = −0.25。`−0.25 < −0.25` 为假 ⟹ 不剔。
	// 1.25 与 0.25 都是二进制精确值，且 left 法线是坐标轴 ⟹ 这条**没有浮点余量**也不会有噪声。
	const ClusterFixture fx = oneCluster(glm::vec3{-1.25f, 0.0f, -1.5f}, 0.25f);
	CHECK(cullOne(fx, kIdentity, boxFrustum()).visibleClusterCount == 1);
}

TEST_CASE("相交：球心在 left 面外但跨过它 → 可见（保守判据：跨界一律留）") {
	// 闭式：球心 (−1.1,0,−1.5) r=0.25 ⟹ left d = −0.1 > −0.25 ⟹ 不剔。
	const ClusterFixture fx = oneCluster(glm::vec3{-1.1f, 0.0f, -1.5f}, 0.25f);
	CHECK(cullOne(fx, kIdentity, boxFrustum()).visibleClusterCount == 1);
}

TEST_CASE("零半径球：点在面上 / 点在内 → 可见；点在外 → 剔除") {
	const Frustum f = boxFrustum();
	// 点在 left 面上：d = 0，`0 < −0.0` 假 ⟹ 可见。
	const ClusterFixture onFace = oneCluster(glm::vec3{-1.0f, 0.0f, -1.5f}, 0.0f);
	CHECK(cullOne(onFace, kIdentity, f).visibleClusterCount == 1);
	// 点在内：六面距离全 > 0。
	const ClusterFixture inside = oneCluster(glm::vec3{0.0f, 0.0f, -1.5f}, 0.0f);
	CHECK(cullOne(inside, kIdentity, f).visibleClusterCount == 1);
	// 点在外：left d = −0.5 < −0.0 ⟹ 剔。零半径把判据退化成「点在不在盒内」，仍要正确
	// （半径缩放写成任何「恒 ≥ 0 的加法项」都会让这条变绿→红可见）。
	const ClusterFixture outside = oneCluster(glm::vec3{-1.5f, 0.0f, -1.5f}, 0.0f);
	CHECK(cullOne(outside, kIdentity, f).visibleClusterCount == 0);
}

// ─── 相机背后（现行语义；最容易写反的一条）───────────────────────────────────────

TEST_CASE("相机背后：球心在相机之后且半径够不着 near 面 → 剔除（现行语义就是剔，不是保守保留）") {
	// 🔴 先读实现再写期望（`scene_context.cppm::extractFrustum`）：near 面 = clip 矩阵**第 2 行**
	// （z_clip ≥ 0，Vulkan clip z∈[0,1]；不是 [-1,1] 约定的 row3+row2），归一化后 = (0,0,−1,−near)
	// ⟹ 有符号距离 = −z − near。**相机背后的点 z > 0 ⟹ 距离恒负**，且 |距离| 随 z 线性增长。
	// 于是「在相机背后」**不是**「保守保留」，而是**被 near 面剔除**——除非半径大到跨过 near 面。
	// 闭式（探针实测值）：球心 (0,0,10) r=0.5 ⟹ near d = −11 < −0.5 ⟹ 剔。
	const ClusterFixture behind = oneCluster(glm::vec3{0.0f, 0.0f, 10.0f}, 0.5f);
	CHECK(cullOne(behind, kIdentity, cameraFrustum()).visibleClusterCount == 0);
}

TEST_CASE("相机背后 + 半径跨过 near 面 → 可见（判据里没有隐藏的「背后一律剔」分支）") {
	// 闭式（探针实测值）：球心 (0,0,10) r=20 ⟹ near d = −11 > −20；四侧面 d = −10/√2 ≈ −7.07 > −20；
	// far d = +110。六面全不满足 d < −r ⟹ 可见。与上一条**只差半径**，配对钉住：判据只看
	// 「球与半空间是否相交」，没有「在相机背后就额外加严/额外保留」的第二条规则。
	const ClusterFixture big = oneCluster(glm::vec3{0.0f, 0.0f, 10.0f}, 20.0f);
	CHECK(cullOne(big, kIdentity, cameraFrustum()).visibleClusterCount == 1);
}

// ─── 半径缩放（源单位 → 世界；产物球是源 glb 单位，见 format.h「坐标系」）─────────

TEST_CASE("半径缩放 scale=0.01：源单位看不见、世界单位看得见 → 可见（抓「忘了把球心变换到世界」）") {
	// 机位 = cameraFrustum()（可见深度 z∈[−100,−1]）。簇球源单位：心 (0,0,−500) r=10。
	// 正确（乘 scale=0.01）：世界心 (0,0,−5) 在深度带内、|x|=|y|=0 ⟹ 六面距离 = {3.54,3.54,3.54,
	// 3.54,4,95}，世界半径 0.1 ⟹ 可见。
	// 忘了变换：源心 (0,0,−500) ⟹ far d = −400（探针实测 −399.9995）< −10 ⟹ 被剔 ⟹ 本条红。
	const ClusterFixture fx{std::vector<ClusterSpec>{ClusterSpec{glm::vec3{0.0f, 0.0f, -500.0f}, 10.0f, 1}}};
	const glm::mat4 m = glm::scale(kIdentity, glm::vec3{0.01f, 0.01f, 0.01f});
	CHECK(cullOne(fx, m, cameraFrustum()).visibleClusterCount == 1);
}

TEST_CASE("半径缩放 scale=0.01：世界单位看不见、源半径看得见 → 剔除（抓「忘了缩放半径」）") {
	// 簇球源单位：心 (0,600,−500) r=10 ⟹ 世界心 (0,6,−5)、世界半径 **0.1**。
	// 闭式（探针实测值）：top 面（法线 (0,−0.7071,−0.7071)）d = (−6+5)/√2 ≈ −0.707 < −0.1 ⟹ 剔。
	// 忘了缩放半径（r 仍 = 10）：−0.707 < −10 为假 ⟹ 可见 ⟹ 本条红（探针实测该点 r=10 时 visible）。
	// 其余五面距离 = {3.54,3.54,7.78,4,95}，都不参与结论。
	const ClusterFixture fx{std::vector<ClusterSpec>{ClusterSpec{glm::vec3{0.0f, 600.0f, -500.0f}, 10.0f, 1}}};
	const glm::mat4 m = glm::scale(kIdentity, glm::vec3{0.01f, 0.01f, 0.01f});
	CHECK(cullOne(fx, m, cameraFrustum()).visibleClusterCount == 0);
}

TEST_CASE("半径缩放非均匀：上界取**最大轴**，不是第一个轴（均匀对照 + 三条非均匀臂 + 长度真实性）") {
	// 对象矩阵 = translate(0,1.5,−1.5) · scale(sx,sy,sz)，簇球心取**源单位原点**
	// ⟹ 世界心 = 平移量 (0,1.5,−1.5)，与缩放**无关**（把「球心变换」从本条结论里摘干净，只留
	// 半径一条）。盒六面距离 = {1,1,2.5,−0.5,0.5,0.5} ⟹ 只有 top 面可能剔，
	// 于是「可见 ⟺ 世界半径 ≥ 0.5 ⟺ max(三轴) × 1 ≥ 0.5」。
	const Frustum f = boxFrustum();
	const glm::vec3 kPos{0.0f, 1.5f, -1.5f};
	const ClusterFixture fx{std::vector<ClusterSpec>{ClusterSpec{glm::vec3{0.0f, 0.0f, 0.0f}, 1.0f, 1}}};
	const auto mat = [&kPos](glm::vec3 s) {
		return glm::translate(kIdentity, kPos) * glm::scale(kIdentity, s);
	};

	// 均匀 0.01：世界半径 0.01 < 0.5 ⟹ 剔。（「取第一个轴」的变异在这条上**不红**——三轴相同，
	// 所以它只是对照臂，主体是下面三条。）
	CHECK(cullOne(fx, mat(glm::vec3{0.01f, 0.01f, 0.01f}), f).visibleClusterCount == 0);
	// 最大轴 = y：半径 2.0 ≥ 0.5 ⟹ 可见。「取第一个轴（0.01）」在这条上变红。
	CHECK(cullOne(fx, mat(glm::vec3{0.01f, 2.0f, 0.01f}), f).visibleClusterCount == 1);
	// 最大轴 = x：同样可见 ⟹ 抓「固定取第二个轴」的写法。
	CHECK(cullOne(fx, mat(glm::vec3{2.0f, 0.01f, 0.01f}), f).visibleClusterCount == 1);
	// 最大轴 = z：同样可见 ⟹ 抓「固定取第三个轴」的写法（与上面两条合起来穷尽「固定某一轴」）。
	CHECK(cullOne(fx, mat(glm::vec3{0.01f, 0.01f, 2.0f}), f).visibleClusterCount == 1);
	// 上界**真的是轴长度**而不是「只要非均匀就算大」：sy=0.4 ⟹ 半径 0.4 < 0.5 ⟹ 剔。
	CHECK(cullOne(fx, mat(glm::vec3{0.01f, 0.4f, 0.01f}), f).visibleClusterCount == 0);
}

// ─── 对象矩阵含旋转（球心不在源原点）─────────────────────────────────────────────

TEST_CASE("含旋转的对象矩阵：球心变换用 `M * v`（列向量语义），不是 `v * M`（主 agent 变异 MC 补）") {
	// **为什么必须有这条**：本文件其余用例的对象矩阵是纯缩放或 `translate·scale`，且簇球心取源
	// 单位**原点**——`w=1` 的向量在两种乘法次序下都取矩阵第 3 列 ⟹ **两种次序结果相同**，
	// 「乘法次序写反」这个真实 bug 恒绿通过（主 agent 2026-10-12 实测：变异 MC 注入后 323 全绿）。
	// 真资产的簇球心**永不在原点**，且 modelMatrix 带旋转（scene.lua entities 的 rotation），
	// 所以这条不是假想形态。glm 下两者确实不同（探针：一般矩阵 `M*c`=(5.15,1.40,−2.90) vs
	// `c*M`=(−0.60,−0.35,−1.40)）。
	//
	// 期望值闭式推导（不抄被测码）：M = translate(0,0,−1.5) · rotZ(90°)，球心 c = (0.5,0,0) r=0。
	//   rotZ(90°)·c = (0.5·cos90 − 0·sin90, 0.5·sin90 + 0·cos90, 0) = (0, 0.5, 0)
	//   再平移 ⟹ 世界心 (0, 0.5, −1.5)
	//   盒（x∈±1, y∈±1, z∈[−2,−1]）六面距离 = {1, 1, 1.5, 0.5, 0.5, 0.5}，全 ≥ 0 ⟹ **可见**。
	// 写反成 `c * M` ⟹ 世界心 (0, −0.5, **0**)（z 分量丢了平移）⟹ near 面 d = −0 − 1 = −1 < −0
	// ⟹ **被剔** ⟹ 本条红（主 agent 实测：注入 MC 后本条与配对臂一起红）。
	const Frustum f = boxFrustum();
	const glm::mat4 m = glm::translate(kIdentity, glm::vec3{0.0f, 0.0f, -1.5f})
	                  * glm::rotate(kIdentity, glm::radians(90.0f), glm::vec3{0.0f, 0.0f, 1.0f});
	// 阳性方向：正确次序可见。
	const ClusterFixture rot{std::vector<ClusterSpec>{ClusterSpec{glm::vec3{0.5f, 0.0f, 0.0f}, 0.0f, 1}}};
	CHECK(cullOne(rot, m, f).visibleClusterCount == 1);
	// 反向配对臂（同矩阵、球心取 (−0.5,0,0) ⟹ 世界心 (0,−0.5,−1.5) 仍可见）：
	// 抓「把旋转整个丢掉」——丢旋转则两点分别落到 x=±0.5（仍可见），故旋转丢失由**下一条**的
	// 深度分量负责，本条只负责乘法次序。
	const ClusterFixture rot2{std::vector<ClusterSpec>{ClusterSpec{glm::vec3{-0.5f, 0.0f, 0.0f}, 0.0f, 1}}};
	CHECK(cullOne(rot2, m, f).visibleClusterCount == 1);
}

TEST_CASE("含旋转的对象矩阵：半径缩放取轴长度（旋转是正交的、不改列长），非均匀缩放 + 旋转复合") {
	// M = translate(0,1.5,−1.5) · rotZ(90°) · scale(0.01, 2.0, 0.01)：列长 = (0.01, 2.0, 0.01)
	// （旋转正交 ⟹ **不改变列向量长度**）⟹ 世界半径上界 = max(列长) × 源半径 = 2.0 × r。
	// 球心取源单位原点 ⟹ 世界心 = 平移量 (0, 1.5, −1.5)，盒六面距离 = {1, 1, 2.5, **−0.5**, 0.5, 0.5}
	// ⟹ 只有 top 面可能剔，且 **剔除 ⟺ −0.5 < −r_世界 ⟺ r_世界 < 0.5**（球心在盒外 0.5，
	// 由半径决定去留——球心若在盒内则任何半径都可见，那条对照臂会恒绿，踩过）。
	// 源半径 0.5：正确（2.0×0.5 = 1.0 ≥ 0.5）可见；误取第一个轴（0.01×0.5 = 0.005 < 0.5）⟹ 剔 ⟹ 红。
	// 与「非均匀取最大轴」那条的区别：那条矩阵**无旋转**（列长 = 缩放本身，平凡成立），本条加了
	// 旋转，抓的是「用 scale 分量而不是列长」与「把旋转也乘进半径」两类写法。
	const Frustum f = boxFrustum();
	const glm::mat4 m = glm::translate(kIdentity, glm::vec3{0.0f, 1.5f, -1.5f})
	                  * glm::rotate(kIdentity, glm::radians(90.0f), glm::vec3{0.0f, 0.0f, 1.0f})
	                  * glm::scale(kIdentity, glm::vec3{0.01f, 2.0f, 0.01f});
	const ClusterFixture fx{std::vector<ClusterSpec>{ClusterSpec{glm::vec3{0.0f, 0.0f, 0.0f}, 0.5f, 1}}};
	CHECK(cullOne(fx, m, f).visibleClusterCount == 1);
	// 对照臂：源半径 0.002 ⟹ 世界 0.004 < 0.5 ⟹ 剔（钉住「不是恒可见」，防上界被写成无穷大）。
	const ClusterFixture tiny{std::vector<ClusterSpec>{ClusterSpec{glm::vec3{0.0f, 0.0f, 0.0f}, 0.002f, 1}}};
	CHECK(cullOne(tiny, m, f).visibleClusterCount == 0);
}

// ─── 统计锚与空集 ─────────────────────────────────────────────────────────────────

TEST_CASE("可见三角合计 = Σ 可见簇 triCount（手算闭式 129，不是数返回条数）") {
	// 四簇（盒视锥、单位对象矩阵），逐簇闭式距离：
	//  c0 心 (0,0,−1.5)     r=0.25 tri=1   ⟹ {1,1,1,1,0.5,0.5}          ⟹ 可见
	//  c1 心 (−2,0,−1.5)    r=0.25 tri=7   ⟹ left d = −1                ⟹ 剔
	//  c2 心 (0,2,−1.5)     r=0.25 tri=3   ⟹ top d = −1                 ⟹ 剔
	//  c3 心 (0.5,−0.5,−1.5) r=0.25 tri=128 ⟹ {1.5,0.5,0.5,1.5,0.5,0.5} ⟹ 可见
	// 闭式期望：可见 = {0,3}，合计 = 1 + 128 = **129**（≠ 条数 2 ⟹ 排除「合计 = 条数」的同义反复）。
	const ClusterFixture fx{std::vector<ClusterSpec>{
	    ClusterSpec{glm::vec3{0.0f, 0.0f, -1.5f}, 0.25f, 1},
	    ClusterSpec{glm::vec3{-2.0f, 0.0f, -1.5f}, 0.25f, 7},
	    ClusterSpec{glm::vec3{0.0f, 2.0f, -1.5f}, 0.25f, 3},
	    ClusterSpec{glm::vec3{0.5f, -0.5f, -1.5f}, 0.25f, 128}}};
	const ClusterCullResult r = cullOne(fx, kIdentity, boxFrustum());
	CHECK(r.visibleClusters == std::vector<uint32_t>{0, 3});
	CHECK(r.visibleClusterCount == 2);
	CHECK(r.visibleTriangles == 129);
}

TEST_CASE("空簇集 → 返回空、三角合计 0（前提守卫先立住，别写恒真断言）") {
	const ClusterFixture fx{std::vector<ClusterSpec>{}};
	// 前提守卫：夹具真的是空的。夹具哪天写错成非空，下面三条断言全部失去意义 ⟹ 先 REQUIRE。
	REQUIRE(fx.storage.empty());
	REQUIRE(fx.asset.clusters.empty());
	REQUIRE(fx.asset.header.clusterCount == 0);

	const ClusterCullResult r = cullOne(fx, kIdentity, boxFrustum());
	CHECK(r.visibleClusters.empty());
	CHECK(r.visibleClusterCount == 0);
	CHECK(r.visibleTriangles == 0);
}

TEST_CASE("不碰层级：nodes 全坏也不影响结果（参考实现必须与 GPU 版算法不同）") {
	// 两个夹具的**簇集完全相同**（一被剔一可见），差别只在 nodes：一个空、一个填 4 个「球心在
	// 原点、半径 0」的节点（原点在本盒之外：near 面距离 = −1）。若实现改走层级遍历（★1-a 的
	// GPU 形态），这些装不下任何东西的节点会让可见簇直接消失。
	// 闭式期望：可见 = {1}、三角合计 = 9，与 nodes 完全无关。
	const std::vector<ClusterSpec> specs{
	    ClusterSpec{glm::vec3{-2.0f, 0.0f, -1.5f}, 0.25f, 5},
	    ClusterSpec{glm::vec3{0.0f, 0.0f, -1.5f}, 0.25f, 9}};
	// 坏节点存储先于夹具声明（span 指向它；析构顺序反过来才会悬空）。
	std::vector<MeshletDiskNode> badNodes(4);
	ClusterFixture noNodes{specs};
	ClusterFixture withBadNodes{specs};
	withBadNodes.asset.nodes = badNodes;  // 值初始化 = 球心 (0,0,0)、半径 0、group 0

	REQUIRE(noNodes.asset.nodes.empty());  // 前提守卫
	REQUIRE(withBadNodes.asset.nodes.size() == 4);
	// 前提守卫：这些节点**确实**是「谁都装不下」的坏节点（球心在原点、半径 0 ⟹ 盒外）。
	for (const MeshletDiskNode& n : withBadNodes.asset.nodes) {
		CHECK(n.radius == 0.0f);
		CHECK(n.center[0] == 0.0f);
	}

	const Frustum f = boxFrustum();
	const ClusterCullResult a = cullClustersLinear(noNodes.asset, kIdentity, f);
	const ClusterCullResult b = cullClustersLinear(withBadNodes.asset, kIdentity, f);
	CHECK(a.visibleClusters == std::vector<uint32_t>{1});
	CHECK(b.visibleClusters == std::vector<uint32_t>{1});
	CHECK(a.visibleTriangles == 9);
	CHECK(b.visibleTriangles == 9);
}

// ─── 视锥约定对账（盒视锥的 near/far 方向必须与真相机一致，否则六面用例整体失真）──

TEST_CASE("视锥约定：盒视锥的 near/far 朝内方向与 extractFrustum 一致（防盒视锥自己漂）") {
	// 本文件的六面归因全靠盒视锥。这条把「盒的 near/far 内含侧」与真相机视锥对齐。
	const Frustum cam = cameraFrustum();
	const Frustum box = boxFrustum();
	// 探针实测：cam near = (0,0,−1,−1)、far = (0,0,1,100.0005)；盒取同向。
	CHECK(cam.planes[4].z < 0.0f);   // near 法线朝内 = 视向 −Z
	CHECK(cam.planes[5].z > 0.0f);   // far 法线朝内 = 指向相机 +Z
	CHECK(box.planes[4].z < 0.0f);
	CHECK(box.planes[5].z > 0.0f);
	// 且两者对「near 之前」与「far 之外」的零半径点判定一致（都不可见）。
	const ClusterFixture beforeNear = oneCluster(glm::vec3{0.0f, 0.0f, -0.5f}, 0.0f);
	CHECK(cullOne(beforeNear, kIdentity, cam).visibleClusterCount == 0);
	CHECK(cullOne(beforeNear, kIdentity, box).visibleClusterCount == 0);
	const ClusterFixture beyondFar = oneCluster(glm::vec3{0.0f, 0.0f, -200.0f}, 0.0f);
	CHECK(cullOne(beyondFar, kIdentity, cam).visibleClusterCount == 0);
	CHECK(cullOne(beyondFar, kIdentity, box).visibleClusterCount == 0);
}

}  // namespace RGL
