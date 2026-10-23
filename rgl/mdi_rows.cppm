export module mdi_rows;

import rlstd;

/**
 * mdi_rows —— 真 MDI 阴影段的**行表构建器**（docs/mdi_plan.md §19.3 ★M-0，rlbase，纯 CPU 无设备）
 *
 * ## 它解决的那一件事（§19.2 D1）
 * 阴影段簇路径下每级联本来有**一堆逐候选 draw**（`emitShadowCasters` 逐候选 × 逐级发
 * `DrawCall`）。真 MDI 把它们换成**一次** `drawMeshTasksIndirectEXT`：每候选一行 24B 参数，
 * GPU 逐行读自己的 `groupCountX`（spike 的 `per_draw_groupcount` 已实测）。本模块只做
 * 「候选序列 → 行数组 + 聚合锚」这一步纯 CPU 换算，**零消费者**（★M-1 才接 GPU 载体与 shader）。
 *
 * ## 行布局（§19.2 D1 的 24B；🔴 ★M-3T-c 起第 6 个 word 已启用）
 * ```
 * 行 i = [ gcX | 1 | 1 | objectIndex | clusterStart | meshGroupCount ]   共 24B = 6 word
 *         └──── 前 12B 恰是 VkDrawMeshTasksIndirectCommandEXT ────┘└ task 读 word[3]/[4]/[5] ┘
 * ```
 * 为什么 24 不是 20：`clusterStart` 逐 object 不同（LOD 回落）⟹ 16B 的单个 word[3] 装不下两个
 * 索引；24 = 8 的倍数（对齐友好）。🔴 **原本"留给 ★M-3"的那枚保留 word 已被 ★M-3T-c 的
 * `meshGroupCount` 用掉** ⟹ ★M-3 真要加「材质/顶点块」那一维时得把行扩到 32B（8 word），
 * 代价 = 行表多 6KB（1530 行 × 8B × 3 级联），换来的是 ×2 不必在 shader 里再拼一遍。🔴 计划正文的 ASCII 图把保留位画成三枚 0（那是 32B），与同节
 * 标题「行布局 = 24B」、D3 的 `drawMeshTasksIndirectEXT(…, 24)` 与「候选数 × 24B × 3 级联 ≈
 * 114KB」三处冲突 ⟹ 本模块按 **24B** 落地（三处对一处），差异已写进交付报告。
 * 🔴 `cascadeIndex` **不进行、走 push**（一次 MDI 调用 = 一个级联 = 一个常量），故
 * `ShadowMeshletDrawArgs` 的 12B 契约与执行器通用 push 路径一字节不动。
 *
 * ## 🔴 前 12B 的对账**不在本模块**做（rlbase 不许 import rlvk）
 * 分层护栏（`tools/checks/rules_layers.py`）把「rlbase 整层无设备依赖」变成机器断言，所以这里
 * 只有裸 `uint32_t` 字段、没有 `VkDrawMeshTasksIndirectCommandEXT` 这个名字。"前 12B 恰是该
 * 结构"这条由 **tests/test_mdi_rows** 做第二钉（`tests/` 链全部四层库，可以 `import rlvk;`）——
 * 计划里写的「`static_assert` + 用例**双钉**」= 本文件的 24B/stride 断言 + 测试里的逐字段偏移对账。
 *
 * ## 🔴 mesh 组槽数不在本模块乘
 * 候选带来的 `meshGroupCount`（= 每簇 2 槽那个 ×2 的成品数）由**调用方算好传进来**，本模块只搬运。
 * 唯一真相源 =
 * `render_list_impl.cppm::meshletTaskCount`（`meshlet_lod.cppm` 文件头已为这条立过规矩：那里
 * 不乘、这里也不乘，第三个消费者出现时仍然只有一处乘）。本模块出现槽数常量 = 造第二个真相源。
 * 🔴 ★M-3T-c 起这个 ×2 成品数喂的是 `meshGroupCount`（task 拿去当 `DispatchMesh` 的 X），
 * **不再是** `groupCountX`（后者 = 每 draw 一枚 task：可见 ? 1 : 0）。把 ×2 挪进 task shader
 * 同样算造第二个真相源，所以行里直接带成品数。
 *
 * ## ★M-1a：行表的**尺寸/偏移算术**同样住在本模块（rlbase，`just test` 可钉）
 * `mdiShadowSliceBytes` / `mdiShadowArgsCapacityBytes` / `mdiShadowTailBytes` /
 * `mdiShadowRangeFits` / `mdiShadowRowLocation` 是「byte ↔ row」这一层唯一的算式产地
 * （§19.5）。它们错了不崩：越界读只是 VU 违例（07089/07090），读歪一行只是「画的是另一条
 * 合法命令」。设备侧（`meshlet_shadow_args`）只做**校验 + memcpy**，不自算第二个偏移。
 *
 * ## 行序 = 输入候选序，逐元素对应（**这是契约，不是实现细节**）
 * 「逐位不变」的前提就是这条：★M-1 的 A3 主判据是「MDI 形态 vs 逐条 draw 形态终图逐位相同」，
 * 而 depth-only 图集的 min 语义只在**同一覆盖集**上才与图元序无关。行序一旦变成「按可见性压紧」
 * 或「按 objectSlot 重排」，那条判据立刻失去依据。压紧/前缀和那笔代价本刀**不付**（§19.2 D4：
 * spike 的 `zero_groupcount_noop` 实测 0 计数不跳号 ⟹ 孔洞流合法）。
 *
 * ## 不可见候选写 `gcX = 0`，**其余字段照写**
 * 身份字段（`objectIndex` / `clusterStart`）在不可见行上照样落值：红了要能定位是**哪一行**，
 * 而「整行清零」让行表失去可归因性（且与「身份字段与可见性无关」这条 §19.2 D4 口径一致）。
 *
 * ## 五条不变量 = 构建器/对账器的断言，每条一个**两两不同**的错误串
 * 前缀 `mdi rows: invariant N`（读日志即可归因，不必回来读代码；同 `meshlet_ranges` /
 * `meshlet_lod` 的纪律）。任务书要求「至少覆盖」三条（下表 1-3），第 4 条是**行序契约**本身——
 * 它不在聚合量上留痕迹（两行互换 ⟹ 三个锚一位不变），不钉就等于没写。
 *
 * | # | 不变量 | 不成立时意味着 |
 * |---|---|---|
 * | 1 | 行数 == 候选数（且 `rowCount` 锚 == 行数）| 构建器自己漏行/多行，或有人给行表做了压紧 |
 * | 2 | **可见**候选的 `meshGroupCount > 0` | 上游契约破了（`meshletTaskCount(0)` 或选级表 count 为 0 却仍判可见）；宁可报错也不发一枚 `DispatchMesh(0)` 的空 task |
 * | 3 | 聚合锚自洽：`Σ gcX` == 逐行求和、`emptyRows` == 行数中 `gcX==0` 者、且后者 == **不可见候选数** | 锚与行分家 = ★M-1 的 `mdi.rows` 闭式锚失效，收益判据读不到数 |
 * | 3b | `Σ gcX == rowCount − emptyRows`（🔴 ★M-3T-c：`gcX ∈ {0,1}`，每**可见**行恰一枚 task）| 有人把 ×2 又乘回 gcX、或给一个 draw 发了多枚 task ⟹ task 数回到 mesh 组数量级，本批 3.4× 收益静默归零 |
 * | 4 | 行 i 恰是候选 i 的行（逐元素：gcX / objectIndex / clusterStart / **meshGroupCount**）| 行序不再是候选序（排序/互换/错位），A3 的逐位论证失去依据 |
 *
 * 不变量 2 只在**构建器**判（它是输入契约）；1/3/4 住在 `validateMdiShadowRows`——构建器返回前
 * 跑它（自检），★M-4「GPU 写行表 vs CPU 构建器逐元素对账」也跑它（同一份判据，两处真相源迟早漂）。
 *
 * ## 为什么在 rlbase
 * 判据 = 「这段逻辑需不需要一个 VkDevice 才能跑」：不需要（只读一段候选 span）⟹ rlbase，
 * `just test`（无 GPU 无窗口）可钉。形状照 `meshlet_lod` / `meshlet_ranges`（同为纯 CPU 扫描器，
 * 纯搬运**不携带 Vulkan 布局身份**）。
 */

