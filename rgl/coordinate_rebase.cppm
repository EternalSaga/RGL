module;
#include <glm/glm.hpp>
export module coordinate_rebase;

import rlstd;
import scene_context;  // quantizeShadowDistance（取 2 的幂的算子，本模块复用、不重写那个算式）

/**
 * coordinate_rebase —— 坐标层的家（rlbase）：债-85 两条约束的机器判据（相对坐标计划批 1）
 * + 全仓唯一的「绝对 → 相对」换算入口 toRelative（批 3 第一刀；批 4 第二刀起有矩阵/点两个
 * 重载，减法仍只有一处——点版委托矩阵版）
 * + 跨帧原点差的复合 motionWithOriginShift（批 4 第一刀：TAA / velocity 的跨帧簿记）。
 *
 * 债-85（docs/还债总账.md）：origin rebasing（把世界原点搬到相机）2026-10-25 owner 拍板**要做**
 * （门关逐位不变交付），两条约束是施工时必须遵守的硬约束：① 决定 float32 抖动量级的是
 * **相机离原点多远**（不是地图多大）；② 原点 snap 粒度必须取**最粗消费者（阴影图集纹素）的尺度**，
 * 不是相机精度。「下次动原点前先读债-85」这条口头防线已失效两次（docs/相对坐标地基_plan.md §0.3），
 * 本模块把两条约束变成纯函数 + 单测（tests/test_coordinate_rebase.cppm），让防线不再依赖"有人读到"。
 *
 * ## 护栏先于实现（批 1 时的「零消费者是故意的」，现已有消费者）
 * 批 1 落地时本模块零生产调用点 —— 护栏先于实现，是为了让实现一落地就撞上它，
 * 而不是让它自己发明一套规则（docs/相对坐标地基_plan.md §3 批 1b）。批 3 起 toRelative
 * 有了第一个消费者（实例矩阵生产点），批 4 第一刀起 motionWithOriginShift 有了第二个
 * （taaSystem），批 4 第二刀起点版重载有了第三、四个（cameraView 的 eye/target 与
 * fillLightData 的 eyePos/灯位置 —— 「shader 侧世界坐标一律相对原点」这一个不变量的出口）。
 * 🔴 删除本模块或其单测 = 拆掉债-85 ② 的唯一机器防线。
 *
 * ## 分层判据（AGENTS：这段逻辑需不需要一个 VkDevice 才能跑）
 * 不需要 —— 输入是相机位置与纹素步长，输出是标量/向量，纯数学 ⟹ rlbase，`just test` 毫秒级可跑。
 */

