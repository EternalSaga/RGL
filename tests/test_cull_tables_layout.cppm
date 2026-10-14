module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <cstddef>  // offsetof（宏不经 BMI 传递）
#include <fstream>
#include <sstream>
#include <string>
#include <vulkan/vulkan.h>  // VkDrawMeshTasksIndirectCommandEXT（序列 @108 那 12B 的真身）

/**
 * test_cull_tables_layout —— GPU culling 三张 buffer 元素契约的**字节布局阳性锚点**
 *
 * ## 被测对象与它买的东西
 *
 * 被测 = `rgl/generated/cull_tables_types_generated.cppm`（`cull_tables.slang` 经反射管线的产物）。
 * 迁移前这份布局有三份拼写：C++ 侧 `gpu_cull.cppm::CullEntry` + `kCullSequenceStride` +
 * `kCullSeq*` 四常量，`cull.comp.slang` 的 `kEntryStride = 60u` / `+108u` 字面量，
 * `stream_fanout.comp.slang` 的 `kSequenceStride = 120u` / `8u..120u` 拷贝段。三条
 * `static_assert` 只量 C++ 单边（关于手抄副本的同义反复），shader 侧改布局静默。
 *
 * 迁移后两侧都从生成物取类型，**本文件负责生成物自己看不见的另一层**：生成物只说"shader 里
 * 是这样"，不说"shader 里**该**是那样"。
 *
 * ## 🔴 期望值的出处（判据不抄生成物、不抄 schema）
 *
 * 下面每个数字来自**迁移前人工核对过的字节表**：
 *   cullTable 行 60B：center @0 / radius @12 / objectSlot @16 / pipelineIndex @20 / groupCount @24
 *   sequences 行 120B：execSlotFrag @0 / execSlotMesh @4 / DrawArgs @8 / 保留 @12..108 / meshCmd @108
 *   countBuf 元素 8B：保留 @0 / total @4
 * 这三个类型同时被 `cull_tables_schema.lua` 声明一遍（reflect 期交叉校验 = 变异拦截点）。**本文件
 * 是第三份**，用途不同：schema 红了说明"shader 改了没同步表"，本文件红了说明"shader 与表**一起**
 * 改了、或反射管线本身坏了"（丢 `-fvk-use-scalar-layout` → 含 vec4 的 CullEntry 立刻漂）。
 *
 * ## 🔴 变异红线声明（哪个变异让我红）
 *
 *   ① **改契约字段类型**（`cull_tables.slang` 的 `objectSlot` uint → float）→ `just reflect`
 *      **先于本文件**红（schema 交叉校验 [FAIL]，生成物不产出）。本文件是它的第二道：若有人绕过
 *      schema 直接改生成物，`offsetof(CullEntry, objectSlot) == 16` 这条红。
 *   ② **同类型字段换序**（`meshCmdX` 与 `meshCmdY` 互换）→ `just reflect` 红（offset 108/112 对调）。
 *      ⚠ 本仓实测过「C++ 写入点用位置聚合初始化会让改名/换序双双静默通过」（债-118）⟹ 产品侧的
 *      对应防线是 `main_scene.cppm` 的 CullEntry **指定初始化**，不在本文件。
 *   ③ **把 meshCmd 挪 4B**（等价于 stride 漂）→ `just reflect` 红；本文件的
 *      `offsetof(meshCmdX) == 108` 与「meshCmd 恰好收尾于 stride 末端」两条同时红。
 *   ④ **把某枚类型从 schema 提取清单里删掉** → 生成物少一个 struct ⟹ 「总数锚」红（3 → 2）。
 *   ⑤ **把 `drawArgs` 改成嵌套 `DrawArgs`**（给同一布局造第二个真相源）→ 「反向用例」红，
 *      且实测更早就编不过（两个模块导出同名 `RGL::DrawArgs`，先例 = test_compute_push_layout ③）。
 *   ⑥ **中心从三个 float 退回 `float3`** → 本文件**不红**（scalar layout 下同宽），这是**故意**的：
 *      三个 float 买的是"掉 flag 时 schema 交叉校验会红"，不是"布局不同"。别把它当本文件的判据。
 */
export module test_cull_tables_layout;

import rlstd;
import cull_tables_types_generated;
import material_types_generated;  // DrawArgs（序列 @8 槽区的真身，4B = 仅 objectIndex）

