module;
/**
 * test_wet_params —— Phase 15 批 15.2 子步 A：湿面区两个 vec4 子区的字节布局契约
 * + fillWetParams 填值契约（纯 CPU、无设备）。
 *
 * 湿面区（@723968 身份调制系数 vec4 + @723984 空间变化 vec4）是批 15.2A 的纯新增尾区；
 * 消费者 = deferred_geometry 湿面 G-buffer 调制（15.2B 已接线，默认开）。消费端载体自
 * **scene data 迁移批 1** 起 = scene.slang `ConstantBuffer<WetParamsBlock> sceneWet`
 * （binding 32 子范围 UBO 描述符，shader 侧零字节字面量）。红线 A（出厂画面逐位不变）
 * 由锚点截图钉（skill rgl-verify 口径），本文件钉的是**管线本身**：六枚系数从 WetTuning
 * 到 scene data 字节的映射正确。
 *
 * ## 钉什么
 *
 * (a) **布局**：按 vkrenderer_impl::writeSceneStaging 的 memcpy 口径把两个 vec4 写进
 *     湿面区两个子区，再按 **shader 侧提取链**（scene.slang `sceneWet.params` /
 *     `sceneWet.spatial` = 描述符基址 kSceneDataWetOffset + struct 字段偏移，本文件手抄
 *     为 0u / 16u）读回，断言逐位还原。抓的是**字段偏移错 / 步长错 / 两子区重叠**。
 * (b) **填值**：wetParams = (strength, dark, roughLo, roughHi)、
 *     wetSpatial = (rippleAmp, puddleFreqRadii, 0, 0)，全部逐位；**两槽不互换**由
 *     「六枚探针值两两不等 + 逐槽钉死 + 跨槽显式互斥」三层保证。
 * (c) **出厂默认值 = 六枚字面量**：`strength` 默认 **1.0** 的断言是「湿面默认开」拍板
 *     （用户，2026-10-10）的钉；默认值唯一出处 = WetTuning 成员初始化器，本测试把它钉成
 *     字面量（写成 WetTuning{} 的字段引用就是"默认值等于默认值"的空断言）。
 * (d) **消融档透传**：strength = 0 原样落槽为 0（CPU 不 clamp——`strength = 0` 是合法
 *     消融档，配置层与填值层都不校验，理由 → readWetBlock 注释）。
 *
 * ## 两源结构（迁移批 1 换形 → 收口后**补做写侧**再换一次形）
 *
 * 🔴 **批 1 当时只换了读侧**，写端留了两次 memcpy + 两个子区常量
 * （`kSceneDataWetParamsOffset` / `kSceneDataWetSpatialOffset = 区首 + 16u`），而本文件头
 * 旧版声称的"写侧子区 == 生成 struct 字段偏移**双钉**"里 **第二钉实际不存在**——测试钉的是
 * `子区B == 子区A + 16u`，字面量对字面量，与契约 `offsetof` 没有任何连线。收口后补做：
 * 写端按契约 `WetParamsBlock` **整块写**，两个子区常量删除 ⟹ "spatial 在区内第 16 字节"
 * 这个事实只剩反射一处，同步链从三段缩到两段：
 *   ① scene.slang struct ↔ scene_forward_schema.lua 声明 = `just reflect` 期 validator
 *      交叉核对（vertexLayout vs SPV 反射逐字段，tools/validator.cppm；构建开头 SHA1
 *      护栏强制改 shader 必重跑 reflect）；
 *   ② schema → 生成物 WetParamsBlock（scene_forward_types_generated）自带 offsetof /
 *      sizeof static_assert。
 * 本文件剩下的钉 = ③ 生成物 ↔ 本文件手抄字段偏移（下面的 offsetof static_assert）+ 区宽
 * == `sizeof(WetParamsBlock)`（区宽 = 子范围描述符 range，必须等于契约宽）。
 * 🔴 **写端位置不再需要钉**——它不携带位置信息了。这才是"消除双源"和"给双源加护栏"的区别。
 *
 * ⚠ 措辞纪律：kSlangWetParamsFieldOffset / kSlangWetSpatialFieldOffset 仍是**本文件手抄
 * 常量**（钉的是「手抄 == 契约生成物」）；scene.slang 那一侧由 ① 的 reflect 期核对钉，
 * 不在本测试进程内。端到端护栏 = 红线 A 锚 + 影响域判据（phase15_plan §3.2）。
 *
 * ## 变异红线声明（哪个变异让我红）
 *
 *   ① 本文件手抄字段偏移单侧漂（0u/16u 改 +16）→ **编译期红**（offsetof static_assert 炸
 *      编译）；改 scene.slang struct 字段序/类型 → reflect 期 validator 交叉核对红（①段
 *      链，构建 SHA1 护栏逼重跑）；只改 schema 不改 shader → 同上红；schema+shader 同步
 *      换序 → 生成物字段偏移变 ⟹ 本文件 ③ 段钉红。⚠ 改 kSceneDataWetOffset（整区搬家）
 *      **不该红**——写侧 memcpy 与 binding 32 描述符基址**同源同值**，画面逐位不变
 *      （字面量第二源已删，这正是迁移批 1 的目的）。
 *   ② fillWetParams 两槽互换（wetParams ↔ wetSpatial 整体）或槽内任意两枚互换 → (b) 红
 *      （测试里六枚值两两不等，串位必撞逐槽 CHECK 或跨槽互斥 CHECK）。
 *   ③ WetTuning 默认值被改（哪怕只改一枚一位）→ (c) 红 ⟹ 同时是红线 A 与「默认开」拍板
 *      的测试前预警。
 *   ④ fillWetParams 加 clamp（如 `max(strength, ε)`）或按 strength 把其余系数联动清零 →
 *      (d) 红（消融档必须写得出来、其余五枚不受总开关影响）。
 *   ⑤ 两子区重叠（wetSpatial 偏移写成 wetParams 的值）→ 编译期 static_assert 红
 *      （spatial = params + 16）；运行时形态 = 两个 load4 读到同一份字节 → (a) 的
 *      「两子区读回互不相等」红。
 *   ⑥ writeSceneStaging 用错基址（如误写 SSR 区基址）→ 本测试**不红**，与
 *      test_ssr_params 同款已知覆盖缺口（单测复刻 memcpy 而非调用它）；端到端护栏 =
 *      红线 A 锚（出厂画面逐位）+ imgqa 影响域判据（skill rgl-verify）。
 *   ⑦（债-152）锚定偏移改回吃相对坐标（`fillWetParams` 里把 origin/anchor 传成两个 0，
 *      或 shader 侧退回 `P.xz * freq`）→ 本文件的 (e)「原点==锚 ⟹ zw 恰 +0」与 (f)「远
 *      工作点偏移是小量且 double 域做差」两条红；⚠ 但**图像级**的红由 `imgqa material-swim`
 *      的 J1/J4 抓（本文件只钉 CPU 侧映射，钉不到 shader 有没有真用它）。
 *   ⑧（债-152）偏移符号写反（`anchor − origin`）→ (b) 红（探针差值 0.625 vs −0.625）。
 *   ⑨（债-152）减法先在 float 里做（`float(origin) − float(anchor)`）→ (f) 的阳性对照红。
 *
 * 配置解析侧（缺块回落 / 逐键覆盖 / 消融档可写出 / 无扁平键双读）不在本文件——那是 loader
 * 契约，见 test_scene_config.cppm 的 wet 块 roundtrip 用例。
 */
