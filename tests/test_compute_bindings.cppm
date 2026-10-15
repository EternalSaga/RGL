module;
#include <doctest/doctest.h>
#include <initializer_list>  // 花括号初始化列表（import 不带进来，GMF 自己 include）
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// compute binding 接口表（rgl/generated/compute_bindings_generated.cppm）与它的查表/解析
// 机制（shader.cppm 的 resolveComputeBinding / computeShaderKey / resolveComputeBindings）
// 的回归。产物只读，本文件只在测试侧建立断言。
//
// ## 为什么值得钉（这一刀买的东西）
//
// 改前，七处 compute shader object 的调用点写的是 **binding 号字面量**：
// `{0, HeapSlot{headSlot}}, {1, HeapSlot{nodesSlot}}, {2, HeapSlot{depthSlot}}`。
// 而号是 shader 侧 `[[vk::binding(N)]]` 的声明。两边手抄 ⟹ 改一边时**没有任何东西会响**：
// 无编译错、无反射校验、无 VVL 报错，只是往错的堆槽读数据（症状落在渲染上，离真因三跳）。
// 这与 descriptorSlots 当初立项的理由同形。
//
// 现在调用点写名字，号查构建期接口表。本文件钉的是这张表的**内容**与解析的**判据**：
// 表只说"shader 里是这样"，不说"shader 里**该**是那样"——期望值出处 = 下面逐条注明的
// shader 源码行（逐条打开 `rgl/shaders/*.comp.slang` 读 `[[vk::binding(N)]]` 实参），
// **不抄生成物、不抄记忆**。若反射管线本身坏了（判别子捞进 push constant、漏一枚 shader、
// 排序漂了），生成物会忠实地跟着坏，而本文件红。
//
// ## 🔴 变异红线声明（哪个变异让我红）
//
//   ① **改 shader 的 binding 号但不重新 `just reflect`**：把 `izb_prime.comp.slang` 的
//      `primed` 从 `[[vk::binding(3)]]` 改成 7 → 不 reflect → **构建被 reflection_check 的
//      SHA1 守卫 FATAL 拦下**（本文件根本跑不到）。这条红的是护栏，不是本文件——写在这里
//      是为了别让后来人把"测试没红"读成"没生效"。
//   ② **改 shader 的 binding 号并重新 reflect**：同上改 3→7 → 接口表跟着变 → 本文件
//      「七个调用点」用例**必须红**（`primed` 期望 3 实得 7）。同一条覆盖"改名"：改名后
//      表里查不到那个名字，`REQUIRE(got.has_value())` 红。
//   ③ **把某枚 shader 从管线名单里摘掉**（cmake/RglReflection.cmake 的
//      rgl_reflect_compute_access 参数表少一行）→ 「覆盖锚点」用例**必须红**（9 → 8）。
//      产品侧同时红：构造那枚 shader object 时抛"shader 不在接口表里"（表是承重的，不是装饰）。
//   ④ **把判别子写错**（比如不再按 descriptor-table-slot 过滤，push 也收进来）→
//      「覆盖锚点」里的**逐枚条数**断言红（每枚都多一条 push）。同一条也覆盖"过滤过宽把
//      别的模块的全局量捞进来"。
//   ⑤ **调用点名字打错一个字母**（`"head"` → `"headX"`）→ 本文件不红（本文件测表，不测
//      调用点字面量），**启动即抛**并点名 shader + 错名 + 合法名清单。写在这里是为了说明
//      这条的落点在运行期，不在本文件；它的单元等价形态 = 下面「三类报错各自可归因」的 ②。
//   ⑥ **方向编码坏掉**（B2-2 加的 writable/readable：`accessToFlags` 把 `none` 也编成
//      readable，或把 `readWrite` 编成只读）→ 「七个调用点」里逐条的 writable/readable 断言
//      **必须红**（期望值出处 = 同一行的声明类型，不是生成物）；`none` 那条另由
//      「none 编码锚点」独立守——两个方向不对称的失效面不该共用一条断言。
//
// ## 为什么这些用例能在 `just test`（无 GPU 无窗口）里跑
//
// 解析逻辑是**纯函数**（不碰 VkDevice、不读 SPV、不做运行期反射——SPV 不持有名字，
// 名字→号只能查构建期产物）。所以三类报错的消息本体也在无设备子集里可测，不必等真设备。

export module test_compute_bindings;

