module;
#include <doctest/doctest.h>
#include <fstream>
#include <initializer_list>  // 范围 for / 聚合初始化的花括号列表（modules 下不从 import 带进来）
#include <set>
#include <string>
#include <vector>

/**
 * test_shader_mapping —— 批 M「只烘 SPV 真声明的 binding」的契约门（★M-3T-a 起 stage 数 2~3）
 *
 * ## 为什么值得钉（本批 ★M-3T-a 实踩的回归，2026-10-13）
 *
 * task stage 变成可选之后，"这个条目有没有 task"和"这个 stage 的 SPV 解析失败"曾被写成**同一个值**
 * （`std::nullopt`）⟹ 全部 479 条被误判成解析失败 → 退回全烘 + 启动刷屏。而**终图逐位不变**
 * （全烘是"恒正确"的失效方向），A3 主判据照样绿 ⟹ 逐位判据对这一类回归**天然失明**，
 * 当时唯一的信号是人眼扫到 `warnings=479`。文档只记录不阻止，所以这条落成断言。
 *
 * ## 阳性锚点（变异红线）
 *
 * 每条变异都是"改坏被测实现"，**逐条实测过红**（2026-10-13，五条全红 + 每次还原重编后基线全绿）：
 *   1. 把"不存在的 stage"重新塞回列表当 nullopt 哨兵（= 本批实踩的原始 bug）→ 用例 1、2、4 红。
 *   2. 删掉 `if (!effect.taskStageSpv.empty()) push_back(...)`（= task 的声明漏进并集）
 *      → 用例 2、3、4 红（用例 3 顺带红：task 不入列表，它的坏码流就没人触发 nullopt）。
 *   3. 把解析失败分支的 `return std::nullopt` 改成 `continue`（= 静默吞掉坏码流）→ 用例 3 红。
 *   4. 过滤条件**漏掉 `!`**（`!declared->contains(x)` → `declared->contains(x)`，只烘没声明的）
 *      → 用例 4 红。这是"烘错集合"最真实的漂移形态。
 *   5. 弱化成 `declared->empty()`（非空就全烘，= 过滤失效）→ 用例 4 红。
 * ⚠ 想验的"把过滤整条删掉"**编不过**：`filter` 与 `declared` 双双变 unused 撞 `-Werror`，拿到的是
 *   「编译失败」不是「判据红」。谓词类变异必须保留参数使用（→ skill rgl-test-discipline），
 *   本用例因此用 4/5 两种"保留使用的失效形态"覆盖同一维度。
 * 期望值出处 = 产物的 SPIR-V 接口（`spirv-dis` 数 `OpDecorate ... Binding`），不抄被测码。
 *
 * ## 为什么在 `just test` 而不是 `TEST_SUITE("gpu")`
 *
 * 被测两个函数是**纯函数**（输入码流 + binding 形态，输出集合 / 条数），不要 VkDevice、不建任何
 * 设备对象 ⟹ 按分层判据（"这段逻辑需不需要一个 VkDevice 才能跑"）它属于无设备层。
 */

export module test_shader_mapping;

import rlstd;
import rlvk;    // vk::DeviceSize（PipelineBindings/ExtraBinding 的字段类型）
import shader;  // ShaderEffect / PipelineBindings / 被测两函数

using RGL::PipelineBindings;
using RGL::PipelineExtraBinding;
using RGL::ShaderEffect;
// 两函数住在 shader 模块的内部细节命名空间（导出仅为可测性，不是给运行时调用面的）
using RGL::shaderDetail::bakeBindingMappings;
using RGL::shaderDetail::declaredBindingsUnion;

namespace {

	/** 读 SPV 码流。文件缺失/长度非 4 倍数 = REQUIRE **响亮失败**（静默跳过 = 假绿）。 */
	std::vector<uint32_t> readSpv(const std::string& path) {
		std::ifstream f(path, std::ios::binary | std::ios::ate);
		REQUIRE(f);
		const auto n = static_cast<size_t>(f.tellg());
		REQUIRE(n % 4u == 0u);
		std::vector<uint32_t> code(n / 4u);
		f.seekg(0);
		f.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(n));
		return code;
	}

	// ── 期望值（实测出处，非被测码）───────────────────────────────────────────────
	// spirv-dis 数 OpDecorate ... Binding：
	//   mdi_shadow_rows.mesh.spv = {0}      mdi_spike.frag.spv = {}（无 descriptor）
	//   mapping_task.task.spv    = {3}      ← 专为本用例造，tests 里无任何 mesh/frag 用 binding 3
	// 若哪天这些产物的接口变了，下面的断言会红并逼人来这里同步——这是**想要的**行为（产物接口
	// 就是契约），不是"测试太脆"。
	const std::set<uint32_t> kNoTaskDeclared{0};
	const std::set<uint32_t> kWithTaskDeclared{0, 3};

	/** mesh + frag 两阶段（taskSpv 留空 = 绝大多数条目的形态）。 */
	ShaderEffect makeNoTask() {
		ShaderEffect e;
		e.meshStageSpv = readSpv(RGL_TEST_SHADER_DIR "/mdi_shadow_rows.mesh.spv");
		e.fragStageSpv = readSpv(RGL_TEST_SHADER_DIR "/mdi_spike.frag.spv");
		return e;
	}

	/** 在上面那份上挂 task 码流（= ★M-3T-a 的 `shadow_meshlet_mdi` 条目形态）。 */
	ShaderEffect makeWithTask() {
		ShaderEffect e = makeNoTask();
		e.taskStageSpv = readSpv(RGL_TEST_SHADER_DIR "/mapping_task.task.spv");
		return e;
	}

}  // namespace

