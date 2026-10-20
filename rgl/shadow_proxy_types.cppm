export module shadow_proxy_types;

import rlstd;
import meshlet_asset;   // MeshletAsset（MustL0 段视图，v2）
import meshlet_ranges;  // MeshletRangeScan（前缀性与 l0Count 的对照面）

/**
 * shadow_proxy_types —— 阴影投射代理层的**钉段表**：从簇产物的 MustL0 段扫出
 * per-objectSlot 的「这段簇必须画 L0」条目（shadow proxy 批次 N10 的 P2，rlbase，纯 CPU 无设备）。
 *
 * ## 它解决的那一件事（docs/shadow_proxy_plan.md §0 / §7.2）
 * 阴影段按 `shadow_lod` 选级时，一个 primitive（= 一个材质批次）里所有部件**共享同一张简化层级、
 * 在同一级一起塌**。贴在更大主体上的细附属结构（花箱 / 牛腿 / 窗棂）被并进邻近大簇后不再单独
 * 投影。打包期（P1）判出「死亡级 == 1」的那批分量（只有级 0 还留着它们），把它们所属的 L0 group
 * **排到每个 prim depth==0 段的最前面**并在产物尾部记一条簇区间；本模块把那张表读成运行期能直接
 * 用的形态：一个 prim 至多两条 draw —— 钉段选级恒 0，其余照常走 D。
 *
 * ## 为什么本批（P2）**零消费者**
 * 接消费点是 P3（第一次变画面的一批）。P2 只做「读段 → 建表 → 打日志」，门开态必须与出厂态
 * 逐位相同 —— 那张截图就是「P2 真的没接画面」的自证。表先立起来，P3 只加消费点不改表。
 *
 * ## 🔴 二值形态只救得到「死亡级 == 1」那批（覆盖面，别读成「保住全部 ≥1m 部件」）
 * 对角线 ≥1m 的 10,203 个受保护分量里，恒存活 3,531 / **必须 L0 只有 196** / 死在级 2~11 共
 * 6,476（`docs/还债实测数据.md` §4.8 数 5，2026-10-20 改准）。两条 draw 只表达「钉 0」，那 6,476
 * 个要「钉 死亡级−1」才救得到（= 逐分量选级，§5 已按代价账否掉）。用户 2026-10-20 拍板**按二值
 * 形态继续**，理由 = 通用性（"这从来不是个 bistro 专用渲染器"），不是理由为收益更大。
 *
 * ## 五条产物结构不变量 = 建表的断言，同时是「打包侧重排漂了」的探测器
 * 每条一个**两两不同**的错误串、前缀 `shadow proxy: invariant N`（形态照 `meshlet_lod` 五条）。
 * 因为算在启动期而不是离线，这些从「审一次产物」升级成「每次启动都验」（计划 §3.6 R5 的理由）。
 *
 * | # | 不变量 | 不成立时意味着 |
 * |---|---|---|
 * | 1 | `clusterOffset == ranges.perPrim[prim].base`（钉段 = prim 簇区间的**前缀**）| 打包侧 P1 的重排漂了 / 段序口径变了 ⟹ 运行期取不出「前一段恒 0」的两条 draw |
 * | 2 | `clusterCount <= ranges.perPrim[prim].l0Count`（钉段不超出 L0 段）| 钉到了粗级簇上 = 「必须 L0」判据与簇序分家 |
 * | 3 | 条目按 prim **严格升序**（无重复）| 同一 prim 两条 = 后一条覆盖前一条，静默少画一段 |
 * | 4 | `prim < primCount` | 产物自相矛盾（MustL0 段引用了不存在的 prim）|
 * | 5 | `clusterOffset + clusterCount <= clusterCount(全表)` | ranges 与 asset 不是同一份 / 产物尾部被截（越界读簇包围体）|
 *
 * ## 为什么在 rlbase（R7 的判据 = 归属即判据）
 * 「这段逻辑需不需要一个 VkDevice 才能跑」：不需要（只读 clusters / mustL0 / perPrim 三个 span）
 * ⟹ rlbase，`just test` 毫秒级可钉；`rules_layers` 扫 import 边，逆向依赖当场链接失败。
 * 🔴 建表是**纯函数**：入参只有产物只读视图 + 同一份 asset 的区间扫描 + 槽位反查表，**不碰设备、
 * 不碰单例、不读配置**（这条边界是 §3.3「将来加烘焙」的全部前提：加烘焙 = 把它的输出序列化，
 * 表结构与判据一行不改）。形状照 `meshlet_lod`（同为簇产物的纯 CPU 消费者，纯扫描不携带布局身份）。
 */