#include <doctest/doctest.h>
#include <cmath>      // signbit / abs（锚定偏移的符号与「小量」钉，债-152）
#include <cstddef>    // offsetof（契约生成物字段偏移双钉）
#include <cstring>
#include <glm/glm.hpp>

export module test_wet_params;

import rlstd;
import render_list;      // fillWetParams（可测接缝，fillSsrParams 同款先例）
import frame_draw_list;  // FrameDrawList / SceneFrameData.wetParams / wetSpatial
import runtime_config;   // WetTuning（默认值唯一出处）
import scene_data;       // kSceneDataWetOffset / kSceneDataWetBytes / kSceneRegionAlign
import scene_forward_types_generated;  // WetParamsBlock（契约生成物，③ 段钉的对象）

namespace RGL {
namespace {

/// scene.slang `sceneWet.params` 的 struct 字段偏移（两源结构的 shader 侧镜像：手抄
/// scene.slang WetParamsBlock 声明顺序，勿改成常量引用——镜像失手抄价值即失效）。
constexpr uint32_t kSlangWetParamsFieldOffset = 0u;
/// scene.slang `sceneWet.spatial` 的 struct 字段偏移（同上）。
constexpr uint32_t kSlangWetSpatialFieldOffset = 16u;

// 编译期双保险（两源链 ③ 段）：手抄字段偏移 == 契约生成物字段偏移、区宽 == struct 宽
//（变异①在此炸；①② 段链 = reflect 期 validator 核对 + 生成物自带 assert，见文件头）。
static_assert(kSlangWetParamsFieldOffset == offsetof(WetParamsBlock, params),
	"scene.slang WetParamsBlock.params field offset drifted from hand-copied mirror");
static_assert(kSlangWetSpatialFieldOffset == offsetof(WetParamsBlock, spatial),
	"scene.slang WetParamsBlock.spatial field offset drifted from hand-copied mirror");
static_assert(sizeof(WetParamsBlock) == kSceneDataWetBytes,
	"WetParamsBlock width drifted from kSceneDataWetBytes (sub-range descriptor range)");
// 子区排布（迁移批复补后：写侧的两个子区常量**已删除**，"spatial 在区内第 16 字节"这个
// 事实只剩契约 `WetParamsBlock` 字段序一处 ⟹ 这里只钉手抄镜像本身自洽 + 区宽 + 相邻关系，
// 不再钉"写侧子区常量 == 区首/区首+16"（那种钉是字面量对字面量，见文件头 ⚠）。
//（簇化轴 ★1-a1i 在湿面之后追加 `ObjectClusterRange` 尾区 ⟹「湿面区是最后一个尾区」的旧断言
//  改钉相邻关系，照 test_ssr_params 批 15.2 的同款处置；无重叠无空洞。尾区末尾断言 =
//  tests/test_meshlet_ranges）；区宽 = 2×vec4。
static_assert(kSlangWetParamsFieldOffset == 0u,
	"params must sit at the head of the wet region（字段序是唯一位置来源）");
static_assert(kSlangWetSpatialFieldOffset == kSlangWetParamsFieldOffset + sizeof(glm::vec4),
	"spatial must sit right after params (no overlap, no hole)");
// 批 L（§9.7）后相邻不变式 = 「下一区基址 = alignUp(前区尾, 256)」：无重叠 + 间隙 < 256
//（对齐间隙无常驻内容，writeClusterRangeRegion/本文件写侧都不越出本区字节面）。
static_assert(alignUp(kSceneDataWetOffset + kSceneDataWetBytes, kSceneRegionAlign) ==
		kSceneDataClusterRangeOffset,
	"wet region must be followed by the cluster-range region at the next 256 step");
static_assert(kSceneDataWetBytes == sizeof(WetParamsBlock),
	"wet region width drifted (must equal the contract struct, = sub-range descriptor range)");

/// shader `sceneData.Load4(byteOffset)` 等价形态（test_ssr_params 同款：真 memcpy）。
glm::vec4 load4(const std::vector<std::byte>& buf, uint32_t byteOffset) {
	glm::vec4 v;
	std::memcpy(&v, buf.data() + byteOffset, sizeof(v));
	return v;
}

/**
 * 非退化**锚定探针**（债-152 起 wetSpatial.zw 有主，不再是恒 0 的保留位）。
 *
 * 挑法与六枚系数同一条：两分量互不相同、与六枚 tuning 系数两两不等（跨槽串位必须改变
 * 可观察字节）、都不是 0（"漏填偏移"与"填了 0"要可区分）、且**二进制精确可表示**
 * （1.125 / 0.5 / 2.25 / 0.25 都是 2 的幂组合 ⟹ double 做差再收窄 float 全程无舍入，
 * 断言能钉到逐位，而不是钉一个 `Approx`）。
 * 差值 = (0.625, −2.5)。
 */
constexpr glm::dvec3 kProbeOrigin { 1.125, 0.0, -2.25 };  ///< 渲染原点（绝对层）
constexpr glm::dvec3 kProbeAnchor { 0.5, 0.0, 0.25 };     ///< 场景锚（绝对层）
constexpr float kProbeOffsetX = 0.625f;                   ///< = origin.x − anchor.x（精确）
constexpr float kProbeOffsetZ = -2.5f;                    ///< = origin.z − anchor.z（精确）

/**
 * 非退化 tuning：六枚系数**两两互不相同，且都不等于任何出厂默认值**。
 *
 * 为什么这么挑：变异②（串位/两槽互换）要求"填错槽"必须改变可观察字节。若系数之间有
 * 相等者，swap 两个槽可能读回同一个数（假绿）。两两不等且都不是 0（0 会让"漏填"和
 * "填了 0"不可区分）。
 */
WetTuning distinctTuning() {
	WetTuning t;
	t.strength = 1.5f;
	t.dark = 0.25f;
	t.roughLo = 0.11f;
	t.roughHi = 0.44f;
	t.rippleAmp = 0.77f;
	t.puddleFreqRadii = 3.25f;
	return t;
}

}  // namespace

TEST_CASE("湿面两个 vec4 子区布局：契约 struct 整块写、基址 + 手抄字段偏移 0/16 读回逐位相同") {
	const WetTuning tuning = distinctTuning();

	FrameDrawList list;
	// 🔴 传**非零**锚定探针（债-152 起 wetSpatial.zw 承载真数据）：zw 恒 0 时「后 8 个字节
	// 被写坏/读错」在这条布局测试里看不出来。喂非零偏移后，子区重叠 / 步长错 / 区宽错都会
	// 落到可观察字节上（这条测试的射程因此比改前宽 8 字节）。
	fillWetParams(list, tuning, kProbeOrigin, kProbeAnchor);

	// staging 写入：vkrenderer_impl::writeSceneStaging 同款（迁移批复补后 = 按契约 struct
	// 整块写，写侧不再有子区常量）。尾部 +16B 垫 = 变异①运行时形态（读回字段偏移 +16
	// 单侧漂）的读取范围，避免越界 UB。
	std::vector<std::byte> stage(kSceneDataWetOffset + kSceneDataWetBytes + 16u, std::byte{0});
	const WetParamsBlock wetBlock {
		.params = list.sceneData.wetParams, .spatial = list.sceneData.wetSpatial };
	std::memcpy(stage.data() + kSceneDataWetOffset, &wetBlock, sizeof(wetBlock));

	// shader 侧读回：binding 32 子范围描述符基址（= kSceneDataWetOffset，C++ 写描述符
	// 同源）+ 手抄字段偏移（两源结构）。
	const glm::vec4 p = load4(stage, kSceneDataWetOffset + kSlangWetParamsFieldOffset);
	const glm::vec4 s = load4(stage, kSceneDataWetOffset + kSlangWetSpatialFieldOffset);
	CHECK(p == list.sceneData.wetParams);  // 提取链恒等（步长/偏移错在此红）
	CHECK(s == list.sceneData.wetSpatial);
	CHECK(p.x != 0.0f);                    // 缓冲没读成全 0（变异①运行时形态的判据前提）
	CHECK(s.x != 0.0f);
	// 两子区互不重叠：读到同一份字节 ⟹ 变异⑤（wetSpatial 偏移写成 wetParams 的值）。
	CHECK(p != s);
}

TEST_CASE("fillWetParams：六枚系数 + 锚定偏移逐位落槽、两槽不互换（串位变异必须改变可观察字节）") {
	const WetTuning tuning = distinctTuning();

	FrameDrawList list;
	fillWetParams(list, tuning, kProbeOrigin, kProbeAnchor);

	// wetParams = (strength, dark, roughLo, roughHi)，float 直传无换算。
	CHECK(list.sceneData.wetParams.x == tuning.strength);
	CHECK(list.sceneData.wetParams.y == tuning.dark);
	CHECK(list.sceneData.wetParams.z == tuning.roughLo);
	CHECK(list.sceneData.wetParams.w == tuning.roughHi);
	// wetSpatial = (rippleAmp, puddleFreqRadii, 锚定偏移 x, 锚定偏移 z)。
	// 🔴 zw 自债-152 起**有主**（此前是恒 0 的保留位，本测试当时钉的正是「zw 必须 0」)。
	// 现在 zw 承载「渲染原点 − 场景锚」，本条钉的是**映射**：z ← 差的 x、w ← 差的 z（不是
	// x/y 也不是 z/w 的抄错），且差值逐位精确（探针全是二进制精确数，见 kProbeOrigin 注释）。
	// 变异：z/w 互换 → 本条红；把 anchor 加回去（符号写反）→ 本条红（0.625 vs −0.625）。
	CHECK(list.sceneData.wetSpatial.x == tuning.rippleAmp);
	CHECK(list.sceneData.wetSpatial.y == tuning.puddleFreqRadii);
	CHECK(list.sceneData.wetSpatial.z == kProbeOffsetX);
	CHECK(list.sceneData.wetSpatial.w == kProbeOffsetZ);

	// **两槽不互换**的显式句：distinctTuning 让六枚值两两不等 ⟹ 任何跨槽串位都会让
	// 「本槽某个值等于另一槽的某个值」。逐槽 CHECK 已钉死映射，这里把"另一槽的值不许
	// 出现在本槽"写在明面上，而不是让读者反推（变异②：两槽整体互换在此红）。
	const float paramsSlots[4] = {
		list.sceneData.wetParams.x, list.sceneData.wetParams.y,
		list.sceneData.wetParams.z, list.sceneData.wetParams.w,
	};
	const float spatialSlots[4] = {
		list.sceneData.wetSpatial.x, list.sceneData.wetSpatial.y,
		list.sceneData.wetSpatial.z, list.sceneData.wetSpatial.w,
	};
	// 🔴 八枚（六系数 + 两枚偏移）必须**两两不等**，否则下面的跨槽互斥检查有盲区（探针值
	// 相等时串位读回同一个数 = 假绿）。当场出声，而不是靠注释里的"我挑过了"。
	for (std::size_t i = 0; i < 4; ++i)
		for (std::size_t k = 0; k < 4; ++k) {
			INFO("探针退化：wetParams." << i << " == wetSpatial." << k);
			CHECK(paramsSlots[i] != spatialSlots[k]);
		}
	const char* paramsNames[4] = { "wetParams.x", "wetParams.y", "wetParams.z", "wetParams.w" };
	for (std::size_t i = 0; i < 4; ++i) {
		for (std::size_t k = 0; k < 4; ++k) {
			INFO(paramsNames[i] << " == wetSpatial[" << k << "]（跨槽串位）");
			CHECK(paramsSlots[i] != spatialSlots[k]);
		}
	}
}

TEST_CASE("WetTuning 出厂默认值 = 六枚字面量（「湿面默认开」拍板的钉 + 红线 A 前提，变异③）") {
	// 字面量与 configs/runtime.lua `wet` 块、docs/phase15_plan.md §2.4 批次表同源。
	// **这里必须是字面量**：写成 WetTuning{} 的字段引用就成了"默认值等于默认值"的空断言。
	// 任一默认值被改 ⟹ 本条红 ⟹ 出厂配置的输出不再逐位等于批 15.2A（红线 A 破）；
	// strength 那一枚同时是「湿面默认开、15.2B 重立红线 A」拍板的钉。
	const WetTuning d;
	CHECK(d.strength == 1.0f);           // 总开关兼强度：**默认开**（用户拍板，不许悄悄翻成 0）
	CHECK(d.dark == 0.6f);               // 湿区 albedo 压暗系数（×0.5–0.7 区间取中）
	CHECK(d.roughLo == 0.05f);           // 水洼粗糙度
	CHECK(d.roughHi == 0.35f);           // 湿石粗糙度
	CHECK(d.rippleAmp == 0.3f);          // 水膜法线扰动幅度
	CHECK(d.puddleFreqRadii == 12.0f);   // 水洼噪声频率（/ sceneRadius，尺度无关）

	// shader 侧不变式在默认值下必须成立（越界不钳制，但出厂值没理由越界）：
	// 水洼必须比湿石更光滑，反了 = 空间调制方向倒置。默认值被改成越界 ⟹ 本条红。
	CHECK(d.roughLo < d.roughHi);
	// 出厂默认值必须**全非零**：FrameDrawList 的两个 vec4 零初始化，若某个默认系数也是 0，
	// "漏填"（fillWetParams 没被调用）在默认配置下与正确填法不可区分——本文件断言仍绿而
	// 数据已错。六枚全非零 ⟹ 默认配置下"漏填"必然读回 0 ≠ 默认值，可区分。
	CHECK(d.strength != 0.0f);
	CHECK(d.dark != 0.0f);
	CHECK(d.roughLo != 0.0f);
	CHECK(d.roughHi != 0.0f);
	CHECK(d.rippleAmp != 0.0f);
	CHECK(d.puddleFreqRadii != 0.0f);
}

TEST_CASE("fillWetParams：strength=0 消融档原样落槽为 0、其余五枚与锚定偏移不受联动（CPU 不 clamp，变异④）") {
	// `strength = 0` 是**合法消融档**（湿面关 = 因果泄漏恒等式的门关档），配置层与填值层
	// 都不校验不 clamp（理由 → readWetBlock 注释）。变异④：填值层加 `max(strength, ε)` 或
	// 按总开关把其余系数联动清零 ⟹ 本条红（消融档必须写得出来，且只关"湿面调制"本身）。
	// 债-152 把这条的射程扩到 zw：锚定偏移**也不许**被总开关联动清零（它不是调参旋钮，
	// 而且"strength=0 时顺手不传偏移"会让消融档与出厂档在图案上分叉 = 逐位对照失效）。
	WetTuning t = distinctTuning();
	t.strength = 0.0f;

	FrameDrawList list;
	fillWetParams(list, t, kProbeOrigin, kProbeAnchor);

	CHECK(list.sceneData.wetParams.x == 0.0f);   // 0 不是"缺省"哨兵，写 0 就得是 0
	// 其余五枚**原样透传**（不随总开关清零——联动清零 = 消融档改写了别的旋钮的语义）。
	CHECK(list.sceneData.wetParams.y == 0.25f);  // dark
	CHECK(list.sceneData.wetParams.z == 0.11f);  // roughLo
	CHECK(list.sceneData.wetParams.w == 0.44f);  // roughHi
	CHECK(list.sceneData.wetSpatial.x == 0.77f);         // rippleAmp
	CHECK(list.sceneData.wetSpatial.y == 3.25f);         // puddleFreqRadii
	CHECK(list.sceneData.wetSpatial.z == kProbeOffsetX);  // 锚定偏移 x（不随 strength 清零）
	CHECK(list.sceneData.wetSpatial.w == kProbeOffsetZ);  // 锚定偏移 z（同上）
}

TEST_CASE("fillWetParams：原点 == 锚（出厂 + 门关）⟹ wetSpatial.zw 恰为 +0.0f（三枚截图锚逐位不变的前提）") {
	// 🔴 这条是「门关态交付图逐位不变」在 CPU 侧的对应物，也是本批**没有**重立截图锚授权下
	// 唯一能提前钉住它的地方：着色器算 `q = (P.xz + ws.zw) * freq`，只有 zw **恰为 +0.0f**
	// 时它才与改前的 `P.xz * freq` 逐位相同。
	// 为什么必须钉到「+0 而不是 −0」：`-0.0f` 与 `+0.0f` 相加/相乘在 IEEE754 下给出不同符号
	// 的零，而 wetFbm 的 `floor`/`frac` 只吃数值不吃符号（实测两态截图逐位相同），所以符号
	// 本身不是判据；但把「恰为 +0」钉死能挡住"偏移走了 abs/取负再相加"这类改写（它会把
	// 一个真偏移悄悄变成 0）。
	FrameDrawList list;
	fillWetParams(list, distinctTuning(), glm::dvec3 { 0.0 }, glm::dvec3 { 0.0 });
	CHECK(list.sceneData.wetSpatial.z == 0.0f);
	CHECK(list.sceneData.wetSpatial.w == 0.0f);
	CHECK(!std::signbit(list.sceneData.wetSpatial.z));  // 出厂门关：0.0 − 0.0 = +0
	CHECK(!std::signbit(list.sceneData.wetSpatial.w));

	// 原点与锚**相等但非零**（门开态里相机恰好停在锚上）⟹ 同样恰为 +0（幂等于出厂）。
	FrameDrawList same;
	fillWetParams(same, distinctTuning(), glm::dvec3 { 1e6, 0.75, -3.5 },
		glm::dvec3 { 1e6, 0.0, -3.5 });
	CHECK(same.sceneData.wetSpatial.z == 0.0f);
	CHECK(same.sceneData.wetSpatial.w == 0.0f);
}

TEST_CASE("fillWetParams：远工作点（1e6）下偏移是**小量**且逐位精确（🔴 不许先在 float 里做差）") {
	// 债-152 的修法只有一条：图案自变量换成「世界 − 锚」，传进着色器的只是「渲染原点到该锚」
	// 的**小偏移**。这条钉的就是"小"与"准"两件事，两者都不是显然的：
	//  · 准：减法在 double 域做（`wetAnchorOffset`），1e6 处的 0.0625 m 差值能表示；先在
	//    float 里各存一份 1e6 再相减，ULP(1e6) = 0.0625 ⟹ 差值直接塌成 0 或跳一格。
	//  · 小：偏移量级 = 米，而不是 1e6 ⟹ 着色器里 (P + offset)·freq 全程小量，噪声不被毁。
	//    反例（任务书明令禁止的修法）= 把原点加回去：q 会吃到 ~1e7，ULP(1e7) ≈ 1。
	FrameDrawList list;
	// 0.03125 = 2⁻⁵ = **半个** float32 在 1e6 处的 ULP（ULP(1e6) = 2⁻⁴ = 0.0625）⟹ 它在
	// double 里精确、在 float32 的 1e6 附近**表示不出来**，正是"顺序错了就丢"的那个量。
	fillWetParams(list, distinctTuning(), glm::dvec3 { 1e6 + 0.03125, 0.75, 1e6 - 0.03125 },
		glm::dvec3 { 1e6, 0.0, 1e6 });
	CHECK(list.sceneData.wetSpatial.z == 0.03125f);   // 先在 float 里做差 ⟹ 0.0（本条红）
	CHECK(list.sceneData.wetSpatial.w == -0.03125f);  // 同上
	CHECK(std::abs(list.sceneData.wetSpatial.z) < 1.0f);  // 「小量」本身也钉一条
	// 阳性对照：把同样的数先在 float 里过一道再相减，结果**必须不同**——证明本用例真的
	// 走在 double 域（否则换成 float 实现也照样绿 = 空断言，同 toRelative 单测的同款手法）。
	const float driftFloatOrder = float(1e6 + 0.03125) - float(1e6);
	CHECK(driftFloatOrder != list.sceneData.wetSpatial.z);
}

}  // namespace RGL
