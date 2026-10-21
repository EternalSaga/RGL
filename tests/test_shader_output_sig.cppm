module;
#include <doctest/doctest.h>
#include <cstdint>
#include <initializer_list>  // 合成指令表的花括号列表（modules 下不从 import 带进来）
#include <string>
#include <vector>

/**
 * test_shader_output_sig —— `spvFragmentOutputSignature` 的契约门（2026-10-21，债-140 批
 * 顺带修的 VUID-vkUpdateIndirectExecutionSetShaderEXT-None-11148）。
 *
 * ## 为什么值得钉
 *
 * 这个签名是 **DGC execution set 的准入判据**：set 里每个 fragment shader 必须与初始态
 * 输出接口逐位相同。判据的实现只要漏掉一条（例如把 Input 当 Output 收、或把 BuiltIn
 * 输出算进来），启动期硬判据就会**要么放行违例、要么误杀合法条目**——两种都静默：
 * 前者只剩 VVL 在 NVIDIA 上刷屏（本机其它卡不走真 DGC，根本看不见），后者表现为
 * "DGC 构造失败回落重放"的 warn，图上完全看不出来。
 *
 * ## 期望值出处
 *
 * 手写合成 SPV（不读构建产物）：按 SPIR-V 1.4 指令编码逐词摆出
 * `OpTypeFloat/OpTypeVector/OpTypePointer/OpDecorate(Location/BuiltIn)/OpVariable`，
 * 期望值由 SPIR-V 规范语义算得（storage class Output + 带 Location 的变量 = 输出接口），
 * **不抄被测实现**。
 *
 * ## 变异红线（逐条实测，2026-10-21）
 *
 *   1. 存储类判据 `v[3] != 3` 改成"连 Input 一起收" → 「Input 不进签名」用例红
 *      （签名里多出 loc0:Vf32x4）。
 *   2. `if (!location) continue` 删掉（BuiltIn 也进签名）→ 「BuiltIn 不进签名」用例红。
 * 其余两条（depth-only 空签名、坏魔数哨兵）是**反向用例**，实现"顺手返回空串/空集"即红。
 */

export module test_shader_output_sig;

import rlstd;
import shader;  // spvFragmentOutputSignature

namespace {

	constexpr uint32_t kOpTypeFloat = 22;
	constexpr uint32_t kOpTypeVector = 23;
	constexpr uint32_t kOpTypePointer = 32;
	constexpr uint32_t kOpVariable = 59;
	constexpr uint32_t kOpDecorate = 71;

	constexpr uint32_t kDecorationLocation = 30;
	constexpr uint32_t kDecorationBuiltIn = 11;
	constexpr uint32_t kStampFragDepth = 1;  // BuiltIn FragDepth 的值（语义只用于"有装饰但无 Location"）

	constexpr uint32_t kStorageInput = 1;
	constexpr uint32_t kStorageOutput = 3;

	/** 指令词 = wordCount<<16 | opcode（SPIR-V 物理布局），后接操作数。 */
	std::vector<uint32_t> inst(uint32_t opcode, std::initializer_list<uint32_t> operands) {
		std::vector<uint32_t> w;
		w.push_back((static_cast<uint32_t>(operands.size()) + 1) << 16 | opcode);
		w.insert(w.end(), operands.begin(), operands.end());
		return w;
	}

	/** 合成一个最小 SPV：5 词头 + 给定指令序列（bound 取 64，够放本文件的 id）。 */
	std::vector<uint32_t> makeSpv(std::initializer_list<std::vector<uint32_t>> instructions) {
		std::vector<uint32_t> spv{0x07230203u, 0x00010400u, 0u, 64u, 0u};
		for (const auto& i : instructions)
			spv.insert(spv.end(), i.begin(), i.end());
		return spv;
	}

	/** 两输出（loc1 = vec4、loc0 = f32）+ 一输入（loc0）+ 一 BuiltIn 输出（无 Location）。 */
	std::vector<uint32_t> outputsPlusNoiseSnv() {
		return makeSpv({
			inst(kOpTypeFloat, {1u, 32u}),
			inst(kOpTypeVector, {2u, 1u, 4u}),
			inst(kOpTypePointer, {3u, kStorageOutput, 2u}),  // → vec4 输出
			inst(kOpTypePointer, {4u, kStorageOutput, 1u}),  // → f32 输出（含 BuiltIn 用）
			inst(kOpTypePointer, {5u, kStorageInput, 2u}),   // → vec4 输入
			inst(kOpDecorate, {10u, kDecorationLocation, 1u}),
			inst(kOpDecorate, {11u, kDecorationLocation, 0u}),
			inst(kOpDecorate, {12u, kDecorationLocation, 0u}),
			inst(kOpDecorate, {13u, kDecorationBuiltIn, kStampFragDepth}),
			inst(kOpVariable, {3u, 10u, kStorageOutput}),
			inst(kOpVariable, {4u, 11u, kStorageOutput}),
			inst(kOpVariable, {5u, 12u, kStorageInput}),
			inst(kOpVariable, {4u, 13u, kStorageOutput}),
		});
	}

}  // namespace