namespace RGL {

namespace {

/// 读生成物文本。失败返回空串——调用方**必须**先 REQUIRE 它非空再判内容：把"读不到"与
/// "内容里没有"混成同一个结果，反向用例就恒真（假绿）。
std::string readProduct() {
	std::ifstream in(RGL_CULL_TABLES_CPPM);
	if (!in.is_open())
		return {};
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

/// 统计非重叠出现次数（数量锚点用；不复用被测机制）。
size_t countOccurrences(const std::string& text, std::string_view needle) {
	size_t n = 0;
	for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1))
		++n;
	return n;
}

}  // namespace

TEST_CASE("CullEntry：60B 与逐字段偏移（迁移前字节表逐位复现）") {
	// ── cull_tables.slang `struct CullEntry`（scalar block layout：逐字段紧排、对齐 4、零 padding）──
	// 变异红线 ①：objectSlot 改成 float ⟹ schema 交叉校验先红（类型串 uint32_t ≠ float）；
	// 本组守的是"绕过 schema 直接改生成物"那条路。
	CHECK(sizeof(CullEntry) == 60);
	CHECK(offsetof(CullEntry, centerX) == 0);
	CHECK(offsetof(CullEntry, centerY) == 4);
	CHECK(offsetof(CullEntry, centerZ) == 8);
	CHECK(offsetof(CullEntry, radius) == 12);
	CHECK(offsetof(CullEntry, objectSlot) == 16);
	CHECK(offsetof(CullEntry, pipelineIndex) == 20);
	CHECK(offsetof(CullEntry, groupCount) == 24);
	CHECK(offsetof(CullEntry, totalTriangles) == 28);
	CHECK(offsetof(CullEntry, vertexOffset) == 32);
	CHECK(offsetof(CullEntry, baseColor) == 36);
	CHECK(offsetof(CullEntry, roughnessMetallic) == 52);

	// 🔴 中心三标量与迁移前的 `glm::vec3` **同宽**：这是"float3 → 三个 float 的退化逐字节不变"的
	// 直接证据。它不是风格断言——若哪天有人把中心改成 float4（或有人以为 float3 对齐到 16 而
	// 手工补白），这条先于 sizeof 点名，而 sizeof 那条会跟着一起红但指不到根因。
	CHECK(offsetof(CullEntry, radius) - offsetof(CullEntry, centerX) == sizeof(glm::vec3));
	// 末字段恰好收尾于 stride 末端（中间没有藏 padding）：60 = 52 + 8 的唯一算式。
	CHECK(offsetof(CullEntry, roughnessMetallic) + sizeof(glm::vec2) == sizeof(CullEntry));
	// shader 侧读的最后一个字段必须整只落在行内（步长错了才会整表错位画错）。
	CHECK(offsetof(CullEntry, groupCount) + 4 <= sizeof(CullEntry));
}

TEST_CASE("CullSequence：120B 与逐字段偏移 + 旧三条 static_assert 的不变式在此接棒") {
	// ── cull_tables.slang `struct CullSequence` ──
	// 变异红线 ②③ 的落点：换序 meshCmdX/Y、或把 meshCmd 挪 4B，下面两条立刻互相指错。
	CHECK(sizeof(CullSequence) == 120);
	CHECK(offsetof(CullSequence, execSlotFrag) == 0);
	CHECK(offsetof(CullSequence, execSlotMesh) == 4);
	CHECK(offsetof(CullSequence, drawArgs) == 8);
	CHECK(offsetof(CullSequence, drawArgsSlack) == 12);
	CHECK(offsetof(CullSequence, meshCmdX) == 108);
	CHECK(offsetof(CullSequence, meshCmdY) == 112);
	CHECK(offsetof(CullSequence, meshCmdZ) == 116);

	// ── 旧 gpu_cull.cppm 三条 static_assert 的**同一批不变式**，改写在生成物上 ──
	// （原三条量的是手抄常量，本组量的是反射实查的 shader 布局；不变式一条没丢。）
	// #1 execSlot 2×u32 @0/@4 与 DrawArgs@8 相邻：DGC EXECUTION_SET token 按 shaderStages 位
	//     LSB→MSB 读**连续** 2 个 u32，中间插任何东西 = execution set 下标读错。
	CHECK(offsetof(CullSequence, execSlotFrag) + 4 == offsetof(CullSequence, execSlotMesh));
	CHECK(offsetof(CullSequence, execSlotMesh) + 4 == offsetof(CullSequence, drawArgs));
	// #2 meshCmd 12B 必须恰好收尾于 stride 末端（否则 DGC DRAW_MESH_TASKS token 越出序列槽）。
	CHECK(offsetof(CullSequence, meshCmdX) + sizeof(VkDrawMeshTasksIndirectCommandEXT)
		== sizeof(CullSequence));
	// #3 DrawArgs 不得超出 @8..108 的 100B 保留槽区（DGC PUSH_DATA token 按 sizeof(DrawArgs) 推）。
	CHECK(sizeof(DrawArgs)
		<= offsetof(CullSequence, meshCmdX) - offsetof(CullSequence, drawArgs));

	// ── 独立算式（不抄生成物、不抄 schema 的第三份）──
	// 保留槽区宽度 = 96B：迁移前那句「@8..108 共 100B 保留宽度，DrawArgs 本身 4B，余 96B 空置」
	// 的算术本体。slack 少一个 u32 = 步长从 120 变 116 = 整条序列错位。
	CHECK(sizeof(CullSequence::drawArgsSlack) == 96);
	CHECK(offsetof(CullSequence, drawArgsSlack) + sizeof(CullSequence::drawArgsSlack)
		== offsetof(CullSequence, meshCmdX));
	// DGC layout TOC 的处理序 = 序列偏移升序（vk_indirect_commands 的 token 书写序依赖它）：
	// EXECUTION_SET < PUSH_DATA < DRAW_MESH_TASKS。生成物字段序一旦重排而 token 书写序没跟上，
	// 驱动按偏移重排后语义就变了，这条是那条前提的机器版本。
	CHECK(offsetof(CullSequence, execSlotFrag) < offsetof(CullSequence, drawArgs));
	CHECK(offsetof(CullSequence, drawArgs) < offsetof(CullSequence, meshCmdX));
	// 步长上限护栏（迁移前写在注释里：≤ RADV maxIndirectCommandsIndirectStride 2048）。
	CHECK(sizeof(CullSequence) <= 2048);
}

