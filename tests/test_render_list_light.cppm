module;
/**
 * test_render_list_light —— fillLightData 落槽契约（批 12.3d-A）
 *
 * 为什么值得测：灯数组落槽是 CPU(ECS) → scene data 字节 → 延迟 light pass 的交接点。
 * 契约核心 = **按 Light.index 落槽，不依赖 view 迭代序**（entt 迭代序无保证；一旦被
 * 改回"迭代序落槽"，前向头单灯与延迟灯数组会随 entity 增删悄悄换序——无报错、
 * 只有画面慢慢不对，正是本项目最贵的一类 bug）。
 *
 * 接缝（12.3d）：fillLightData 导出到接口，registry 栈上构造直打（entt 非模块化，
 * GMF #include <entt/entt.hpp>，main 同款用法；include 面经 rlbase PUBLIC EnTT::EnTT
 * 传递到测试 target）。
 *
 * ## 以源码为准的两处行为（与指令预期的差异如实记录）
 *   ① 无灯时 primary = 默认 Light{}，其 direction 默认值 **(0.35,-1,0.25) 非零** →
 *     glm::normalize 不产生 NaN；头单灯字段是"默认灯换算值"而非全零。
 *   ② index ≥ kMaxLights 的 entity **计入 lightEntityCount 但不占槽** →
 *     lightCount 可 > 实际写入的槽数；空出的尾槽回落**默认 Light{} 的换算值**
 *     （源码语义：lightSlot 默认构造 + 逐槽无条件换算），本文件把它钉成断言。
 *
 * ## 变异红线声明（哪个变异让我红）
 *   乱序用例     ：`lightSlot[L.index]` → `lightSlot[lightEntityCount-1]`（迭代序落槽，
 *                  被禁止的形态）→ lightArray[0] 变 index2 灯 → 红；primary = index1 灯
 *                  → 头单灯断言红
 *   头单灯用例   ：删 normalize → 方向向量模长 != 1 锚 + expectNear(normalize) 红；
 *                  删 color×intensity 预乘 → expectNear 红（intensity ≠ 1）
 *   计数用例     ：index≥kMax 早退跳计（把 ++ 挪进 if 内）→ lightCount 0 ≠ 1 红；
 *                  min 删掉 → lightCount 9 > kMaxLights 红（越界语义）
 *   重复 index   ：同上迭代序变异 → lightArray[1] 被 v2 占 → "默认灯"断言红
 *   params 用例  ：range/type 交换 → 点光灯（type=1）params.x==5.5 红
 *   无灯用例     ：primary{} 改成值残差（如漏初始化）→ 默认值逐字段断言红
 *
 * 迭代序前提：entt 的 view 迭代序**无保证**（本仓 entt 3.16 实测**不是**创建序）。
 * 契约（按 index 落槽）不依赖它；"重复 index 后写覆盖先写"的期望值取自同一 view
 * 再迭代取尾（与实现同序），不假设顺序。乱序落位用例在任意迭代序下都成立，
 * 但变异验红（lightSlot[index] → lightSlot[count-1] 必红）要求创建序 ≠ index 序，
 * 创建顺序 {2,0,1} 在正反两种迭代序下都满足。
 */
#include <doctest/doctest.h>
#include <initializer_list>  // 必须：modules 下 std::initializer_list 不从 import 带进来
#include <cmath>
#include <entt/entt.hpp>     // entt 非模块化（main 同款 GMF include；include 面随 rlbase PUBLIC 传递）
#include <glm/glm.hpp>

export module test_render_list_light;

import rlstd;
import test_support;
import render_list;            // fillLightData（12.3d 接缝）
import frame_draw_list;        // FrameDrawList / SceneFrameData
import scene_context;          // Light
import lights_types_generated; // GpuLight / kMaxLights