TEST_CASE("spvFragmentOutputSignature：只收 Output+Location，Input/BuiltIn 不进签名") {
	const std::string sig = RGL::spvFragmentOutputSignature(outputsPlusNoiseSnv());
	CHECK(sig == "loc0:f32;loc1:Vf32x4");
}

TEST_CASE("spvFragmentOutputSignature：声明序无关（同接口不同摆法 = 同签名）") {
	const auto reordered = makeSpv({
		inst(kOpTypePointer, {3u, kStorageOutput, 2u}),
		inst(kOpDecorate, {12u, kDecorationLocation, 0u}),
		inst(kOpVariable, {5u, 12u, kStorageInput}),
		inst(kOpTypeFloat, {1u, 32u}),
		inst(kOpDecorate, {13u, kDecorationBuiltIn, kStampFragDepth}),
		inst(kOpVariable, {4u, 13u, kStorageOutput}),
		inst(kOpTypeVector, {2u, 1u, 4u}),
		inst(kOpDecorate, {10u, kDecorationLocation, 1u}),
		inst(kOpTypePointer, {4u, kStorageOutput, 1u}),
		inst(kOpVariable, {3u, 10u, kStorageOutput}),
		inst(kOpDecorate, {11u, kDecorationLocation, 0u}),
		inst(kOpVariable, {4u, 11u, kStorageOutput}),
		inst(kOpTypePointer, {5u, kStorageInput, 2u}),
	});
	CHECK(RGL::spvFragmentOutputSignature(reordered) ==
		RGL::spvFragmentOutputSignature(outputsPlusNoiseSnv()));
}

TEST_CASE("spvFragmentOutputSignature：depth-only（无带 Location 的输出）= 空签名") {
	// 反向用例：实现把 BuiltIn 输出（FragDepth）算进来就会拿到非空串。
	const auto depthOnly = makeSpv({
		inst(kOpTypeFloat, {1u, 32u}),
		inst(kOpTypePointer, {4u, kStorageOutput, 1u}),
		inst(kOpTypePointer, {5u, kStorageInput, 1u}),
		inst(kOpDecorate, {13u, kDecorationBuiltIn, kStampFragDepth}),
		inst(kOpDecorate, {12u, kDecorationLocation, 0u}),
		inst(kOpVariable, {4u, 13u, kStorageOutput}),
		inst(kOpVariable, {5u, 12u, kStorageInput}),
	});
	CHECK(RGL::spvFragmentOutputSignature(depthOnly).empty());
}

TEST_CASE("spvFragmentOutputSignature：坏码流返回哨兵（不与任何真实签名相等）") {
	const std::vector<uint32_t> bad{0xDEADBEEFu, 0u, 0u, 0u, 0u, 0u};
	CHECK(RGL::spvFragmentOutputSignature(bad) == "SPV_CORRUPT");
}

TEST_CASE("spvFragmentOutputSignature：接口不同必然签名不同（VU-11148 的判据面）") {
	// 单输出（全屏 pass 形态）vs 五输出（G-buffer 形态）——两者混进同一 execution set 就是
	// NVIDIA 上实测的那条 VUID；这里钉住"判据能区分"。
	const auto single = makeSpv({
		inst(kOpTypeFloat, {1u, 32u}),
		inst(kOpTypePointer, {3u, kStorageOutput, 1u}),
		inst(kOpDecorate, {10u, kDecorationLocation, 0u}),
		inst(kOpVariable, {3u, 10u, kStorageOutput}),
	});
	CHECK(RGL::spvFragmentOutputSignature(single) == "loc0:f32");
	CHECK(RGL::spvFragmentOutputSignature(single) != RGL::spvFragmentOutputSignature(outputsPlusNoiseSnv()));
	CHECK(RGL::spvFragmentOutputSignature(single) != "");
}
