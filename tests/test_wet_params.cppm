module;
/**
 * test_wet_params —— Phase 15 批 15.2 子步 A：湿面区两个 vec4 子区的字节布局契约
 * + fillWetParams 填值契约（纯 CPU、无设备）。
 *
 * 湿面区（@722368 身份调制系数 vec4 + @722384 空间变化 vec4）是**纯新增尾区**：既有偏移
 * 逐字节不动（diff0），本步**零渲染消费者**（scene.slang 只备 readWetParams/readWetSpatial
 * 读取函数、无调用点，先例 = 批 15.1 readSsrViewProj「本步无调用点」）。红线 A（出厂画面
 * 逐位不变）由 just regress ab 钉，本文件钉的是**管线本身**：六枚系数从 WetTuning 到
 * scene data 字节的映射正确。
 *
 * ## 钉什么
 *
 * (a) **布局**：按 vkrenderer_impl::writeSceneStaging 的 memcpy 口径把两个 vec4 写进
 *     @722368 / @722384，再按 **shader 侧提取链**（scene.slang readWetParams / readWetSpatial
 *     的 Load4 字面量）读回，断言逐位还原。抓的是**偏移错 / 步长错 / 两子区重叠**。
 * (b) **填值**：wetParams = (strength, dark, roughLo, roughHi)、
 *     wetSpatial = (rippleAmp, puddleFreqRadii, 0, 0)，全部逐位；**两槽不互换**由
 *     「六枚探针值两两不等 + 逐槽钉死 + 跨槽显式互斥」三层保证。
 * (c) **出厂默认值 = 六枚字面量**：`strength` 默认 **1.0** 的断言是「湿面默认开」拍板
 *     （用户，2026-10-10）的钉；默认值唯一出处 = WetTuning 成员初始化器，本测试把它钉成
 *     字面量（写成 WetTuning{} 的字段引用就是"默认值等于默认值"的空断言）。
 * (d) **消融档透传**：strength = 0 原样落槽为 0（CPU 不 clamp——`strength = 0` 是合法
 *     消融档，配置层与填值层都不校验，理由 → readWetBlock 注释）。
 *
 * ## 两源结构（先例 = test_ssr_params / test_ssr_viewproj，理由在那不重复）
 *
 * 写入侧偏移 = C++ 常量 kSceneDataWetParamsOffset / kSceneDataWetSpatialOffset，
 * **读回侧偏移 = 字面量 722368u / 722384u**（手抄 scene.slang 的两个 Load4）。
 * 编译期另有 static_assert 双保险（C++ 常量 == 本文件手抄值）。
 *
 * ⚠ **本测试钉不住 shader 那一侧**（措辞纪律，别让注释把它吹成"两侧钉死"）：
 * `kSlangWetParamsLiteral` / `kSlangWetSpatialLiteral` 是**本文件手抄的 C++ 常量**，不是从
 * scene.slang 提取的。若有人只改 scene.slang 里的 `Load4(722368u)`，本测试与全部
 * static_assert **都不红**。本步零消费者的端到端护栏 = just regress ab（红线 A，画面必须
 * 不动）；15.2B 接消费者后须立湿面专属判据（缺口账与 SSR 同款 → phase15_plan §3.2）。
 *
 * ## 变异红线声明（哪个变异让我红）
 *
 *   ① kSceneDataWetParamsOffset / kSceneDataWetSpatialOffset 改 +16 → **编译期红**（本文件与
 *      scene_data.cppm 的 static_assert 双钉炸编译）；把本文件读回字面量改 722384u →
 *      (a) 红（params 读回的是 spatial 的字节，p != wetParams）。⚠ 单改 scene.slang 的字
 *      面量**不在此覆盖内**（见上）。
 *   ② fillWetParams 两槽互换（wetParams ↔ wetSpatial 整体）或槽内任意两枚互换 → (b) 红
 *      （测试里六枚值两两不等，串位必撞逐槽 CHECK 或跨槽互斥 CHECK）。
 *   ③ WetTuning 默认值被改（哪怕只改一枚一位）→ (c) 红 ⟹ 同时是红线 A 与「默认开」拍板
 *      的测试前预警。
 *   ④ fillWetParams 加 clamp（如 `max(strength, ε)`）或按 strength 把其余系数联动清零 →
 *      (d) 红（消融档必须写得出来、其余五枚不受总开关影响）。
 *   ⑤ 两子区重叠（wetSpatial 偏移写成 wetParams 的值）→ 编译期 static_assert 红
 *      （spatial = params + 16）；运行时形态 = 两个 load4 读到同一份字节 → (a) 的
 *      「两子区读回互不相等」红。
 *   ⑥ writeSceneStaging 用错常量（如误写 kSceneDataSsrParamsOffset）→ 本测试**不红**，与
 *      test_ssr_params 同款已知覆盖缺口（单测复刻 memcpy 而非调用它）；端到端护栏 =
 *      红线 A（本步零消费者 ⟹ just regress ab 逐位不变就是护栏）。
 *
 * 配置解析侧（缺块回落 / 逐键覆盖 / 消融档可写出 / 无扁平键双读）不在本文件——那是 loader
 * 契约，见 test_scene_config.cppm 的 wet 块 roundtrip 用例。
 */