namespace RGL {
namespace {

using testing::expectNear;

/** 测试机位（任意非零非整值，头单灯 eyePos 字段直传）。 */
constexpr glm::vec3 kEye{7.5f, -2.25f, 11.0f};

/** 按 index 造可辨识的灯：所有字段随 index 变化且避开退化值
 *  （direction 非零且 |d|≠1 → normalize 必改值，"忘归一化"变异可检）。 */
Light makeLight(uint32_t i) {
	Light L;
	L.index = i;
	L.direction = glm::vec3(0.5f + 0.2f * i, -1.0f, 0.1f * i);
	L.color = glm::vec3(0.2f + 0.1f * i, 0.5f, 0.9f - 0.05f * i);
	L.intensity = 2.0f + 0.5f * i;
	L.ambient = glm::vec3(0.01f * i, 0.02f, 0.03f);
	L.position = glm::vec3(1.0f * i, 2.0f * i + 1.0f, 3.0f * i);
	L.range = 5.0f + i;
	L.type = i % 2u;  // 覆盖定向(0)与点光(1)两型
	return L;
}

/** fillLightData 对单灯的期望换算（照源码逐式：normalize / color×intensity 预乘 /
 *  params = (range, type, 0, 0)）。slot 允许是默认 Light{}（未落槽回落形态）。 */
GpuLight expectSlot(const Light& L) {
	GpuLight G;
	G.direction = glm::vec4(glm::normalize(L.direction), 0.0f);
	G.position = glm::vec4(L.position, 0.0f);
	G.color = glm::vec4(L.color * L.intensity, 0.0f);
	G.params = glm::vec4(L.range, static_cast<float>(L.type), 0.0f, 0.0f);
	return G;
}

/** GpuLight 逐分量比对（失败时 expectNear 打出差值明细）。 */
void checkSlot(const GpuLight& got, const GpuLight& want, const char* what) {
	INFO(what);
	for (int k = 0; k < 4; ++k) {
		expectNear(got.direction[k], want.direction[k]);
		expectNear(got.position[k], want.position[k]);
		expectNear(got.color[k], want.color[k]);
		expectNear(got.params[k], want.params[k]);
	}
}

}  // namespace

TEST_CASE("fillLightData：乱序创建（2,0,1）仍按 Light.index 落槽，primary = index 0") {
	entt::registry rg;
	// 创建序刻意与 index 序不同（{2,0,1} 无论 view 实际正反迭代，都与 index 升序不同）：
	// 迭代序落槽的实现在这里必错位。
	Light expect[kMaxLights];  // expect[i] = index i 的灯（未创建槽留默认）
	for (const uint32_t idx : {2u, 0u, 1u}) {
		expect[idx] = makeLight(idx);
		rg.emplace<Light>(rg.create(), expect[idx]);  // entt 3.16：emplace 需显式 entity（main 同款）
	}

	FrameDrawList list;
	const Light primary = fillLightData(rg, kEye, list);

	CHECK(list.sceneData.lightCount == 3);
	// primary（返回值本体 + 头单灯换算）= index 0 的灯，不是先创建的 index 2 灯。
	CHECK(primary.index == 0);
	CHECK(primary.direction == expect[0].direction);
	CHECK(primary.color == expect[0].color);
	CHECK(primary.intensity == expect[0].intensity);

	// 头单灯（④）：color = primary.color × primary.intensity（CPU 预乘契约）、
	// direction = normalize、eyePos = 传入机位、ambient 直传。
	for (int k = 0; k < 3; ++k) {
		expectNear(list.sceneData.light.color[k],
			expect[0].color[k] * expect[0].intensity);
		expectNear(list.sceneData.light.direction[k],
			glm::normalize(expect[0].direction)[k]);
	}
	CHECK(list.sceneData.light.eyePos == kEye);
	CHECK(list.sceneData.light.ambient == expect[0].ambient);
	// 独立锚：头单灯方向已归一（模长 1）——忘 normalize 的变异即使 expectNear 被
	// 后续"宽容"也拦得住（makeLight 保证原向量 |d|≠1）。
	expectNear(glm::length(glm::vec3(list.sceneData.light.direction)), 1.0);

	// 尾区灯数组（①落位 + ⑤params）：lightArray[i] = index i 的灯换算值。
	for (uint32_t i = 0; i < 3; ++i)
		checkSlot(list.sceneData.lightArray[i], expectSlot(expect[i]),
			("lightArray[" + std::to_string(i) + "]").c_str());
	// 未触及的尾槽保持默认零（fillLightData 只写 [0, nLights)）。
	checkSlot(list.sceneData.lightArray[3], GpuLight{}, "lightArray[3] 默认零");
}

TEST_CASE("fillLightData：无灯 entity → lightCount=0、头单灯 = 默认 Light{} 换算") {
	entt::registry rg;
	FrameDrawList list;
	const Light primary = fillLightData(rg, kEye, list);

	const Light def{};
	CHECK(list.sceneData.lightCount == 0);
	// 返回值 = 默认 Light{}（同旧 empty() 分支）：逐字段钉死。
	CHECK(primary.direction == def.direction);
	CHECK(primary.color == def.color);
	CHECK(primary.intensity == def.intensity);
	CHECK(primary.ambient == def.ambient);
	// 头单灯 = 默认灯换算（**不是全零**）：默认 direction (0.35,-1,0.25) 非零 →
	// normalize 有限（NaN 担忧不成立，以源码默认值为准）。
	CHECK(glm::length(def.direction) > 0.0f);
	for (int k = 0; k < 3; ++k) {
		expectNear(list.sceneData.light.direction[k],
			glm::normalize(def.direction)[k]);
		expectNear(list.sceneData.light.color[k], def.color[k] * def.intensity);
	}
	CHECK(list.sceneData.light.eyePos == kEye);
	// lightArray 一槽都不写（lightCount=0 → 循环零次）。
	checkSlot(list.sceneData.lightArray[0], GpuLight{}, "无灯 lightArray[0]");
}

TEST_CASE("fillLightData：index ≥ kMaxLights 不落槽但计数；min(count, kMaxLights) 封顶") {
	// 情形 A：只有一个灯且 index 越界 → 不落槽，但 lightEntityCount=1 计入 →
	// lightCount = 1，槽 0 回落**默认 Light{} 的换算值**（源码语义，钉成断言）。
	{
		entt::registry rg;
		rg.emplace<Light>(rg.create(), makeLight(kMaxLights));  // index = kMaxLights → 越界
		FrameDrawList list;
		fillLightData(rg, kEye, list);
		CHECK(list.sceneData.lightCount == 1);  // 计数含越界灯（删计数 → 0 红）
		checkSlot(list.sceneData.lightArray[0], expectSlot(Light{}),
			"越界灯不占槽，槽 0 = 默认灯换算");
	}
	// 情形 B：0..kMaxLights-1 满 + 1 个越界灯 → lightCount 封顶 kMaxLights，
	// 有效槽内容不被越界灯污染。
	{
		entt::registry rg;
		Light expect[kMaxLights + 1];
		for (uint32_t i = 0; i < kMaxLights; ++i) {
			expect[i] = makeLight(i);
			rg.emplace<Light>(rg.create(), expect[i]);
		}
		expect[kMaxLights] = makeLight(kMaxLights);
		rg.emplace<Light>(rg.create(), expect[kMaxLights]);
		FrameDrawList list;
		fillLightData(rg, kEye, list);
		CHECK(list.sceneData.lightCount == kMaxLights);  // min(9,8)（删 min → 9 红）
		for (uint32_t i = 0; i < kMaxLights; ++i)
			checkSlot(list.sceneData.lightArray[i], expectSlot(expect[i]),
				("满员 lightArray[" + std::to_string(i) + "]").c_str());
	}
}

TEST_CASE("fillLightData：index 重复 → 后写覆盖先写（view 迭代序内），尾槽回落默认灯") {
	entt::registry rg;
	// 三个 entity 全 index 0（值取 makeLight(1..3) 保证互异）。
	Light v1 = makeLight(1u);
	v1.index = 0;
	Light v2 = makeLight(2u);
	v2.index = 0;
	Light last = makeLight(3u);
	last.index = 0;
	rg.emplace<Light>(rg.create(), v1);
	rg.emplace<Light>(rg.create(), v2);
	rg.emplace<Light>(rg.create(), last);

	// entt 的 view 迭代序**无保证**（3.16 实测非创建序）——期望值不假设顺序，
	// 直接拿同一 view 再迭代一遍取"最后一个"：断言的是源码真实语义
	// "primary/lightSlot[0] = 迭代序最后的 index-0 灯"（顺序由被测调用与本行共享，
	// 不存在"测试以为的顺序 ≠ 实现的顺序"）。
	Light expectLast{};
	rg.view<Light>().each([&](const Light& L) { expectLast = L; });
	CHECK(expectLast.index == 0);  // 三个全是 index 0，最后一个必是

	FrameDrawList list;
	const Light primary = fillLightData(rg, kEye, list);

	// 计数按 entity 数（3），不按去重槽位——照源码 min(3, kMaxLights) 语义。
	CHECK(list.sceneData.lightCount == 3);
	// 后写覆盖先写：primary 与槽 0 都是迭代序最后的 index-0 灯。
	CHECK(primary.range == expectLast.range);
	CHECK(primary.direction == expectLast.direction);
	checkSlot(list.sceneData.lightArray[0], expectSlot(expectLast), "重复 index 槽 0 = 迭代序最后");
	// 槽 1/2 没人写 → 默认 Light{} 换算（"count 与有效槽数脱钩"的源码形态；
	// 迭代序落槽 lightSlot[count-1] 的变异会把两个非默认灯写进槽 1/2 → 此处红）。
	checkSlot(list.sceneData.lightArray[1], expectSlot(Light{}), "重复 index 槽 1 默认");
	checkSlot(list.sceneData.lightArray[2], expectSlot(Light{}), "重复 index 槽 2 默认");
	// 阳性锚：三灯值互异（若 makeLight 退化导致三值相同，上面的"最后赢"断言会变摆设）。
	CHECK(v1.range != v2.range);
	CHECK(v2.range != last.range);
}

TEST_CASE("fillLightData：GpuLight.params = (range, type, 0, 0)（含点光 type=1）") {
	entt::registry rg;
	// makeLight(1) = type 1（点光）、range 6；makeLight(0) = type 0、range 5。
	rg.emplace<Light>(rg.create(), makeLight(0u));
	rg.emplace<Light>(rg.create(), makeLight(1u));
	FrameDrawList list;
	fillLightData(rg, kEye, list);

	CHECK(list.sceneData.lightArray[0].params == glm::vec4(5.0f, 0.0f, 0.0f, 0.0f));
	CHECK(list.sceneData.lightArray[1].params == glm::vec4(6.0f, 1.0f, 0.0f, 0.0f));
}

}  // namespace RGL