import rlstd;
import compute_bindings_generated;
import shader;  // resolveComputeBinding / computeShaderKey / resolveComputeBindings（纯函数）

namespace RGL {

namespace {

/// 子串包含判定（报错消息断言用）。不复用任何被测机制。
bool contains(const std::string& haystack, std::string_view needle) {
	return haystack.find(needle) != std::string::npos;
}

/// 跑一次解析并取异常消息；**没抛返回空串**——调用方必须先 REQUIRE 它非空再判内容，
/// 否则"没抛"会被读成"抛了一条空消息"（假绿）。
std::string throwMessage(
	const std::string& spvPath, std::span<const std::pair<std::string_view, HeapSlot>> named) {
	try {
		(void)resolveComputeBindings(spvPath, named);
		return {};
	} catch (const std::runtime_error& e) {
		return e.what();
	}
}

}  // namespace

TEST_CASE("阳性锚点：izb_create 的 head/nodes/depthTex（期望值 = shader 源码逐行）") {
	// 出处 = rgl/shaders/izb_create.comp.slang:30-32：
	//   [[vk::binding(0)]] RWByteAddressBuffer head;
	//   [[vk::binding(1)]] RWByteAddressBuffer nodes;
	//   [[vk::binding(2)]] Texture2D<float>   depthTex;
	// 🔴 这三条是"名字键"整刀的语义地基：名字→号一旦串位，堆槽就喂错 buffer。
	const auto head = resolveComputeBinding("izb_create.comp.slang", "head");
	REQUIRE(head.has_value());
	CHECK(*head == 0);

	const auto nodes = resolveComputeBinding("izb_create.comp.slang", "nodes");
	REQUIRE(nodes.has_value());
	CHECK(*nodes == 1);

	const auto depth = resolveComputeBinding("izb_create.comp.slang", "depthTex");
	REQUIRE(depth.has_value());
	CHECK(*depth == 2);

	// push constant **不入表**（heap 下由 vkCmdPushDataEXT 自动供给，没有映射条目可映射）。
	// 出处 = 同文件 :34 `[[vk::push_constant]] IzbCreatePush push;`
	CHECK_FALSE(resolveComputeBinding("izb_create.comp.slang", "push").has_value());

	// 🔴 方向（B2-2 加的第二个问题）：同一批源码行读声明类型 —— RW* = 会写，Texture2D = 只读。
	// hazard 判据（compute_access_log）吃的就是这两个 bool，方向错 = 漏报或误报 hazard。
	const auto rows = computeBindings("izb_create.comp.slang");
	auto find = [&](std::string_view name) {
		for (const auto& b : rows)
			if (b.name == name)
				return b;
		return RgComputeBinding{"", 0, false, false};
	};
	CHECK(find("head").writable);
	CHECK(find("head").readable);      // RWByteAddressBuffer（:30）
	CHECK(find("nodes").writable);     // RWByteAddressBuffer（:31）
	CHECK_FALSE(find("depthTex").writable);
	CHECK(find("depthTex").readable);  // Texture2D（:32）只读
}

TEST_CASE("阳性锚点：七个调用点用到的每个名字都解析到 shader 里的那个号（含方向）") {
	// 这张表 = 「调用点写的名字」与「shader 里的号 + 读写方向」的对应关系本体（= 改前手抄的那些
	// 字面量）。号与名字逐条出处见行尾注释（打开 shader 读 `[[vk::binding(N)]]` 实参）；方向的
	// 出处 = **同一行的声明类型**（`RW*` → writable，`readonly` / `StructuredBuffer` / `Texture2D`
	// → 只读），🔴 不抄 compute_access.json、不抄生成物。
	struct Expect {
		std::string_view shader;
		std::string_view name;
		uint32_t binding;
		bool writable;
		bool readable;
	};
	static constexpr Expect kExpects[] = {
		// izb_create.comp.slang:30-32（izb_shadow_impl.cppm 的 createShader）
		{"izb_create.comp.slang", "head", 0, true, true},        // RWByteAddressBuffer
		{"izb_create.comp.slang", "nodes", 1, true, true},       // RWByteAddressBuffer
		{"izb_create.comp.slang", "depthTex", 2, false, true},   // Texture2D<float>
		// izb_prime.comp.slang:19-21（izb_shadow_impl.cppm 的 primeShader）
		// 🔴 primed 是 3 不是 2：跳号是 shader 侧真形态，手抄时最容易"顺手写 2"
		{"izb_prime.comp.slang", "head", 0, false, true},        // ByteAddressBuffer（只读！）
		{"izb_prime.comp.slang", "nodes", 1, false, true},       // ByteAddressBuffer
		{"izb_prime.comp.slang", "primed", 3, true, true},       // RWByteAddressBuffer
		// evssm_moments.comp.slang:23-24（evssm_shadow_impl.cppm 的 momentsShader）
		{"evssm_moments.comp.slang", "shadowDepth", 0, false, true},  // Texture2D<float>
		{"evssm_moments.comp.slang", "moments", 1, true, true},       // RWTexture2D<float4>
		// evssm_downsample.comp.slang:20-21（evssm_shadow_impl.cppm 的 downShaders）
		{"evssm_downsample.comp.slang", "mipSrc", 0, true, true},     // RWTexture2D（读侧也 RW）
		{"evssm_downsample.comp.slang", "mipDst", 1, true, true},     // RWTexture2D
		// cull.comp.slang:43-48（gpu_cull_impl.cppm 的 cullShader）
		{"cull.comp.slang", "cullTable", 0, false, true},        // readonly ByteAddressBuffer
		{"cull.comp.slang", "sequences", 2, true, true},         // RWByteAddressBuffer
		{"cull.comp.slang", "countBuf", 4, true, true},          // RWByteAddressBuffer
		// scene_tables.slang:27（cull / meshlet_cull 都经 import scene_tables 拿到它）
		{"cull.comp.slang", "objectMatrixTable", 23, false, true},       // StructuredBuffer
		{"meshlet_cull.comp.slang", "objectMatrixTable", 23, false, true},
		// stream_fanout.comp.slang:20-25（gpu_cull_impl.cppm 的 fanoutShader）
		// ⚠ 与 cull.comp.slang 同名的两个 binding 号**不同**（sequences 在 cull 是 2、
		// 在 fanout 是 1）——这正是"按 shader 分表"而不是全局一张名字表的原因。
		// ⚠ 方向也**不同**：sequences 在 cull 是 RW、在 fanout 是只读 ⟹ hazard 的写侧唯一。
		{"stream_fanout.comp.slang", "cullTable", 0, false, true},  // readonly ByteAddressBuffer
		{"stream_fanout.comp.slang", "sequences", 1, false, true},  // ByteAddressBuffer（只读）
		{"stream_fanout.comp.slang", "streams", 2, true, true},     // RWByteAddressBuffer
		// meshlet_cull.comp.slang:34-39（meshlet_cull_service_impl.cppm 的 cullShader）
		{"meshlet_cull.comp.slang", "cullItems", 0, false, true},       // readonly ByteAddressBuffer
		{"meshlet_cull.comp.slang", "clusters", 1, false, true},        // readonly ByteAddressBuffer
		{"meshlet_cull.comp.slang", "clusterVisible", 4, true, true},   // RWByteAddressBuffer
	};
	for (const auto& e : kExpects) {
		INFO("shader=" << e.shader << " name=" << e.name);
		const auto got = resolveComputeBinding(e.shader, e.name);
		REQUIRE(got.has_value());
		CHECK(*got == e.binding);
		// 方向按 (shader, name) 回查生成物（resolveComputeBinding 只回答号）。
		bool seen = false;
		for (const auto& b : computeBindings(e.shader)) {
			if (b.name != e.name)
				continue;
			seen = true;
			INFO("方向断言：writable/readable");
			CHECK(b.writable == e.writable);
			CHECK(b.readable == e.readable);
		}
		CHECK(seen);
	}
}

TEST_CASE("none 编码锚点：CB 型参数两个方向 bool 都 false（判据里必须跳过）") {
	// 出处 = scene.slang 的 `[[vk::constant_buffer]]` 区（sceneHeader / sceneIrradiance / …）：
	// 它们是 descriptor-table-slot 型（所以**入表**），但 getResourceAccess() 给 none
	// ⟹ writable/readable 两个都 false。hazard 判据的第 ③ 条靠这两个 false 跳过它们；
	// 若编码把 none 误编成 readable，每个 UBO 槽都会与别人的写槽撞出一条假 hazard。
	static constexpr std::string_view kNoneParams[] = {
		"sceneHeader", "sceneIrradiance", "sceneShadowViewProj", "sceneShadowParams",
		"sceneTaa", "sceneSsr", "sceneWet",
	};
	for (std::string_view shader : {"cull.comp.slang", "meshlet_cull.comp.slang"}) {
		for (std::string_view name : kNoneParams) {
			INFO("shader=" << shader << " name=" << name);
			bool seen = false;
			for (const auto& b : computeBindings(shader)) {
				if (b.name != name)
					continue;
				seen = true;
				CHECK_FALSE(b.writable);
				CHECK_FALSE(b.readable);
			}
			CHECK(seen);  // 不在表里 = 判别子漂了，也算红（别把"查不到"读成"跳过了"）
		}
	}
	// 反向的一半：同表里真读真写的参数必须**不是**双 false（否则上面那条恒真）。
	for (const auto& b : computeBindings("cull.comp.slang"))
		if (b.name == "sequences") {
			CHECK(b.writable);
			CHECK(b.readable);
		}
}

TEST_CASE("反向用例：不存在的名字 / 不存在的 shader → nullopt（区分「没找到」与「坏掉了」）") {
	// 差一个字母（变异 ⑤ 的形态）：必须是 nullopt，**不是** 0——兜底成 0 = 静默读 head 槽
	CHECK_FALSE(resolveComputeBinding("izb_create.comp.slang", "headX").has_value());
	CHECK_FALSE(resolveComputeBinding("izb_create.comp.slang", "").has_value());
	// 大小写敏感：shader 里没有 "Head"
	CHECK_FALSE(resolveComputeBinding("izb_create.comp.slang", "Head").has_value());
	// 不存在的 shader：查不到而不是"空表里查不到"混成一种可读回形态
	CHECK_FALSE(resolveComputeBinding("nope.comp.slang", "head").has_value());
	CHECK(computeBindings("nope.comp.slang").empty());

	// 🔴 名字在**别的** shader 里存在，不代表在本 shader 里存在（查错 shader 的实现会过掉
	// 上面所有用例）：primed 只在 izb_prime（=3），izb_create 里没有。
	CHECK(resolveComputeBinding("izb_prime.comp.slang", "primed").has_value());
	CHECK_FALSE(resolveComputeBinding("izb_create.comp.slang", "primed").has_value());
	// 同名的两个 binding 必须各自解析到各自的号（cull=2 / fanout=1）
	CHECK(*resolveComputeBinding("cull.comp.slang", "sequences") == 2);
	CHECK(*resolveComputeBinding("stream_fanout.comp.slang", "sequences") == 1);
}

TEST_CASE("空集合守卫 + 覆盖锚点：表里恰好 9 枚 shader，逐枚非空、按号升序") {
	const auto shaders = computeShaderNames();
	// 🔴 先断非空再遍历：空集合上的 for 断言恒真（假绿）。这条同时是"管线整条没跑"的探针。
	REQUIRE_FALSE(shaders.empty());
	// 覆盖锚点：9 = 实测的 compute-access 管线名单长度（cmake/RglReflection.cmake 的
	// rgl_reflect_compute_access 参数：cull / env_prefilter / evssm_downsample / evssm_moments /
	// izb_create / izb_prime / meshlet_cull / mip_gen / stream_fanout）。
	// 摘掉一枚 → 这里红（变异 ③）；产品侧同时抛"shader 不在接口表里"。
	CHECK(shaders.size() == 9);

	std::string joined;
	for (unsigned i = 0; i < shaders.size(); ++i) {
		INFO("shader[" << i << "]=" << shaders[i]);
		CHECK_FALSE(shaders[i].empty());
		// 键必须是 basename 且以 .slang 结尾——computeShaderKey 的产物形态，两边对不上就查不到
		CHECK(shaders[i].ends_with(".slang"));
		CHECK(shaders[i].find('/') == std::string_view::npos);
		// shader 名升序 = 产物确定性排序的钉子（不排序 = 每次 reflect 一份假 diff）
		if (i > 0)
			CHECK(shaders[i - 1] < shaders[i]);

		const auto bindings = computeBindings(shaders[i]);
		// 九枚全都有 descriptor-table-slot 参数（push constant 不收）；某枚变空 = 判别子漂了
		// 或 shader 把 binding 全删了，两种都该出声。
		REQUIRE_FALSE(bindings.empty());
		for (unsigned j = 0; j < bindings.size(); ++j) {
			CHECK_FALSE(bindings[j].name.empty());
			if (j > 0) {
				CHECK(bindings[j - 1].binding < bindings[j].binding);  // 严格升序（同 shader 内号唯一）
				CHECK(bindings[j - 1].name != bindings[j].name);
			}
			// push constant 不入表：判别子写错的最直接形态就是收进 "push"
			CHECK(bindings[j].name != "push");
		}
		joined += shaders[i];
		joined += ' ';
	}
	// 表里必须认得这七枚（= 七个调用点涉及的 shader）。少一枚 = 调用点启动即抛。
	for (std::string_view need : {"izb_create.comp.slang", "izb_prime.comp.slang",
	         "evssm_moments.comp.slang", "evssm_downsample.comp.slang", "cull.comp.slang",
	         "stream_fanout.comp.slang", "meshlet_cull.comp.slang"})
		CHECK(contains(joined, need));
}

TEST_CASE("computeShaderKey：spvPath → 接口表键（basename + .spv 换 .slang）") {
	CHECK(computeShaderKey("generated_shaders/izb_create.comp.spv") == "izb_create.comp.slang");
	CHECK(computeShaderKey("izb_prime.comp.spv") == "izb_prime.comp.slang");
	CHECK(computeShaderKey("build/rgl/generated_shaders/cull.comp.spv") == "cull.comp.slang");
	// 不以 .spv 结尾时**不猜**：照拼，键查不到 → 调用方抛"不在表里"并原样点名（可归因）
	CHECK(computeShaderKey("weird.spv.txt") == "weird.spv.txt.slang");
}

TEST_CASE("三类报错各自可归因（resolveComputeBindings 无设备可测）") {
	const std::vector<std::pair<std::string_view, HeapSlot>> oneHead{
		{"head", HeapSlot{7}}};

	// ── 正向：解析结果 = (号, 槽) 且顺序 = 调用点给的顺序 ──
	const std::vector<std::pair<std::string_view, HeapSlot>> named{
		{"nodes", HeapSlot{11}}, {"head", HeapSlot{5}}};
	const auto resolved = resolveComputeBindings("generated_shaders/izb_create.comp.spv", named);
	REQUIRE(resolved.size() == 2);
	CHECK(resolved[0].first == 1);  // nodes → 1
	CHECK(resolved[0].second.slot == 11);
	CHECK(resolved[1].first == 0);  // head → 0
	CHECK(resolved[1].second.slot == 5);

	// ── ① shader 不在表里：消息点名 shader + 列出表里有哪些 shader ──
	const auto msg1 = throwMessage("generated_shaders/nope.comp.spv", oneHead);
	REQUIRE_FALSE(msg1.empty());
	CHECK(contains(msg1, "nope.comp.slang"));        // 推导出的键原样出现
	CHECK(contains(msg1, "izb_create.comp.slang"));  // 合法 shader 清单
	CHECK(contains(msg1, "不在 compute binding 接口表里"));

	// ── ② 名字不在该 shader 里：点名 shader + 错名 + 该 shader 全部合法名（带号）──
	const std::vector<std::pair<std::string_view, HeapSlot>> wrongName{
		{"headX", HeapSlot{7}}};
	const auto msg2 = throwMessage("generated_shaders/izb_create.comp.spv", wrongName);
	REQUIRE_FALSE(msg2.empty());
	CHECK(contains(msg2, "izb_create.comp.slang"));
	CHECK(contains(msg2, "headX"));
	CHECK(contains(msg2, "head(0)"));      // 合法名清单带号 = 不用再去开 shader
	CHECK(contains(msg2, "nodes(1)"));
	CHECK(contains(msg2, "depthTex(2)"));
	// 反向的一半：别的 shader 的合法名不该出现在本 shader 的清单里
	CHECK_FALSE(contains(msg2, "primed(3)"));

	// ── ③ 同一个名字传两次：点名 shader + 名字 + 两个堆槽 ──
	const std::vector<std::pair<std::string_view, HeapSlot>> twice{
		{"head", HeapSlot{5}}, {"head", HeapSlot{9}}};
	const auto msg3 = throwMessage("generated_shaders/izb_create.comp.spv", twice);
	REQUIRE_FALSE(msg3.empty());
	CHECK(contains(msg3, "izb_create.comp.slang"));
	CHECK(contains(msg3, "head"));
	CHECK(contains(msg3, "传了两次"));
	CHECK(contains(msg3, " 5 "));
	CHECK(contains(msg3, "9"));
	// 三条消息两两不同 = 三类错误可从日志一眼分开（可归因的最低要求）
	CHECK(msg1 != msg2);
	CHECK(msg2 != msg3);
	CHECK(msg1 != msg3);
}

}  // namespace RGL
