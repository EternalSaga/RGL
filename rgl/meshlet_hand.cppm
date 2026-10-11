export module meshlet_hand;

import rlstd;
import meshlet_asset;

/**
 * meshlet_hand —— UV 手性的 **CPU 参考实现 + 簇→三角寻址遍历**（簇化轴 ★0.5-c，rlbase，纯 CPU 无设备）
 *
 * ## 它存在的唯一理由：给 ★1-a1ii 的新 mesh shader 当「第二实现」对账伙伴
 * 新 mesh shader 要做两件今天没有任何代码做过的事：**沿簇寻址三角形**与**逐三角重算 UV 手性**
 * （产物顶点池的 `tangent.w` 存的是 +1 占位，而 bistro 逐三角 82.00% 为负，`normalmap.slang`
 * 消费 `worldT.w` ⟹ 不重算 = 八成面的法线翻错，见 `docs/mdi_plan.md` §10.3 硬前置 1）。
 * 两件事在 shader 里错了都**不报错、只是图像静默错**。本模块把它们各写一份 CPU 版：GPU 版必须
 * 与它**逐三角一致**（`MeshletHandResult::perTriangle` 就是为这条对账留的出口），而它自己的
 * 判据 = 闭式解 + loader 同式公式。形状照 `meshlet_cull`（同为簇产物的纯 CPU 对账伙伴）。
 *
 * ## 寻址口径（= `tools/meshlet_pack/format.h`「段内寻址」的读侧重述，最容易写错的一条）
 * 簇 c 的第 k 个三角形：
 *   局部索引 = `tris[c.triOffset + 3k + {0,1,2}]`（u8；🔴 **triOffset 是 Tris 段内字节偏移**，
 *              元素恰 1B 所以字节偏移 == 元素下标——但口径必须按字节记，写成 `×3` 就漂）
 *   全局顶点 = `remap[c.remapOffset + 局部索引]`（u32；🔴 **remapOffset 是 Remap 段内元素偏移**，
 *              不是字节偏移）
 *   UV       = `vertexPool[全局顶点 × 48 + 24]` 起的两个 f32（ModelVertex 契约，
 *              `rgl/shaders/model_schema.lua` 单一真相源；`generated/model_types_generated.cppm`
 *              的 `offsetof(ModelVertex, uv) == 24` 与 `meshlet_asset` 的 `kMeshletVertexStride
 *              == 48` 把这两个数钉在编译期，本模块常量与它们同数由 impl 的 static_assert 对账）
 *
 * ## 公式权威 = loader `gltf_loader_impl.cppm::uvTriangleHandedness`（逐字符照抄语义）
 * `s1 = uv[i1] − uv[i0]`、`s2 = uv[i2] − uv[i0]`（逐分量 float 减法）；
 * `det = s1x·s2y − s2x·s1y`；**`det < 0 → −1`，否则 `+1`**——含 `det == 0` 与 NaN 走 `+1`
 * 的现行语义（NaN 的比较恒假 ⟹ 落 `+1` 分支；写成 `!(det >= 0)` 形态就把它翻成 `−1` 了，
 * `tests/test_meshlet_hand.cppm` 有 NaN 用例专门钉住）。
 * 🔴 **口径红线**（照抄 loader 注释，别混）：**82.00%（2,322,319 / 2,832,120）是逐三角口径**；
 * 「prim 内多数三角为负」是另一口径 = 93.5%（1487/1591）。本模块与 packer `--stats` 都是逐三角。
 * 🔴 **不 import loader 那个 static 函数**：它住在 `gltf_loader:impl`、也不该被 import；
 * 一致性由「同式 + 单测在随机 UV 三元组上对拍**公式**（独立手算）」担保，对拍的不是那个符号。
 *
 * ## 逐三角输出 = 将来 GPU 对账的接口（别只给计数把这条路堵死）
 * `perTriangle` 按**遍历序**（簇下标升序、簇内三角升序、被过滤的簇不产条目）逐三角给 ±1。
 * GPU 版对账 = 同区间同 filter 跑一遍、逐元素比。聚合计数（`total`/`negative`）是统计锚用的
 * 第二形态，不是替代品；只要统计时传 `wantPerTriangle = false`，不白分配真产物量级的 2.8MB。
 *
 * ## 🔴 越界纪律（前置写死 + 断言形态）
 * **前置**：asset 来自 `parseMeshletAsset`。读取侧已校验（→ `meshlet_asset` 校验清单）：
 * `triOffset + triCount×3 ≤ tris.size()`、`remapOffset + vertCount ≤ remap.size()`、
 * `triCount/vertCount ≤ 128`、`remap` 值 `< vertexCount`。本模块**不重复校验**这四条——
 * 每次启动多两遍全表扫描，而它们防的对象（坏产物）根本走不到这里。
 * **断言形态 = 逐三角 `局部索引 < vertCount`（恒编译，返回可归因错误）**：这是读取侧**不查**、
 * 只有打包器 `--verify ②`（同判据 `local >= vertCount` 即红）保证的一条；它坏时
 * `remap[remapOffset + local]` 会读出**本簇区间外**的 remap（段内越界读，静默错）。
 * 由读取侧的 `vertCount ≤ 128` 推得「局部索引 ≥ 128 不可能」——8bit 可解性由此覆盖。
 * 为什么不是 `assert` 宏：项目样式规则 B2 禁 GMF `#include <cassert>`（宏无法经 import 获得），
 * 而 doctest 观测不到 abort ⟹ 断言形态取「返回可归因错误」的等价物（与 `meshlet_asset` /
 * `meshlet_ranges` 的失败形态同纪律）：恒编译、Release 也不关、错误串点名簇号/三角号/三个局部值。
 * `base + count` 越出簇表同样返回错误——那是本模块自己的 API 前置，不归读取侧管。
 *
 * ## L0 判据 = `cluster.refined == -1`（与 `meshlet_ranges` 同口径）
 * `kL0Only` 档下非 L0 簇**一条字节都不读**。这不只是省遍历：「过滤形态不读被过滤数据」必须
 * 可证——单测用「非 L0 簇填垃圾字节（局部索引 255），`kL0Only` 照样绿而 `kAll` 报错」钉住它。
 *
 * ## 真产物统计锚（★0.5-c 判据 4；常驻证据 = 引擎启动 INFO，门 `meshlet.enable` 开才有）
 * 装配层（`main:meshlets`）对全表跑 `kL0Only` 并 INFO 报 `total / negative / l0TriangleCount`：
 * bistro 上 `total == 2,832,120 == header.l0TriangleCount`（遍历恰好覆盖 L0、不重不漏）且
 * `negative == 2,322,319`（packer 在**源 glb** 上独立量的逐三角读数）。两个数**同时**对上 =
 * 「寻址 + UV 忠实 + 公式」三者同时对了——合成用例只验逻辑，验不出真产物形态（同
 * `meshlet_ranges` 的三个统计锚先例）。
 */