#include <doctest/doctest.h>
#include <cstring>
#include <glm/glm.hpp>

export module test_wet_params;

import rlstd;
import render_list;      // fillWetParams（可测接缝，fillSsrParams 同款先例）
import frame_draw_list;  // FrameDrawList / SceneFrameData.wetParams / wetSpatial
import runtime_config;   // WetTuning（默认值唯一出处）
import scene_data;       // kSceneDataWet*Offset / kSceneDataWetBytes / kSceneDataBytes

namespace RGL {
namespace {

/// scene.slang readWetParams 的字面量基址（两源结构的 shader 侧镜像，勿改成常量引用）。
constexpr uint32_t kSlangWetParamsLiteral = 722368u;
/// scene.slang readWetSpatial 的字面量基址（同上）。
constexpr uint32_t kSlangWetSpatialLiteral = 722384u;

// 编译期双保险：C++ 常量与两个 shader 字面量同钉（变异①在此炸）。
static_assert(kSceneDataWetParamsOffset == kSlangWetParamsLiteral,
	"scene.slang readWetParams literal drifted from kSceneDataWetParamsOffset");
static_assert(kSceneDataWetSpatialOffset == kSlangWetSpatialLiteral,
	"scene.slang readWetSpatial literal drifted from kSceneDataWetSpatialOffset");
// 子区排布：wetParams 居湿面区首、wetSpatial 紧随其后，wetSpatial 恰盖到
// kSceneDataBytes 尾（湿面区是最后一个尾区，无重叠无空洞）；区宽 = 2×vec4。
static_assert(kSceneDataWetParamsOffset == kSceneDataWetOffset,
	"params sub-region must sit at the head of the wet region");
static_assert(kSceneDataWetSpatialOffset == kSceneDataWetParamsOffset + 16u,
	"spatial sub-region must sit right after wetParams (no overlap, no hole)");
static_assert(kSceneDataWetSpatialOffset + sizeof(glm::vec4) == kSceneDataBytes,
	"spatial sub-region must end exactly at the scene data buffer tail");
static_assert(kSceneDataWetOffset + kSceneDataWetBytes == kSceneDataBytes,
	"wet region must be the last tail region of the scene data buffer");
static_assert(kSceneDataWetBytes == 32u, "wet region width drifted (vec4+vec4)");

/// shader `sceneData.Load4(byteOffset)` 等价形态（test_ssr_params 同款：真 memcpy）。
glm::vec4 load4(const std::vector<std::byte>& buf, uint32_t byteOffset) {
	glm::vec4 v;
	std::memcpy(&v, buf.data() + byteOffset, sizeof(v));
	return v;
}

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

TEST_CASE("湿面两个 vec4 子区布局：C++ 常量写、字面量 722368/722384 读回逐位相同（两源结构）") {
	const WetTuning tuning = distinctTuning();

	FrameDrawList list;
	fillWetParams(list, tuning);

	// staging 写入：vkrenderer_impl::writeSceneStaging 同款 memcpy（偏移 = C++ 常量）。
	// 尾部 +16B 垫 = 变异①运行时形态（读回字面量 +16 单侧漂）的读取范围，避免越界 UB。
	std::vector<std::byte> stage(kSceneDataWetSpatialOffset + 2 * sizeof(glm::vec4),
		std::byte{0});
	std::memcpy(stage.data() + kSceneDataWetParamsOffset, &list.sceneData.wetParams,
		sizeof(glm::vec4));
	std::memcpy(stage.data() + kSceneDataWetSpatialOffset, &list.sceneData.wetSpatial,
		sizeof(glm::vec4));

	// shader 侧读回：字面量（两源结构）。
	const glm::vec4 p = load4(stage, kSlangWetParamsLiteral);
	const glm::vec4 s = load4(stage, kSlangWetSpatialLiteral);
	CHECK(p == list.sceneData.wetParams);  // 提取链恒等（步长/偏移错在此红）
	CHECK(s == list.sceneData.wetSpatial);
	CHECK(p.x != 0.0f);                    // 缓冲没读成全 0（变异①运行时形态的判据前提）
	CHECK(s.x != 0.0f);
	// 两子区互不重叠：读到同一份字节 ⟹ 变异⑤（wetSpatial 偏移写成 wetParams 的值）。
	CHECK(p != s);
}

TEST_CASE("fillWetParams：六枚系数逐位落槽、两槽不互换（串位变异必须改变可观察字节）") {
	const WetTuning tuning = distinctTuning();

	FrameDrawList list;
	fillWetParams(list, tuning);

	// wetParams = (strength, dark, roughLo, roughHi)，float 直传无换算。
	CHECK(list.sceneData.wetParams.x == tuning.strength);
	CHECK(list.sceneData.wetParams.y == tuning.dark);
	CHECK(list.sceneData.wetParams.z == tuning.roughLo);
	CHECK(list.sceneData.wetParams.w == tuning.roughHi);
	// wetSpatial = (rippleAmp, puddleFreqRadii, 0, 0)；zw 预留必须恒 0。
	CHECK(list.sceneData.wetSpatial.x == tuning.rippleAmp);
	CHECK(list.sceneData.wetSpatial.y == tuning.puddleFreqRadii);
	CHECK(list.sceneData.wetSpatial.z == 0.0f);
	CHECK(list.sceneData.wetSpatial.w == 0.0f);

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

TEST_CASE("fillWetParams：strength=0 消融档原样落槽为 0、其余五枚不受联动（CPU 不 clamp，变异④）") {
	// `strength = 0` 是**合法消融档**（湿面关 = 因果泄漏恒等式的门关档），配置层与填值层
	// 都不校验不 clamp（理由 → readWetBlock 注释）。变异④：填值层加 `max(strength, ε)` 或
	// 按总开关把其余系数联动清零 ⟹ 本条红（消融档必须写得出来，且只关"湿面调制"本身）。
	WetTuning t = distinctTuning();
	t.strength = 0.0f;

	FrameDrawList list;
	fillWetParams(list, t);

	CHECK(list.sceneData.wetParams.x == 0.0f);   // 0 不是"缺省"哨兵，写 0 就得是 0
	// 其余五枚**原样透传**（不随总开关清零——联动清零 = 消融档改写了别的旋钮的语义）。
	CHECK(list.sceneData.wetParams.y == 0.25f);  // dark
	CHECK(list.sceneData.wetParams.z == 0.11f);  // roughLo
	CHECK(list.sceneData.wetParams.w == 0.44f);  // roughHi
	CHECK(list.sceneData.wetSpatial.x == 0.77f);         // rippleAmp
	CHECK(list.sceneData.wetSpatial.y == 3.25f);         // puddleFreqRadii
}

}  // namespace RGL
