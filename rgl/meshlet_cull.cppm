module;
#include <glm/glm.hpp>
export module meshlet_cull;

import rlstd;
import meshlet_asset;
import scene_context;

/**
 * meshlet_cull —— 簇产物的 **CPU 参考簇剔除器**（簇化轴 ★0.5-a，rlbase，纯函数无设备）
 *
 * ## 它存在的唯一理由：给 ★1-a 的 GPU 版当「第二实现」对账伙伴
 * GPU 版做层级遍历 + 逐簇剔除 + 压紧。它「剔多了」（该画的没画）在图像上的表现是「屏幕少了一块」，
 * 与「材质本来就黑」「几何本来就看不见」**不可区分**——没有对账伙伴就无法归因。本模块就是那个伙伴。
 *
 * 🔴 **算法必须与 GPU 版不同**：这里**线性扫描全部簇**，一个层级节点都不碰。理由：被测对象正是
 * 「层级遍历有没有漏掉簇」，参考实现若也走层级，等于**用同一个 bug 验自己**（层级根节点写错、
 * childOffset 偏移一格、某级森林漏根——三种错法在「两边都走层级」的对账里全部隐身）。
 * 代价是 O(clusterCount) 的暴力（bistro = 61,518 簇），本模块只跑在离线/仪器路径上，不进帧循环。
 *
 * ## 两条钉死的约定（★1-a 的 GPU 版**继承**它们，不许各写各的）
 * 1. **视锥约定**：平面提取与球测试**一律复用** `scene_context` 的 `extractFrustum` /
 *    `sphereInFrustum`（法线朝内、按法线长度归一化、clip z∈[0,1]；保守判据 = 球心到任一平面
 *    距离 < −r 才剔，跨界与内含一律判可见）。本模块**不写**第二份平面提取或球测试——
 *    两份实现一旦漂符号约定，对账就变成「两个都错但互相印证」。同型调用点 =
 *    `render_list_impl.cppm` 的 primitive 粒度 CPU 剔除（node-local 球经 modelMatrix 到世界后测）。
 * 2. **坐标系**：产物里的簇包围球是**源 glb 单位**（未乘 `scene.lua` 的 scale=0.01、未应用 node
 *    transform，见 `tools/meshlet_pack/format.h` 头注释「坐标系」段）。世界换算归消费侧，
 *    即本函数的 `objectMatrix` 入参（= 现行路径的 `spin.matrix * tf.world` 同位）。
 *
 * ## 为什么剔除必须保守（半径宁可大不可小）
 * 误剔 = 几何**凭空消失**，画面少一块且没有任何错误信号，事后只能从头排查；多画 = 只花 GPU 时间，
 * 图像逐位不变。两者不对等 ⟹ **判据宁可漏剔不可误剔**。落到实现上是一处具体选择：
 * 非均匀缩放下球半径的缩放系数取**三轴尺度的上界** `max(length(m[0..2]))` 而不是某个轴、
 * 也不是平均——取小的那个轴会让斜向拉伸后的球**小于**真实球的像，边界簇就被误剔。
 */

namespace RGL {

/**
 * 线性簇剔除的结果统计（★0.5-a）。
 *
 * 三个量都是给 ★0.5/★1-a 的**统计锚**用的，不是给渲染路径用的中间产物：
 * `visibleTriangles` 对 `docs/mdi_plan.md` §7.3 离线模拟的「簇粒度剔除 = 全场景三角的 1.65%」，
 * `visibleClusters` 给逐簇可见性归因（被剔的每块都该剔）。
 */
export struct ClusterCullResult {
	/** 可见簇下标（**升序 = 簇表下标序**；线性扫描天然有序，对账时可直接逐位比）。 */
	std::vector<uint32_t> visibleClusters;
	/** 可见簇数（恒等于 `visibleClusters.size()`；单列是为了统计读数不必再取一次 size）。 */
	uint32_t visibleClusterCount = 0;
	/**
	 * 可见三角合计 = Σ 可见簇的 `triCount`（**不是**可见簇条数——一簇最多 128 三角）。
	 * 用 uint64：单物体簇数可达 10⁵ 量级、每簇 ≤ 128，积会越过 uint32 的舒适区。
	 */
	uint64_t visibleTriangles = 0;
};

/**
 * 线性扫描全部簇做视锥剔除（**参考实现**：最笨的算法，故意与 GPU 版不同）。
 *
 * 逐簇：源单位包围球 → `objectMatrix` 变换到世界 → 交 `sphereInFrustum` 判可见。
 * 不读 `asset.nodes`（层级）、不做遮挡、不做 LOD 选级——那些是 ★1-a/★2 的事，
 * 一旦混进来，本函数就不再是「独立第二实现」了。
 *
 * @param asset 经校验的簇产物（只读其 `clusters` 视图；空簇集 → 返回空结果、三角合计 0）
 * @param objectMatrix 源 glb 单位 → 世界（= 现行路径的 modelMatrix；产物球是源单位，见文件头约定 2）
 * @param frustum 视锥（`extractFrustum(proj·view)` 产物，法线朝内且已归一化）
 * @return 可见簇下标（升序）+ 可见簇数 + 可见三角合计
 */
export inline ClusterCullResult cullClustersLinear(const MeshletAsset& asset,
	const glm::mat4& objectMatrix, const Frustum& frustum) {
	// 半径缩放 = 三轴尺度上界（列向量的长度即该轴的缩放）。**必须取 max**：非均匀缩放下球会变成
	// 椭球，其外接球半径 ≥ 任一单轴尺度 × 原半径；取第一个轴（或 min/平均）会**低估**半径，
	// 低估直接违反上面的保守性红线——误剔 = 几何凭空消失，比多画贵得多。
	// 与 render_list_impl 的 primitive 粒度剔除同一算式（同型 = 对账时少一个变量）。
	const float scale = std::max({glm::length(glm::vec3(objectMatrix[0])),
	    glm::length(glm::vec3(objectMatrix[1])), glm::length(glm::vec3(objectMatrix[2]))});

	ClusterCullResult out;
	out.visibleClusters.reserve(asset.clusters.size());
	for (size_t i = 0; i < asset.clusters.size(); ++i) {
		const MeshletDiskCluster& c = asset.clusters[i];
		const glm::vec3 center(c.center[0], c.center[1], c.center[2]);
		const glm::vec3 worldCenter = glm::vec3(objectMatrix * glm::vec4(center, 1.0f));
		// 保守判据住在 sphereInFrustum 里（< −r 才剔）；本层不加任何「更聪明」的提前剔除。
		if (!sphereInFrustum(frustum, worldCenter, c.radius * scale))
			continue;
		out.visibleClusters.push_back(static_cast<uint32_t>(i));
		out.visibleTriangles += c.triCount;
	}
	out.visibleClusterCount = static_cast<uint32_t>(out.visibleClusters.size());
	return out;
}

}  // namespace RGL