namespace RGL {

/** 顶点池内 `ModelVertex.uv` 的字节偏移（model_schema.lua 契约；impl 与 generated 的 offsetof static_assert 对账）。 */
export inline constexpr uint32_t kMeshletHandUvByteOffset = 24;

/** 簇顶点数上限（8bit 局部索引契约 `vertCount ≤ 128`；「局部索引 ≥ 128 不可能」的推导上界）。 */
export inline constexpr uint32_t kMeshletHandMaxClusterVertices = 128;

/** 逐三角手性输出：正手（`det >= 0`，含 `det == 0` 与 NaN——loader 现行语义，勿「修正」）。 */
export inline constexpr int8_t kMeshletHandPositive = 1;

/** 逐三角手性输出：反手（`det < 0`）。 */
export inline constexpr int8_t kMeshletHandNegative = -1;

/** 遍历的簇筛选形态。 */
export enum class MeshletHandFilter : uint32_t {
	/** 区间内全部簇（全部 DAG 级）。 */
	kAll = 0,
	/** 只读 `refined == -1` 的簇（L0，与 `meshlet_ranges` 同口径）；被滤掉的簇一条字节都不读。 */
	kL0Only = 1,
};

/**
 * 一次遍历的结果：聚合计数（统计锚形态）+ 逐三角序列（GPU 对账形态，可选）。
 */
export struct MeshletHandResult {
	/** 遍历到的三角形总数（被过滤的簇不计；`triCount == 0` 的簇天然不贡献）。 */
	uint64_t total = 0;
	/** `det < 0` 的三角数（`det == 0` 与 NaN 计入正手，与 loader 一致）。 */
	uint64_t negative = 0;
	/**
	 * 逐三角手性（+1/−1），**遍历序** = 簇下标升序、簇内三角升序。
	 * 存在意义 = ★1-a1ii GPU 版的逐三角对账接口（同区间同 filter 逐元素比）；
	 * `wantPerTriangle = false` 时恒空。
	 */
	std::vector<int8_t> perTriangle;
};

/**
 * 遍历簇区间 `[base, base+count)` 的三角形，逐三角算 UV 手性并聚合。
 *
 * 寻址口径 / 公式 / 越界纪律 → 文件头三节。只在装配期与仪器路径上跑，不进帧循环
 * （真产物 L0 全表 = 2.83M 三角，毫秒级）。
 *
 * @param asset 经 `parseMeshletAsset` 校验的只读视图（前置 → 文件头「越界纪律」）
 * @param base 簇区间起点（全局簇下标）
 * @param count 区间长度（0 = 空区间 → 全 0 结果；`base + count` 允许恰等于簇表条数）
 * @param filter `kL0Only` = 只读 `refined == -1` 的簇（被滤簇不读任何字节）
 * @param wantPerTriangle true = 填充 `perTriangle`（GPU 对账用）
 * @return 聚合计数（+ 可选逐三角序列）；区间越出簇表 / 局部索引坏 → 以 `meshlet hand: ` 开头的
 *         可归因错误串（两种失败形态错误串不同）
 */
export [[nodiscard]] std::expected<MeshletHandResult, std::string> traverseClusterHandedness(
	const MeshletAsset& asset, uint32_t base, uint32_t count, MeshletHandFilter filter,
	bool wantPerTriangle);

}  // namespace RGL