namespace RGL {

/**
 * 「0.5px 可见爬行」的触发相机距离（世界单位）= 7.6e5，债-85 行内量级表的实测读数
 * （|camera|≈625 → 4.1e-4 px；1e4 → 6.6e-3；1e5 → 6.6e-2；0.5px 爬行触发点 ≈ 7.6e5）。
 *
 * 🔴 **不许在本文件重新推导这个数** —— 自己反推一遍 = 造出第二份会漂的副本
 * （AGENTS 哲学第 1 条；反推放大系数实测对不上，理由 → docs/相对坐标地基_plan.md §5 末）。
 * 口径绑**交付档焦距/分辨率**；换焦距/分辨率时**重新实测触发点并改这个常量**，
 * 不做公式换算。出处 = docs/还债总账.md 债-85 行。
 */
export inline constexpr double kVisibleCrawlCameraDistance = 7.6e5;

/**
 * 相机离世界原点的距离 = |cameraPosition|。
 *
 * 债-85：「决定抖动的是相机离原点多远，不是地图多大」—— 这是决定 float32 抖动量级
 * （上界 = ULP(|camera|)）的唯一自变量，coordinateHeadroom 的入参。
 * 输入用 double：rebase 开工的那天 |camera| 就在 1e6 量级，float32 连位置本身都存不准。
 * 非有限分量原样传播（NaN/inf 进 → NaN/inf 出），合法性裁决在 coordinateHeadroom。
 */
export double distanceFromOrigin(const glm::dvec3& cameraPosition) {
	return glm::length(cameraPosition);
}

/**
 * 距离余量（数量级）= log10(kVisibleCrawlCameraDistance / distance)。
 *
 * 返回 < 1 ⟹ |camera| 已进入触发点一个数量级以内 = origin rebasing 的唯一开工凭据
 * （docs/相对坐标地基_plan.md §3 批 1a / §5）。当前交付机位 |camera| ≈ 625 → ≈ 3.08（现在绿）。
 *
 * 边界与非有限输入的行为（明确写死，不靠读者猜）：
 *  - distance == 0（相机恰在原点，**合法**输入，抖动为零）⟹ +∞，不许出 NaN；
 *  - distance < 0 或 NaN（距离不可能为负 = 调用方 bug）⟹ NaN，不假装合法；
 *  - distance == +∞ ⟹ -∞（公式的自然结果：无穷远 → 余量无穷负，照常触发）。
 */
export double coordinateHeadroom(double distance) {
	if (distance == 0.0)
		return std::numeric_limits<double>::infinity();
	if (!(distance > 0.0))
		return std::numeric_limits<double>::quiet_NaN();
	return std::log10(kVisibleCrawlCameraDistance / distance);
}

/**
 * origin rebasing 唯一允许的原点 snap 粒度 = **最粗**级联的纹素世界步长，再取 2 的幂。
 *
 * ## 为什么是最粗一级（债-85 ②）
 * 粒度必须取「最粗消费者」的纹素尺度：阴影级联的半径随级联递增（每级各自从自己的拟合球推），
 * 最粗级的纹素最大 ⟹ 取数组最大值即取最粗级。按相机精度（ULP(|camera|)）取粒度会让吸附格
 * 跟着相机连续滑 ⟹ 等于把已结案的「影子随视角游动」重新引回来（docs/相对坐标地基_plan.md §0.2）。
 *
 * ## 输入从哪来
 * `render_list_impl.cppm` 的 `fillShadowData` 算的 `texelSizes`（语义「2·r/块边长，每级各自
 * 从自己的球推」），契约字段 `ShadowParamsBlock::texelSizes` 在 `rgl/shaders/scene.slang`。
 * 🔴 不要绕回去从 r 重算 —— 那会造出第二份副本。
 *
 * 取 2 的幂**复用** scene_context 的 quantizeShadowDistance（向上取整，幂等），不重写那个算式；
 * 格距为 2 的幂是 snapOriginToGranularity 注释里格点嵌套性质的前提。
 *
 * ## 非法输入行为（🔴 不许返回 0 假装合法 —— 0 粒度会让 snap 退化成不 snap，
 * ## 而这正是本护栏要拦的形态）
 *  - 空输入或全零（没有任何消费者）⟹ NaN；
 *  - 任一分量为负或非有限（上游级联几何坏了）⟹ NaN；
 *  - 恰为 0 的分量**跳过**，不参与「最粗级」。⚠ **口径如实报**：`fillShadowData` 现在把四级
 *    `texels` **全部填满**（循环跑满 `kActiveShadowCascades`，声明处的 `{0.0f}` 只是初值），
 *    所以 0 在**当前**代码里只可能来自 `shadowTexelSize(xyRadius = 0)` 的退化级联——那种情况本应
 *    出声，但它与「将来级联数可配置时的未生效槽」在数据上不可区分，故本函数取宽松侧（跳过）。
 *    **真正的判据在调用方：它才知道生效级数。**
 *    🔴 别把这条与**同一个函数里的 `splits`** 搞混：那边未生效槽**必须写 kNoFar、留 0 会让整个
 *    广场全黑**（实测教训在 `fillShadowData` 注释里）。两个数组同出一个函数，0 的语义正好相反。
 */
export double rebaseSnapGranularity(std::span<const float> texelWorldSteps) {
	float coarsest = 0.0f;
	for (const float step : texelWorldSteps) {
		if (step == 0.0f)
			continue;  // 未生效级联槽
		if (!(step > 0.0f) || !std::isfinite(step))
			return std::numeric_limits<double>::quiet_NaN();  // 负 / NaN / inf：不假装能选粒度
		coarsest = step > coarsest ? step : coarsest;
	}
	if (coarsest == 0.0f)
		return std::numeric_limits<double>::quiet_NaN();  // 空输入或全零：无消费者
	return static_cast<double>(quantizeShadowDistance(coarsest));
}

/**
 * 把期望原点逐轴 snap 到 granularity 的整数倍（恰在半格处向远离零的方向取整）。
 *
 * 除法用 double 算：desired 在 1e6 量级（rebase 开工那天 |camera| 就在那），float32 在 1e6
 * 处的 ULP 是 0.0625 —— 比任何一级纹素步长都粗，float 除法连「该 snap 到哪条格线」都表示不出。
 *
 * ## 格点嵌套性质（债-139 结案时依赖它）
 * granularity 出自 rebaseSnapGranularity ⟹ 是 2 的幂 ⟹ **相邻粗细档的格线重合**（粗格线必落在
 * 细格线上）⟹ 级联跨档时影边最多挪半个粗纹素，而不是两套互不相关的格点随机错位
 * （scene_context quantizeShadowDistance 注释里的同一条性质，原点这一份此前无人写）。
 *
 * ## 🔴 2 的幂倍平移是**精确浮点运算**（判据论证，比上面那条画质论证更硬）
 * ΔO = k × granularity 且 granularity 是 2 的幂 ⟹ `c + ΔO` **不改尾数** ⟹
 * ```
 * round((c + ΔO)/g)·g = round(c/g + k)·g = (round(c/g) + k)·g = snapped(c) + ΔO
 * ```
 * ⟹ 吸附后的光空间盒**恰好平移 ΔO**、与场景几何完全同构 ⟹ **阴影图集内容逐位不变**。
 * 这条就是「rebase 开 / 关可以在**图集级**做零容差逐位比对」的根据（判据分级
 * → docs/相对坐标地基_plan.md §3 批 5）。⚠ **屏幕级做不到逐位**——对象矩阵与 MVP 的平移列
 * 值确实变了、浮点路径不同，那一级的判据只能是「差异落在闭式精度界内」。
 * ⟹ 把 granularity 从 2 的幂改成任意值，代价不只是画质（格点不再嵌套），
 * **而是整条零容差判据失效**——这是本函数不许「顺手换个更细的格距」的真正原因。
 *
 * ## 非法 granularity 行为
 * granularity <= 0 或非有限 ⟹ 返回三分量全 NaN 的向量。**不返回 desired 原样** ——
 * 「0 粒度 = 不 snap」正是 rebaseSnapGranularity 注释点名要拦的静默退化形态，本函数不配合放行。
 */
export glm::dvec3 snapOriginToGranularity(const glm::dvec3& desired, double granularity) {
	if (!(granularity > 0.0) || !std::isfinite(granularity))
		return glm::dvec3(std::numeric_limits<double>::quiet_NaN());
	const auto snapAxis = [granularity](double v) {
		return std::round(v / granularity) * granularity;
	};
	return glm::dvec3{snapAxis(desired.x), snapAxis(desired.y), snapAxis(desired.z)};
}

/**
 * 全仓**唯一**的「绝对 → 相对」换算入口：绝对层世界矩阵减去某消费者的原点，收窄落 float32。
 *
 * ## 原点是参数，不是全局单例（本批的承重裁决）
 * 每个坐标消费者（渲染 / 物理 / 体积光 / 粒子模拟空间）各自携带自己的原点传进来，
 * 加新系统 = 新增一个原点声明，不动别人的签名（docs/相对坐标地基_plan.md §2.1）。
 * 门关态（原点恒为 dvec3(0)）下本函数是恒等：toRelative(A, 0) 与 glm::mat4(A) **逐位相同**
 * （单测钉死，它是「门关逐位不变」主判据的单元级对应）。
 *
 * ## 🔴 两行顺序承重：先在 double 域减，再一次性收窄（不可调换）
 * camera-relative 的精度收益**全部**来自「大数从不进 float32」：
 *  - 先减后收窄：float(c3 − origin) —— 减法在 double 域做，结果量级 = 离原点的相对距离，
 *    float32 的舍入只发生在这一个小量上；
 *  - 先收窄后减：float(c3) − float(origin) —— c3 先被 float32 舍过一道（|c3| ≈ 1e6 处
 *    ULP = 0.0625），丢掉的尾数减原点救不回来。
 * 调换顺序 = 本函数存在的理由消失；单测有专门的阳性用例钉「两种顺序结果必须不同」。
 *
 * ## 只动平移列 c3 的 xyz
 * 基向量三列（旋转/缩放，不含平移）与 c3.w 一律不动。c3.w 的不变不是「没减」而是
 * 「减了个精确的 0」：glm::dvec4(origin, 0.0) 的 w 分量是 0.0，IEEE 下任何数减 +0.0
 * 逐位返回自身（含 ±0 与 NaN 的符号语义）。
 *
 * 🔴 任何绕过本函数、自己拿绝对坐标减原点的写法都违反批 3 契约
 * （「全仓只有这一处做减法」→ docs/相对坐标地基_plan.md §3 批 3）。
 */
export inline glm::mat4 toRelative(const glm::dmat4& absolute, const glm::dvec3& origin) {
	glm::dmat4 rel = absolute;
	rel[3] = absolute[3] - glm::dvec4(origin, 0.0);  // ① 先在 double 域减平移列
	return glm::mat4(rel);                            // ② 再一次性收窄落库
}

/**
 * 唯一换算入口的**点**形态重载（批 4 第二刀：view 的 eye/target、`SceneLight::eyePos`、
 * `GpuLight::position` 三个出口都是点，不是矩阵）。
 *
 * ## 它**不是**第二个做减法的地方
 * 点 p 的坐标恰是平移阵 T(p) 的平移列，而「绝对 → 相对」就是矩阵版 toRelative 对 c3 做的
 * 那一个动作 ⟹ 本函数把 p 装成平移阵后**委托**给上面那个重载，自己一个运算符都不写。
 * 全仓「绝对 − 原点」的减法仍然只发生在矩阵版那两行里（批 3 契约
 * → docs/相对坐标地基_plan.md §3 批 3）；本重载只是给向量形态的消费者加一条访问路径，
 * 算式的唯一来源没有变（AGENTS 哲学第 1 条：单一来源要承重，访问路径必须消费它）。
 *
 * ## 🔴 只有**点**能传，方向不许传
 * 方向量（forward / up / 灯 direction）平移不变，减原点就是把它们改坏——批 4 任务书的
 * 「明确不许改」表逐条列过。本重载的存在不改变那张表的任何一行。
 *
 * ## 输出 float32（与矩阵版同形：double 域先减、一次性收窄，顺序承重）
 * 当前三个消费者全是 float 契约（`glm::lookAt` 的 float 版 / 反射生成物钉的 vec3、vec4 字段），
 * 收窄发生在减完原点**之后** ⟹ 大数从不进 float32（债-85 ① 的精度收益全部来自这一条顺序）。
 * 将来若有人要 double 精度的相对坐标，**另开入口**，别来改本函数的返回类型。
 *
 * ## 门关恒等
 * origin = dvec3(0) ⟹ 矩阵版减法逐位返回自身 ⟹ 本函数 = 对输入做一次收窄，
 * 与改动前各调用点的 `glm::vec3{absolute}` **逐位相同**（判据
 * = tests/test_view_light_rebase.cppm 的门关恒等锚）。
 */
export inline glm::vec3 toRelative(const glm::dvec3& absolute, const glm::dvec3& origin) {
	glm::dmat4 pointAsTranslation{1.0};
	pointAsTranslation[3] = glm::dvec4(absolute, 1.0);
	const glm::mat4 rel = toRelative(pointAsTranslation, origin);  // 减法只在被委托的那一处
	return glm::vec3{rel[3]};  // 收窄已由矩阵版做过（一次），这里只是拆列取回
}

/**
 * 实例世界矩阵的**唯一生产式**（资产局部 → 本帧渲染世界）= 先绝对域复合 spin，再减原点。
 *
 * ## 为什么它必须和 motionWithOriginShift 住同一个模块
 * 这两条式子是**同一个契约的两半**：本函数定下「第 t 帧的世界坐标长什么样」
 * （`W_t = S_t·A·p − O_t`，枢轴 = 绝对原点），motionWithOriginShift 的推导**以它为前提**
 * （换成旧顺序 `S_t·(A·p − O_t)`，平移项就变成 `prevSpin·ΔO` 而不是 `R·O_t − O_{t−1}`，
 * 两者只差在「spin 的枢轴在绝对原点还是在渲染原点」）。
 * 原本两半分住 render_list_impl.cppm 与 coordinate_rebase.cppm，中间只有一条注释连着
 * —— 那正是 AGENTS 哲学第 4 条点名的形态（注释是最后手段，不是防线）：实测把生产点改回
 * 旧顺序，**全部单测不红**。收进来之后判据 =
 * tests/test_coordinate_rebase.cppm「跨帧契约两半互锁」那条（变异读数见交付报告 M8）。
 *
 * ## 门关逐位不变
 * `origin = 0` ⟹ toRelative 恒等 ⟹ 本函数 = `glm::mat4(glm::dmat4(spin) * absolute)`。
 * 出厂态 spin 恒单位阵（configs/scene.lua `spin = false`）⟹ 与旧的
 * `spin.matrix * glm::mat4(tf.absolute)` 逐位相同（都是「乘单位阵」，累加里的 ±0 处理
 * 两种精度一致）⟹ 交付截图锚不变。⚠ spin **开**时门关也不再逐位相同：本式在 double 域
 * 乘完一次性收窄，旧式在 float 域乘 —— 方向是少一次舍入，但它不是零行为变化，
 * 记账 → docs/相对坐标地基_plan.md §3 批 4。
 */
export inline glm::mat4 toRelativeModel(const glm::mat4& spin, const glm::dmat4& absolute,
                                        const glm::dvec3& origin) {
	return toRelative(glm::dmat4(spin) * absolute, origin);
}

/**
 * 把「本帧世界坐标」搬回「上一帧世界坐标」的跨帧运动矩阵 = 自旋比 **复合** 两帧渲染原点之差。
 *
 * ## 推导（期望值出处 = 这段闭式，不抄任何被测码）
 * 记第 t 帧的渲染世界坐标 `W_t = S_t·A·p − O_t`，其中 `S_t` = 本帧 spin（绕**绝对原点**的旋转，
 * 锚点裁决 → docs/相对坐标地基_plan.md §3 批 3 末）、`A` = 图元绝对矩阵、`p` = 资产局部点、
 * `O_t` = 本帧渲染原点。于是 `S_t·A·p = W_t + O_t`，左乘 `S_t⁻¹` 得 `A·p = S_t⁻¹·(W_t + O_t)`，
 * 代回第 t−1 帧的世界坐标：
 * ```
 * W_{t−1} = S_{t−1}·A·p − O_{t−1} = R·W_t + (R·O_t − O_{t−1}),   R ≔ S_{t−1}·S_t⁻¹
 * ```
 * ⟹ **motion = T(R·O_t − O_{t−1}) · R**，即「先按 R 转，再平移 `R·O_t − O_{t−1}`」。
 * 复合顺序（先转后移，不是先移后转）由 **spin 的锚点在绝对原点** 决定：相对坐标里那个锚点住在
 * `−O_t`，而绕点 `c` 的旋转是 `x ↦ R·x + (c − R·c)`；取 `c = −O_t` 再补上 `O_t − O_{t−1}`，
 * 化简恰好回到上式。若把 spin 当成绕**渲染原点**（`c = 0`），平移项会退化成 `ΔO` 而与 R 无关 ——
 * 那正是批 3 否掉的形态，本函数不许照它写。
 *
 * ## 三个退化档（逐条有用例钉住）
 *  - **门关**（`O_t = O_{t−1} = 0`）⟹ 平移恰为 0 ⟹ 返回 `spinRatio` **逐位不变**（见下面早退）；
 *  - **静止场景**（`R = I`）⟹ 平移 = `O_t − O_{t−1} = ΔO` ⟹ `W_{t−1} = W_t + ΔO`，
 *    这就是「静止相机 + 移动原点」那条被骗判据的正解（不补这一项 = 静止被判成运动）；
 *  - **原点不动但在自旋**（`O_t = O_{t−1} = O ≠ 0`，`R ≠ I`）⟹ 平移 = `(R − I)·O ≠ 0`。
 *    ⚠ 这一档说明「纯旋转比 `prevSpin·inverse(curSpin)`」在门开态**本来就不够**，不是精度近似问题。
 *
 * ## 🔴 没有绕过 toRelative 自己减原点
 * 本函数**不**做「绝对 − 原点」的减法：它先右乘一个平移 `T(O_t)`（矩阵复合，不是减法），
 * 再把差出来的平移列交给 toRelative 去减 `O_{t−1}`。全仓那一处减法仍然只在那一处
 * （批 3 契约 → docs/相对坐标地基_plan.md §3 批 3）。
 *
 * ## 恒等早退为什么存在（以及它**不**守什么 —— 实测划界）
 * 平移恰为零时直接返回入参，而不是走「复合 + 收窄」那条式子赌它舍回同一个矩阵。
 * 承重的事实是 IEEE 加法语义：`x + 0.0f` 把 `−0.0f` 洗成 `+0.0f`（只有减法 `x − 0.0f`
 * 逐位返回自身），而本函数的复合步用的正是加法（`absolute[3] = r[3] + R·O_t`）。
 * 与 taaSystem 的「门关 = 不调用而不是加 0」同一条纪律：门关逐位不变由**结构**保证，
 * 不靠「这条浮点表达式恰好舍回原值」那种要逐次重验的论证。
 * ⚠ **作用域如实报**（2026-10-25 实测，别把这条读成现网缺陷）：生产入参
 * `prevSpin · glm::inverse(curSpin)` 的那次矩阵乘已经把 `−0.0` 归一成 `+0.0`
 * （20000 组随机旋转比里平移列带 −0.0 的 = 0 组）⟹ 早退当前守的是**函数契约**
 * （本函数不许改动入参里任何一位），不是「现在就在漏 −0.0」。但 `glm::inverse` 单独
 * 是产 `−0.0` 的（一个旋转矩阵的逆，其平移列 4 个分量实测 2 个是 −0.0），少一次乘法
 * 这个输入形态就进来了 —— 所以契约守卫留着，判据在 tests/test_coordinate_rebase.cppm
 * 「恒等早退的专属锚点」那条（删掉早退实测红）。
 * 判据取 `translation`（不是「两个原点是否都为零」）：`T(±0)·R = R` 逐位成立，所以门开态里
 * 原点**没动**的那些帧也走同一条早退，同样精确（且同样不引入新舍入）。
 *
 * @param spinRatio      自旋比 `prevSpin · inverse(curSpin)`（关补偿 / 历史不可用 = 单位阵）
 * @param previousOrigin 上一帧的渲染原点 `O_{t−1}`（TaaState::prevOrigin）
 * @param currentOrigin  本帧的渲染原点 `O_t`（RenderOrigin::position）
 */
export glm::mat4 motionWithOriginShift(const glm::mat4& spinRatio, const glm::dvec3& previousOrigin,
                                       const glm::dvec3& currentOrigin) {
	const glm::dmat4 r{spinRatio};
	// R·O_t：w 分量喂 0 ⟹ 只吃 R 的线性部分（自旋是纯旋转，平移列本应为 0，这里不依赖它）
	const glm::dvec4 rotatedCurrent = r * glm::dvec4(currentOrigin, 0.0);
	const glm::dvec3 translation = glm::dvec3(rotatedCurrent) - previousOrigin;
	if (translation.x == 0.0 && translation.y == 0.0 && translation.z == 0.0)
		return spinRatio;  // 门关 / 原点未动：逐位返回，不引入任何新舍入
	glm::dmat4 absolute{r};
	// absolute = R · T(O_t)：右乘平移只动平移列，其余三列原样（(R·T)[3] = R·(O_t,0) + R[3]）
	absolute[3] = r[3] + rotatedCurrent;
	return toRelative(absolute, previousOrigin);  // 减法仍然只由 toRelative 做那一次
}

/**
 * 湿面图案的**世界锚**（债-152）= 各 entity 根平移的均值（绝对层，逐分量）。
 *
 * ## 为什么需要一枚锚（缺陷本体，钉到行）
 *
 * `rgl/shaders/deferred_geometry.frag.slang` 的水洼掩码 = 低频值噪声，自变量本该是**世界
 * xz**（意图 = 画在场景表面上的图案）。rebase 之后送进着色器的 `P` 是**相对渲染原点**的坐标，
 * 而原点每帧 snap 到级联纹素粒度并跟着相机走 ⟹ 噪声场整片随相机平移（用户肉眼所见）。
 *
 * ## 🔴 为什么不能「把原点加回去」（`q = (P + origin)·freq`）
 *
 * 大坐标工作点上 bistro 的 `freq = puddleFreqRadii / sceneRadius ≈ 10` ⟹ 1e6 处会把 ~1e7
 * 喂进 float32 值噪声：ULP(1e7) ≈ 1 ⟹ **噪声直接被毁**，而且毁在 rebase 专门要支持的那些
 * 坐标上。修法只有一条：图案的自变量换成「世界 − 锚」，只把**渲染原点到该锚的小偏移**传进
 * 着色器（`q = (P + (origin − anchor))·freq`，三项全程是小量）。
 *
 * ## 为什么锚取「entity 根平移的均值」，三个候选的裁决
 *
 * ① **相机无关**（硬要求）：锚若含相机，就是把滑移换个地方藏起来。根平移来自 scene.lua 的
 *   `entities[i].position`，装配期写死、运行期只读 ⟹ 逐帧恒量。
 * ② **出厂工作点上恰为零**（本批红线：门关态三枚截图锚逐位不变，且**没有**重立锚的授权）。
 *   出厂 `entities` 不写 `position` ⟹ 根平移恰 (0,0,0) ⟹ 均值恰 (0,0,0) ⟹ 偏移 = 原点 − 锚
 *   在门关态（原点恒零）是 `0.0 − 0.0` **恰为零** ⟹ 着色器里 `(P + 0)` 与改前逐字同。
 *   候选「场景包围球心」（`SceneBounds.center`）**过不了这条**：它是资产几何的派生量，出厂
 *   bistro 的球心 xz 不保证恰零，一次资产改动就会静默挪动出厂画面。
 * ③ **离场景够近**（精度要求）：偏移 = |origin − anchor| 要小，否则大数又进 float32。远工作点
 *   （整场景 +1e6）下根平移 = 1e6、原点 snap 后 ≈ 1e6 ⟹ 偏移是米量级 ✓；候选 ① 的球心同样
 *   够近，但过不了 ②。
 * 多 entity 场景取**均值**：单 entity（现网全部靶与出厂场景）⟹ 恰等于它自己（`sum/1` 在 IEEE
 * 下精确）；多 entity 时它是各根平移的凸组合 ⟹ 仍落在场景点集包络内，③ 继续成立。
 * 空列表返回 (0,0,0)：无实体可锚（也是「门关逐位不变」要求的那个值）。
 *
 * ⚠ 入参刻意是**裸的根平移列表**而不是 `SceneEntityDesc` 列表：本模块在 rlbase，而场景配置
 * 类型在 rlscene 侧——为了两行求和去 import 配置模块，会让「坐标层的纯数学」拖上 sol2 的编译
 * 前置（分层判据 → AGENTS「引擎四库分层」；调用点 = main 的装配层，它两边都认识）。
 *
 * @param entityRootPositions 各 entity 的根平移（绝对层 double，批 2 第一刀起就是 double）
 */
export glm::dvec3 wetSceneAnchor(std::span<const glm::dvec3> entityRootPositions) {
	if (entityRootPositions.empty())
		return glm::dvec3{0.0};
	glm::dvec3 sum{0.0};
	for (const auto& p : entityRootPositions)
		sum += p;
	return sum / static_cast<double>(entityRootPositions.size());
}

/**
 * 送进着色器的**锚定偏移** = 渲染原点 − 场景锚，取 xz 两分量（湿面图案只吃 xz）。
 *
 * 🔴 减法必须在 double 域做完**最后一步才收窄**（与 `toRelative` 同一条纪律，同一条理由）：
 * 先在 float 里各存一份 1e6 再相减，丢掉的尾数救不回来（ULP(1e6) = 0.0625 m，比水洼噪声
 * 特征尺寸粗两个量级）。收窄到 float32 的是**差**本身，它是米量级的小数，表示精确。
 *
 * 门关态（原点恒 (0,0,0)）+ 出厂场景（锚恰 (0,0,0)）⟹ 返回 `(+0.0f, +0.0f)`，着色器里
 * `P.xz + 0.0` 与改前逐位相同（三枚截图锚的逐位不变性由 `just regress` 与 `imgqa
 * material-swim` 的 J3 共同守）。
 *
 * @param origin 本帧渲染原点（`RenderOrigin::position`，绝对层）
 * @param anchor 场景锚（`wetSceneAnchor` 的产物，绝对层）
 */
export glm::dvec2 wetAnchorOffset(const glm::dvec3& origin, const glm::dvec3& anchor) {
	const glm::dvec3 delta = origin - anchor;  // double 域做差
	return glm::dvec2 { static_cast<float>(delta.x), static_cast<float>(delta.z) };
}

}  // namespace RGL