namespace RGL {

/**
 * 阴影代理的**钉段表**（per-objectSlot，registry ctx，装配层启动期建一次）。
 *
 * ## 为什么「存在即生效」（与 `MeshletShadowLodTable` / `ClusterRangeTable` 同构）
 * 「生效」只判定一次（门开 + 产物有 MustL0 段 + 五条不变量全过 ⟹ emplace；任一步不过 = 不
 * emplace 并出声），消费者（P3 的 `collectShadowProxies`）只判 ctx 里有没有这张表 ——
 * **门关 = ctx 无条目 = 零 draw 变化 = 逐位不变的构造性保证**，不靠「读了但结果一样」。
 *
 * ## `count == 0` 是合法的「没有钉段」，不是错误
 * 绝大多数 objectSlot 如此（bistro 真产物：1591 个 prim 里只有 13 个有钉段），而「场景里混有
 * 无产物资产 / v1 产物 / 门槛关着打包的产物」也是合法形态 —— 真把它当错误会在这些合法输入上
 * 误杀整张表。
 */
export struct ShadowProxyTable {
	/** 一个 objectSlot 的钉段条目（P3 的「第一条 draw」用它定区间，选级恒 0）。 */
	struct Entry {
		/**
		 * 🔴 **相对 `ObjectClusterRange::base` 的偏移**（与 `MeshletShadowLodTable::Entry::start`
		 * 同口径，消费点不必再减一次 base）。P1 的重排保证钉段 = prim 簇区间的前缀 ⟹ 本字段
		 * **恒 0**（invariant 1 机器钉住这件事）；字段保留是为了让 P3 的消费点**免假设**：
		 * 将来段内排序口径变了，改的是建表，不是消费点。
		 */
		uint32_t start = 0;
		/** 被钉的簇数（0 = 该槽无钉段 = 常态；非 0 = P3 要多发一条选级恒 0 的 draw）。 */
		uint32_t count = 0;
		/**
		 * 被钉簇段的包围球半径（**产物局部单位**，未乘世界 scale —— 与 format.h 坐标系一节同源，
		 * 换算归消费方）。闭式取法（以段内**首簇球心**为球心参考，保守并）：
		 *
		 *     radius = max_i ( |center_i − center_0| + radius_i )
		 *
		 * 即「球心取 center_0，半径取各簇球的最远外包」—— 它不是最小外接球，但**恒包住整段**
		 * （三角不等式：段内任一点到 center_0 的距离 ≤ 它到自己簇心的距离 + 该簇半径）。
		 * texel 门只吃这个半径，所以保守方向（偏大）只会**多钉**不会漏钉。
		 */
		float radius = 0.0f;
	};
	/** 按 objectSlot 索引（下标 = `MeshRef.objectSlot` = shader objectIndex），容量 = 实例矩阵槽数。 */
	std::vector<Entry> bySlot;
};

/** `primOfSlot` 里「这个 objectSlot 不是本产物的图元」的哨兵（该槽落全 0 条目，合法常态）。 */
export inline constexpr uint32_t kShadowProxyNoPrim = 0xFFFFFFFFu;

/**
 * texel 门：这个钉段在「本级纹素 = texel（世界米）」下值不值得多发一条 draw。
 *
 *     pinActive ⟺ count > 0 && 2·radius·worldScale ≥ K·texel，K = 1
 *
 * 判据长在**尺寸**上而不是三角数上（计划 §7.3 教训 1：三角数门槛给 1m 目标算出 1.34×，尺寸门槛
 * 给 2.21×，差在「1 米大但只有 200 三角」的薄部件——阴影可见性的判据是长度，不是复杂度）。
 * 直径（2·radius）而不是半径：一个遮挡体在阴影图里占不占得下一枚纹素，看的是它的**跨度**。
 *
 * 🔴 **K = 1 写死，不加配置旋钮**——能被调到绿的参数不是判据（同 skill rgl-test-discipline）。
 * 要改 K 得先来一次拍板（它直接决定「多小的部件值得为它多付一条 draw」，与 R4 的 draws 账耦合）。
 *
 * 🔴 **R2 的机器形态就在这条签名上**：入参只有 (条目, 世界换算, 本级纹素)，**没有 far、没有相机、
 * 没有时间**。选级决策只依赖量化档导出的一枚离散量（计划 §3.2），所以「档内恒定」是类型层面的
 * 事实而不是运行时约定；`tests/test_shadow_proxy_types.cppm` 用一条**函数指针声明**把入参面钉成
 * 逐字相同的类型（加参数 / 改类型 / 加带默认值的第四参数全部编不过），谁给它塞第四枚量就红。
 *
 * @param entry 钉段条目（`count == 0` 恒 false，不必在外面先判）
 * @param worldScale 产物局部单位 → 世界米的换算（scene.lua 的 scale，bistro = 0.01）
 * @param texel 本级纹素尺寸（世界米 = 2·r_c / 块边长，与 `fillShadowData` 的 shadowTexelSizes 同式）
 */
export inline bool pinActive(const ShadowProxyTable::Entry& entry, float worldScale, float texel) {
	constexpr float kPinMinTexels = 1.0f;  ///< K（🔴 写死；改它要拍板，见函数头）
	return entry.count > 0 && 2.0f * entry.radius * worldScale >= kPinMinTexels * texel;
}

/**
 * 扫出钉段表并校验五条产物结构不变量（`meshlet_lod::scanMeshletLod` 的同形）。
 *
 * 一趟线性扫 MustL0 段（bistro 真产物 = 13 条）+ 对被钉的 prim 扫它段内的簇求外包半径
 * （Σ 被钉簇数 = 18 group / 实测百级，微秒级）；只在装配期跑一次，不进帧循环。
 *
 * @param asset 经 `parseMeshletAsset` 校验过的只读视图（MustL0 段可以**为空** = v1 产物或
 *              门槛关着打包 ⟹ 返回一张 `bySlot` 全 0 的表，**不是错误**）
 * @param ranges **同一份 asset** 过 `scanClusterRanges` 的产物（invariant 1/2 的对照面；
 *              两份输入不同源由前置错误串点名，不静默）
 * @param primOfSlot objectSlot → 本产物的 prim 下标（装配层按 `MeshRef{asset → objectSlot}`
 *        现建）。🔴 方向是 **slot → prim** 而不是 prim → slot：一个 prim 可以有多个实例
 *        （多 objectSlot），反过来不是函数；装配层的 MeshRef 循环天然就是这个方向。
 *        值 = `kShadowProxyNoPrim` 表示该槽不属于本产物。
 * @return 与 `primOfSlot` 等长的钉段表；任一不变量不成立 → 以 `shadow proxy: invariant N …`
 *         开头的可归因错误串（五条前缀两两不同）
 */
export [[nodiscard]] std::expected<ShadowProxyTable, std::string> buildShadowProxyTable(
	const MeshletAsset& asset, const MeshletRangeScan& ranges,
	std::span<const uint32_t> primOfSlot);

}  // namespace RGL