namespace RGL {

/**
 * 一行 MDI 参数（§19.2 D1 的 24B 行；前 12B 与 `VkDrawMeshTasksIndirectCommandEXT` 逐字段同）。
 *
 * 字段名取 Vulkan 那个结构的名字（`groupCountX/Y/Z`）而不是「taskCount」之类的别名：GPU 读的就是
 * 那三个字段，改名只会让「前 12B 恰是该结构」这条对账多一次翻译。Y/Z 恒 1（§19.1：簇路径的
 * 派发是一维的），故它们只在默认值里出现一次。
 */
export struct MdiShadowRow {
	/**
	 * 本行的 **task** 数。★M-3T-c 起 = `可见 ? 1 : 0`（🔴 语义从"mesh 组数"变成"task 数"）：
	 * 一个 draw 发**一枚** task，由它读行表后一次 `DispatchMesh(meshGroupCount, 1, 1)` 把本对象
	 * 本级联的全部 mesh 组发出来。不可见候选 = 0（GPU 空转，spike 实测不跳号）。
	 *
	 * 🔴 **为什么不是"每 mesh 组一枚 task"**（★M-3T-a 的初版形态）：那样 task 数 = mesh 组数
	 * （实测每级联 5132），task 自身开销把 MDI 的命令数收益在 GPU 侧倒赔掉约 0.5ms
	 * （→ §19.16 代价读数）。收成每 draw 一枚后 task 数 = 可见行数（1530 量级，3.4×）。
	 */
	uint32_t groupCountX = 0u;
	uint32_t groupCountY = 1u;  ///< 恒 1（一维派发）
	uint32_t groupCountZ = 1u;  ///< 恒 1（一维派发）
	/** = `MeshRef.objectSlot` = shader 的 objectIndex（word[3]，task 读后进 payload）。 */
	uint32_t objectIndex = 0u;
	/**
	 * 🔴 **相对 `ObjectClusterRange::base` 的偏移**，不是全局簇下标（口径 = `MeshletLodPrimRange::start`
	 * / `ShadowMeshletDrawArgs::clusterStart`）。shader 侧 `cluster = base + clusterStart + 半区`，
	 * base 由 `readObjectClusterRange(objectIndex)` 提供；传全局下标 = base 被加两遍 =
	 * 「同资产多 prim / 多实例」上画错几何。
	 */
	uint32_t clusterStart = 0u;
	/**
	 * 本行的 **mesh 组**数（= `meshletTaskCount(选级后簇数)`；「每簇 2 槽」的 ×2 唯一真相源在 C++
	 * 侧，🔴 shader 里不得再拼一遍 ×2）。task 拿它当 `DispatchMesh` 的 X；mesh 侧 `groupID.x`
	 * 的取值域因此 = `[0, meshGroupCount)`，与 ★M-3T-a 之前的 `SV_GroupID.x` **逐值同**
	 * ⟹ mesh 的派发算式不必为 MDI 特化（★M-3T-a 那版得从 payload 拿 slot 就是因为 gcX 当时是
	 * mesh 组数、task 之下 `groupID.x` 恒 0）。
	 *
	 * 🔴 本 word 占用了 §19.2 D1 那枚**保留 word**（原留给 ★M-3 模型段那一维）⟹ ★M-3 真要加
	 * 那一维时得把行扩到 32B（8 word）。
	 */
	uint32_t meshGroupCount = 0u;
};
static_assert(sizeof(MdiShadowRow) == 24,
	"MdiShadowRow layout drifted from docs/mdi_plan.md §19.2 D1 (24B MDI row: the first 12B "
	"must stay exactly VkDrawMeshTasksIndirectCommandEXT)");
static_assert(alignof(MdiShadowRow) == alignof(uint32_t),
	"MdiShadowRow must stay a plain uint32 word array (no padding, no over-alignment)");

/**
 * MDI 行步长（VUID-07088 里的 `stride`：必为 4 的倍数且 ≥ `sizeof(VkDrawMeshTasksIndirectCommandEXT)`
 * = 12）。与 `sizeof(MdiShadowRow)` **同一个常量**（不是抄一份 24）：行表是「按 stride 逐行跳」的
 * 裸字节算术，两处各写一遍 ⟹ 有人给行结构加字段时只改一处 = GPU 逐行读歪，画面表现为「随机少画
 * 一块几何」且没有任何错误信号。合法性（4 的倍数且 ≥ 12）另有一条用例钉（tests/test_mdi_rows）。
 */
export constexpr uint32_t kMdiShadowRowStride = static_cast<uint32_t>(sizeof(MdiShadowRow));
static_assert(kMdiShadowRowStride == 24,
	"kMdiShadowRowStride is no longer 24 — §19.2 D1 fixed the row layout at 24B (first 12B must "
	"stay exactly VkDrawMeshTasksIndirectCommandEXT)");

/**
 * GPU 真正读走的命令本体字节数 = `sizeof(VkDrawMeshTasksIndirectCommandEXT)`（§19.5 末两条
 * buffer 边界 VUID 的那个 `+12` 加数）。
 *
 * 🔴 与 `kMdiShadowRowStride`（24）**是两个数**：行表按 24 逐行跳，设备每行只读前 12B。
 * 把两者混成一个常量 = 「容量算多了 12B/行」这种**看起来更安全**的错（VUID 边界算式随之失去
 * 意义：`stride × (drawCount−1) + offset + 12` 退化成 `stride × drawCount`，永远成立）。
 * rlbase 不许 import rlvk ⟹ 这里只能是裸常量，**第二钉**在 tests/test_mdi_rows
 * （`static_assert(kMdiShadowCommandBytes == sizeof(VkDrawMeshTasksIndirectCommandEXT))`）。
 */
export constexpr uint32_t kMdiShadowCommandBytes = 12;
static_assert(kMdiShadowCommandBytes <= kMdiShadowRowStride,
	"the indirect command body must not exceed the row stride (VUID-…-drawCount-07088 wants "
	"stride >= sizeof(VkDrawMeshTasksIndirectCommandEXT))");

/**
 * 一次 MDI 调用的**行位置对**（§19.5 的核心：GPU 侧字节偏移与 shader 侧行基址必须同源）。
 *
 * 🔴 两个数各算一遍迟早漂，而漂的症状是 **shader 读错行 = 画错几何**，不报错、不触发 VVL
 * （行表是裸字节算术，越界读落在同一张 buffer 里就是「另一行的合法数据」）。故本项目里
 * 这对数**只有一个产地** = `mdiShadowRowLocation`，调用点不许自己乘步长、不许自己加帧槽偏移。
 */
export struct MdiShadowRowLocation {
	/** GPU 侧：`vkCmdDrawMeshTasksIndirectEXT` 的 `offset` 参数（**字节**）。 */
	uint64_t argsOffset = 0;
	/** shader 侧：`slot = rowBase + SV_DrawIndex` 的 `rowBase`（**行号**，进 12B push）。 */
	uint32_t rowBase = 0;
};

/**
 * 一个帧槽（slice）的字节数 = `rowCount × kMdiShadowRowStride`（§19.5：slice = 一帧的整张行表，
 * 覆盖本级联段之外的所有段；轮转落在**偏移**上而不是句柄上，故描述符只需覆盖整张）。
 */
export constexpr uint64_t mdiShadowSliceBytes(uint32_t rowCount) {
	return static_cast<uint64_t>(rowCount) * kMdiShadowRowStride;
}

/**
 * 整张 ring buffer 的字节容量 = `slices` 个 slice（`slices` = 帧槽数 = `kFramesInFlight`）。
 *
 * 🔴 尺寸只有这一条公式：`argsOffset` 由 `mdiShadowRowLocation` 算、容量由本函数算，
 * 两者共用同一个 `kMdiShadowRowStride` 与同一个 `sliceRows` ⟹「最后一行越界」在构造上
 * 不可能出现（不是靠 rows×stride 大概够）。越界读不会崩，只是 VU 违例（门关档不跑 VVL
 * 就不现形），所以它必须是一条**闭式**公式而不是各处加加减减。
 */
export constexpr uint64_t mdiShadowArgsCapacityBytes(uint32_t rowCount, uint32_t slices) {
	return mdiShadowSliceBytes(rowCount) * slices;
}

/**
 * 一次调用在行表里读到的**末字节 +1**（= 末行的 `offset + 12`）。
 *
 * 🔴 VUID-vkCmdDrawMeshTasksIndirectEXT-drawCount-07089（`drawCount == 1`：`offset + 12 ≤
 * buffer 大小`）与 -07090（`drawCount > 1`：`stride × (drawCount − 1) + offset + 12 ≤
 * buffer 大小`）在 `drawCount == 1` 时重合（`stride × 0 = 0`）⟹ **一条闭式判据同时是两个 VUID**。
 * `drawCount == 0` = spec 允许的「一条都不发」，不读任何字节 ⟹ 尾 = 起点（不许在此处
 * 让 `drawCount − 1` 无符号回绕成 `2^32−1` 个 stride，那会把「容量够不够」读成永远不够）。
 */
export constexpr uint64_t mdiShadowTailBytes(uint64_t argsOffset, uint32_t drawCount) {
	if (drawCount == 0u)
		return argsOffset;
	return argsOffset + static_cast<uint64_t>(kMdiShadowRowStride) * (drawCount - 1u)
	     + kMdiShadowCommandBytes;
}

/**
 * buffer 边界闸门（07089/07090 合一）：本次调用读到的最后一个字节必须落在容量内。
 *
 * 期望的失效形态不是崩溃而是 VU 违例（症状 = 驱动读到 buffer 尾外的**别人的**内存当命令），
 * 所以写侧（`MeshletShadowArgsRing::upload`）与容量算式共用本判据，两道都过才写。
 */
export constexpr bool mdiShadowRangeFits(uint64_t capacityBytes, uint64_t argsOffset,
	uint32_t drawCount) {
	return mdiShadowTailBytes(argsOffset, drawCount) <= capacityBytes;
}

/**
 * 🔴 **同源 pair 的唯一具名入口**：给定「每帧槽行数 + 帧槽号 + 段起点行号」，一次产出
 * GPU 侧字节偏移与 shader 侧行基址。
 *
 * 两者由**同一个** `row = frameSlot * sliceRows + firstRow` 推出（`argsOffset = row × stride`、
 * `rowBase = row`）⟹ 结构上不可能各算一边。§19.5 的 ring×2 免第二槽解法完全依赖这条：
 * 描述符覆盖整张、管线条目与 shader object 启动期烘一次不动，轮转只改这两个数。
 *
 * 前置（由载体创建时的容量闸门守，见 `createMeshletShadowArgsRing`）：
 *  · `frameSlot < slices`、`firstRow < sliceRows`（调用方守；越界 = 段起点落进下一个 slice
 *    = 把上一帧的行当本帧读，画面表现为「影子滞后一帧」而不报错）；
 *  · `row ≤ UINT32_MAX`（`rowBase` 是 push 里的 32 位行号）。
 *
 * @param sliceRows 一个帧槽的行数（= 载体的 `sliceRows`，两处必须同一个变量）
 * @param frameSlot 帧槽号（= `FrameSession::currentFrame`，🔴 不许与 GPU 侧另取一个）
 * @param firstRow  本段（本级联）在 slice 内的起始行号
 */
export constexpr MdiShadowRowLocation mdiShadowRowLocation(uint32_t sliceRows, uint32_t frameSlot,
	uint32_t firstRow) {
	const uint64_t row = static_cast<uint64_t>(frameSlot) * sliceRows + firstRow;
	return MdiShadowRowLocation {
		.argsOffset = row * kMdiShadowRowStride,
		.rowBase = static_cast<uint32_t>(row),
	};
}

// ———— §27 直派模拟算式（gfx1151 workaround，上游 Mesa#TODO；判据 = 取证文档 §26/§27）————
//
// 直派模拟 = 把每级联的 `drawMeshTasksIndirectEXT(rowCount 行)` 换成语义等价的
// `drawMeshTasksEXT(gridX,1,1)`：行表、push（rowBase/cascade）、task 取数、payload、可见性
// 语义**一个不动**，只换派发指令。行消费契约从「MDI 逐行启动 task」变成「逐组算行号 +
// 空行早退」——下面三枚纯函数就是这份新契约的 **CPU 侧唯一算式产地**（task 侧
// `shadow_meshlet.task.slang` 的 MDI_DIRECT 分支是它的第二份拼写，两份的数值对账 =
// tests/test_mdi_direct_emu 的闭式期望；同「C++↔shader 双拼写」的既有格局）。

/**
 * 行表 → **保守上界网格**：每行一组（不变量 3b「每可见行恰一枚 task」的对偶——直派网格
 * 无法按可见性压紧，压紧要前缀和，§19.2 D4 已裁决不付那笔代价）⟹ grid X = 段行数，
 * 含 gcX=0 的孔洞行（它们靠 `mdiDirectEmuSkipRow` 早退）。
 */
export [[nodiscard]] constexpr uint32_t mdiDirectEmuGridX(uint32_t rowCount) {
	return rowCount;
}

/**
 * 网格容量判据：task 管线的 `groupCountX ≤ maxTaskWorkGroupCount[0]`
 * （VUID-vkCmdDrawMeshTasksEXT-TaskEXT-07322，Y=Z=1 ⟹ 乘积判据 07325 同式退化；
 * spec 下限 65535，真产物 ~1.5k 恒过）。判负 = 装配层回落 MDI 形态并出声
 * （workaround 自己不许把画面搞没）。
 */
export [[nodiscard]] constexpr bool mdiDirectEmuGridFits(uint32_t gridX,
	uint32_t maxTaskWorkGroupCountX) {
	return gridX <= maxTaskWorkGroupCountX;
}

/** 组号 → 行号（task 的 `row = rowBase + SV_GroupID.x` 的 CPU 对偶；行内序恒 0 = 不变量 3b）。 */
export [[nodiscard]] constexpr uint32_t mdiDirectEmuRow(uint32_t rowBase, uint32_t groupX) {
	return rowBase + groupX;
}

/**
 * 空组早退判据：gcX == 0 的行在 MDI 下**根本不启动 task**（`VkDrawMeshTasksIndirectCommandEXT`
 * 的 groupCountX=0），直派下网格覆盖它 ⟹ task 启动后必须按同一判据早退，两边消费才等价。
 * 🔴 只看 word[0]（gcX），不看身份字段——不可见行的身份字段照写（文件头口径）。
 */
export [[nodiscard]] constexpr bool mdiDirectEmuSkipRow(uint32_t groupCountX) {
	return groupCountX == 0u;
}

/**
 * 一个候选（一个级联内的一条阴影投影体候选，字段口径 = `ShadowCandidate` 的可切那半）。
 *
 * 🔴 **本结构不携带 mesh 组槽数口径**：`meshGroupCount` 是「本行要发多少 mesh 组」的**成品数**，
 * 由调用方经 `meshletTaskCount(簇数)` 算好（唯一真相源，文件头）。构建器只搬运它。
 */
export struct MdiShadowCandidate {
	/** = `MeshRef.objectSlot`（进 word[3]，与 `ObjectClusterRange` 等 per-object 表同一条索引）。 */
	uint32_t objectSlot = 0u;
	/**
	 * 本候选的 **mesh 组**数（= `meshletTaskCount(选级后簇数)` 的成品数，🔴 不在本模块乘槽数）；
	 * 可见时必须 > 0（不变量 2）。★M-3T-c 起它喂 `MdiShadowRow::meshGroupCount`（task 拿去当
	 * `DispatchMesh` 的 X），**不再**喂 `groupCountX`（后者 = 每 draw 一枚 task）。
	 */
	uint32_t meshGroupCount = 0u;
	/** 🔴 相对 `ObjectClusterRange::base` 的偏移（进 word[4]，与 push 里的同值同源）。 */
	uint32_t clusterStart = 0u;
	/** 本候选在本级联是否落进光空间体（`casterInShadowVolume` 的结论）。 */
	bool visible = true;
};

/**
 * 一次构建的产物：行数组 + 三个聚合锚。
 *
 * 三个锚不是便利品，是**判据**（§19.3 ★M-0 的「聚合锚」= §19.2 D5 的 profiler 口径原料）：
 * `rowCount` 是 ★M-1 判据 A5 里 `mdi.rows` 的闭式锚，`sumMeshGroupCount` 是「与逐条 draw 形态的
 * Σ groupCount 必须相等」的对账量（task 化不改 mesh 组总数 ⟹ 跨形态可比），`sumGroupCountX` 是
 * 「总共发了多少 task」的**收益**读数，`emptyRows` 是「孔洞流」的规模。
 * 三者与行数组的自洽 = 不变量 3。
 */
export struct MdiShadowRowTable {
	/** 行数组，**下标 = 候选下标**（行序 = 候选序，文件头契约）。 */
	std::vector<MdiShadowRow> rows;
	/**
	 * Σ `groupCountX` = 本级联发出的 **task** 总数（= 本批的**收益**读数：★M-3T-a 的形态下它
	 * = mesh 组数 5132 量级，★M-3T-c 起 = 可见行数 1530 量级）。🔴 `gcX ∈ {0,1}` ⟹ 本量恒等于
	 * `rowCount − emptyRows`（不变量 3b）。
	 */
	uint64_t sumGroupCountX = 0;
	/**
	 * Σ **可见行**的 `meshGroupCount` = 本级联真正发出去的 **mesh 组**总数。**它才是**「与逐条 draw
	 * 形态的 Σ groupCount 必须相等」的那个对账量（★M-3T-c 前挂在 `sumGroupCountX` 上，语义随 gcX
	 * 改单位一起搬到这里）。task 化只改"谁发"、不改"发多少 mesh 组" ⟹ 本量是 task 化前后**必须
	 * 不变**的锚。🔴 **不可见行不计**（逐条形态不为它们发 draw；全行求和会得到一个不随可见性动的
	 * 死数，实测三档级联一位不变 ⟹ 对账价值为零）。
	 */
	uint64_t sumMeshGroupCount = 0;
	/** `groupCountX == 0` 的行数（= 本级联不可见候选数）。 */
	uint64_t emptyRows = 0;
	/** 行数（== 候选数，不变量 1）。 */
	uint64_t rowCount = 0;
};

/**
 * 候选序列 → 24B 行数组 + 聚合锚（§19.3 ★M-0 的唯一入口）。
 *
 * 一趟线性扫（真产物一个级联 ≤ 候选数 1591 量级，纳秒级/行），★M-1 里每帧每级联调一次。
 * 无堆外副作用、不碰设备、不读配置——门 `meshlet.mdi_shadow` 的**消费**在 ★M-1（本批零消费者）。
 *
 * @param cands 一个级联内的候选序列（顺序即行序；空序列 = 合法的空表，不是错误）
 * @return 行数组 + 聚合锚；任一不变量不成立 → 以 `mdi rows: invariant N …` 开头的可归因错误串
 *         （各条前缀两两不同）
 */
export [[nodiscard]] std::expected<MdiShadowRowTable, std::string> buildMdiShadowRows(
	std::span<const MdiShadowCandidate> cands);

/**
 * 行表与它声称的那份候选清单**逐元素**对账（不变量 1 与 3；错误串与构建器同源）。
 *
 * 两个消费者，一份判据：① `buildMdiShadowRows` 返回前自检；② ★M-4「GPU 直写 `gcX`」与 CPU
 * 构建器逐元素对账（§19.3 那一行的「照 `meshlet_hand` 的『序，不是计数』口径」）。
 * 判据面 = 行数 / 逐行 `gcX` 与身份字段 / 三个聚合锚，**不**只看聚合量——GPU 版将来要逐元素
 * 对得上，序本身就是契约的一部分（`test_meshlet_lod` 的 base-shift 教训：聚合量抓不到错位）。
 *
 * @param table 待验行表（行数组 + 锚）
 * @param cands 它声称来自的那份候选清单
 * @return nullopt = 自洽；有值 = 首个可归因错误串
 */
export [[nodiscard]] std::optional<std::string> validateMdiShadowRows(
	const MdiShadowRowTable& table, std::span<const MdiShadowCandidate> cands);

/**
 * 行表在 MDI 阴影管线条目上的 **binding 号**（债-140 第四击 Ⓐ）。
 *
 * 为什么要有名字而不是两处各写 34：烘那一步（`addShadowMeshletMdiEntry` 往
 * `bindings.extra` 塞条目）与对账那一步（`logBakedRowMappingAudit` 按号查烘入的 mapping）
 * 必须查**同一个号**——写两份字面量，改号时必漂，而漂了的症状是"对账永远说没烘"，
 * 一条假的红字比没有红字更糟。
 *
 * 号源：定案号 22 被 sceneHeader UBO 占用（docs/mdi_plan.md §19.2 D2），实测让位到 34。
 * 🔴 shader 侧的对应声明 = `rgl/shaders/shadow_meshlet.task.slang` 的
 * `[[vk::binding(34, 0)]]`；C++↔shader 两侧同向由批 M 的 `declaredBindingsUnion` 在运行期兜
 *（SPV 没声明的 binding 不烘），本常量只保证 **C++ 侧两处**不各抄一份。
 */
export constexpr uint32_t kMdiShadowRowBinding = 34;

}  // namespace RGL