TEST_CASE("mapping_union：无 task stage 的条目不得被判成解析失败（全烘回退只准真失败触发）") {
	const auto effect = makeNoTask();
	REQUIRE(effect.taskStageSpv.empty());  // 前提：本用例测的就是"没有 task"这一格

	const auto merged = declaredBindingsUnion(effect, "no_task");
	// 🔴 本行就是本批实踩回归的断言：nullopt = 退回全烘。把"不存在的 stage"当 nullopt 哨兵时，
	// 这里拿到的是 nullopt（479 条全中），而终图逐位不变 ⟹ 逐位判据看不见它。
	REQUIRE(merged.has_value());
	// 阳性锚点：光"有值"不够——恒返回空集的实现同样"有值"。集合内容才证明解析真跑了。
	CHECK(*merged == kNoTaskDeclared);
}

TEST_CASE("mapping_union：task 声明的 binding 必须进并集（漏掉 = VUID-11292 启动炸的方向）") {
	auto effect = makeNoTask();
	const auto before = declaredBindingsUnion(effect, "before_task");
	REQUIRE(before.has_value());
	// 前提守卫：mesh + frag 这一对**不**声明 3，否则"加了 task 集合才含 3"这个判据是空的。
	CHECK(before->count(3u) == 0);

	effect.taskStageSpv = readSpv(RGL_TEST_SHADER_DIR "/mapping_task.task.spv");
	const auto after = declaredBindingsUnion(effect, "with_task");
	REQUIRE(after.has_value());  // 有 task 也不得退全烘（用例 1 的对称半边）
	CHECK(*after == kWithTaskDeclared);
}

TEST_CASE("mapping_union：任一 stage 码流坏掉 = nullopt（三侧各自都要能触发，且不得静默吞）") {
	// 合法长度 + 错魔数 = spvSplit 失败（源码注释：「魔数不对/截断 = 解析失败，不是『什么都不声明』」）
	const std::vector<uint32_t> junk{0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u};

	auto effect = makeNoTask();
	effect.meshStageSpv = junk;
	CHECK_FALSE(declaredBindingsUnion(effect, "junk_mesh").has_value());

	effect = makeNoTask();
	effect.fragStageSpv = junk;
	CHECK_FALSE(declaredBindingsUnion(effect, "junk_frag").has_value());

	// task 这一侧单独测：它只在"有 task"时才进列表，漏测 = 整条 task 分支无判据。
	effect = makeWithTask();
	effect.taskStageSpv = junk;
	CHECK_FALSE(declaredBindingsUnion(effect, "junk_task").has_value());
}

TEST_CASE("mapping_bake：烘的恰好是声明的（消费者侧条数；task 的声明要能多烘出一条）") {
	// 请求集 = base {0,1} + extra {3, 9}。3 只有 task 声明、9 无人声明 ⟹ 两个方向都有信号：
	// task 漏进并集 → 少烘 3；过滤失效 → 多烘 9。
	PipelineBindings bindings;
	bindings.descriptorSize = 64;
	bindings.extra = {PipelineExtraBinding{3, false, 7, 0}, PipelineExtraBinding{9, false, 8, 0}};

	VkDescriptorSetAndBindingMappingEXT out[48]{};

	const auto noTask = makeNoTask();
	// 声明 = {0} ⟹ base 里 binding 1 被裁、extra 两条都被裁 ⟹ 只剩 binding 0
	CHECK(bakeBindingMappings(noTask, bindings, "bake_no_task", out) == 1);

	const auto withTask = makeWithTask();
	const uint32_t n = bakeBindingMappings(withTask, bindings, "bake_with_task", out);
	CHECK(n == 2);  // 少一条 = task 的声明没进并集
	if (n == 2) {
		// running index 序（base 先、extra 续）也是契约的一部分：只钉条数会漏掉"烘错了槽"
		CHECK(out[0].firstBinding == 0u);
		CHECK(out[1].firstBinding == 3u);
	}
}