TEST_CASE("CullCount：8B，@4 = DGC sequenceCountAddress 与 countTotalAddress() 的落点") {
	CHECK(sizeof(CullCount) == 8);
	CHECK(offsetof(CullCount, reserved) == 0);
	// 🔴 这个 4 就是 GpuCull::countTotalAddress() 里那个 `+ 4`（DGC sequenceCountAddress）。
	// 迁移前它是裸字面量，shader 侧的 `countBuf.Store(4u, …)` 是第二个裸字面量。
	CHECK(offsetof(CullCount, total) == 4);
	CHECK(offsetof(CullCount, total) + 4 == sizeof(CullCount));
}

TEST_CASE("数量锚点：生成物里恰好 3 个 struct（提取清单少一枚都红）") {
	// 🔴 先钉"读到了真文件"，再钉总数——空文本上的 count==3 是假绿。
	const std::string text = readProduct();
	INFO("path = " << RGL_CULL_TABLES_CPPM);
	REQUIRE(!text.empty());
	// 3 = cull_tables_schema.lua 的 vertexLayouts 条目数。变异红线 ④：少一枚 → 2 → 红。
	CHECK(countOccurrences(text, "export struct ") == 3);
	// 每个 struct 都得带尺寸 assert（模板按 size>0 才发；一条 size 静默变 0 = 那一枚的布局
	// 从此没有编译期守卫，而总数仍是 3）。
	CHECK(countOccurrences(text, "static_assert(sizeof(") == 3);
}

TEST_CASE("反向用例：生成物里没有第二个 DrawArgs（序列 @8 的退化是定案，不是省事）") {
	// 序列 @8 的真身是 material_types_generated 的 `RGL::DrawArgs`。把它当**嵌套契约**列进
	// cull_tables_schema.lua 的 vertexLayouts ⟹ 本生成物再导出一份同名 struct（schema 的嵌套
	// 合法判据就是"出现在本表 vertexLayouts 名单里"，见 schema_loader.cppm::vertexAttrCppType）。
	// 两个模块导出同名 RGL::DrawArgs = 同一布局两个真相源，先例判罚见文件头变异红线 ⑤。
	const std::string text = readProduct();
	REQUIRE(!text.empty());
	// 反向用例的输入形态必须**能**违反这条契约：先证明这份文本确实是那份生成物（里面有别的
	// struct），否则"没有 DrawArgs"在一份空文件上也成立。
	REQUIRE(text.find("export struct CullSequence") != std::string::npos);
	// 字段名是小写开头的 drawArgs；这里找的是**类型名** DrawArgs（大小写敏感）。
	CHECK(text.find("DrawArgs") == std::string::npos);
}

}  // namespace RGL
