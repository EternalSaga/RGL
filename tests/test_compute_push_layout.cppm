module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <cstddef>  // offsetof（宏不经 BMI 传递）
#include <fstream>
#include <sstream>
#include <string>

// compute push 契约的字节布局回归——被测对象 = rgl/generated/compute_push_types_generated.cppm
// （compute-push 反射管线的产物，rlbase 成员）。生成物只读，本文件只在测试侧建立断言。
//
// ## 为什么值得钉（这一刀买的东西）
//
// 替换前，evssm / izb 的 push 布局是**手抄进 C++ 的镜像 struct**，配一组
// `static_assert(offsetof(MomentsPush, mapSize) == 0, …)`。那组 assert 量的是**手抄的 C++ struct
// 本身**，不是 shader ⟹ 它是关于 C++ 侧的同义反复：shader 把 `mapSize` 与 `cols` 换序而 C++ 没跟着
// 动时，三条 assert 全过，C++ 写 mapSize@0 而 shader 读 mapSize@4 —— 静默。EVSSM/IZB 又只在
// shadow.lua 开门时构造，症状落在渲染上（mask 几何错位 / 矩图糊），离真因三跳。
//
// 现在布局由反射派生，"C++ 与 shader 一致"这一层由生成物自带的 sizeof/offsetof assert 在编译期兜底
// （那份 assert 量的是编译器实查的 shader 布局）。**本文件负责的是生成物看不见的另一层**：生成物
// 只说"shader 里是这样"，不说"shader 里**该**是那样"。期望值出处 = 下面逐条注明的 shader 源码字段
// 声明序 + scalar block layout（逐字段紧排、所有类型对齐 4、零 padding）手算。若哪天反射管线本身
// 坏了（丢 flag 退回 std140、判别子捞进真 ConstantBuffer、模板渲染漏字段），生成物会忠实地跟着坏，
// 而本文件红。
//
// ## 🔴 变异红线声明（哪个变异让我红）
//
//   ① **改 shader 字段顺序**：把 `evssm_moments.comp.slang` 的 `mapSize` 与 `cols` 换序 →
//      `just reflect` → 本文件「八枚 push struct」用例**必须红**（EvssmMomentsPush 的
//      mapSize 期望 @0 实得 @4）。这正是旧的同义反复 assert 抓不到的那种变异——它当时全绿。
//      （同时产品代码也会红：`{mapSize, cols, exponent}` 按位置发字段，值会串位但编得过，
//      所以**只有本文件**能把它拦在测试里。）
//   ② **把某枚 shader 从管线名单里删掉**（cmake/RglReflection.cmake 的
//      rgl_reflect_compute_push 参数表少一行）→ 「总数锚」用例**必须红**（8 → 7）。
//      同一条也覆盖"某枚 shader 哪天把 push constant 删了"：工具端的 selectivity 自检会先
//      让 just reflect 直接失败，而名单层面的漏登记只有这条锚看得见。
//   ③ **把 meshlet_cull.comp.slang 加进那条名单**（造第二个 MeshletCullPush 真相源）→
//      「反向用例」与「总数锚」**双双红**（实测：`export struct` 数 8→9 与 `static_assert(sizeof(`
//      数 8→9 各红一条，`MeshletCullPush` 那条红一条）。⚠ 实测还有一层更早的表现：两个模块
//      导出同名 `RGL::MeshletCullPush` 会让 clang 22 在部分 TU 上直接编译失败（实录 =
//      test_render_list_light.cppm 优化器段错误 exit 135）⟹ 这条变异的真话是「先编不过，
//      编得过也测不过」，两种都拦得住，但别把它读成"只有断言在拦"。
//   ④ **改 shader 字段名**（izb_create 的 `pixelW` 改名）→ 本文件不红，**产品代码编译失败**
//      ——那是这一刀真正的产品（布局第一次有出处），写在这里是为了别让后来人误以为 ④ 归本文件管。
//      实测：`rgl/izb_shadow_impl.cppm:295: error: no member named 'pixelW' in 'RGL::IzbCreatePush'`。
//
// ## 为什么"总数"与"反向"两条要读产物文本而不是写 C++ 断言
//
// C++ 里探测"某个名字存不存在"是**硬错误**而不是 SFINAE（`requires { typename RGL::X; }` 里 X
// 不依赖模板参数 ⟹ 名字查找失败发生在概念定义点，直接编译失败，实测 clang 22 报 "no type named
// 'Absent' in namespace"）。所以"生成物里恰好 8 个 struct""生成物里没有 MeshletCullPush"这两条
// 在类型层面写不出来，只能数产物文本里的 `export struct`。路径经 tests/CMakeLists.txt 的
// RGL_COMPUTE_PUSH_CPPM 传入（生成物是**已提交文件**，源树路径稳定）。

export module test_compute_push_layout;

import rlstd;
import compute_push_types_generated;

namespace RGL {

namespace {

// 读生成物文本。失败返回空串——调用方**必须**先 REQUIRE 它非空再判内容：把"读不到"与"内容里没有"
// 混成同一个结果，反向用例就恒真（假绿）。
std::string readProduct() {
	std::ifstream in(RGL_COMPUTE_PUSH_CPPM);
	if (!in.is_open())
		return {};
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

// 统计非重叠出现次数（数量锚点用；std::ranges 没有现成计数子串的东西，就地手写一个，
// 不复用被测机制）。
size_t countOccurrences(const std::string& text, std::string_view needle) {
	size_t n = 0;
	for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1))
		++n;
	return n;
}

}  // namespace

TEST_CASE("八枚 compute push struct：sizeof 与逐字段 offsetof（期望值 = shader 声明序手算）") {
	// ── cull.comp.slang `struct CullPush { float4 planes[6]; uint sequenceCount; }` ──
	// scalar layout：float4 = 16B、数组紧排 ⟹ planes @0 占 96B，sequenceCount @96，总 100。
	// 100 ≤ maxPushDataSize 且不是 16 的倍数——push 区没有"必须补齐到 16"的规矩（VkPushData 是
	// 任意字节数），所以"总尺寸是 4 的倍数"才是这里的 scalar 指纹，16 的倍数不是。
	CHECK(sizeof(CullPush) == 100);
	CHECK(offsetof(CullPush, planes) == 0);
	CHECK(offsetof(CullPush, sequenceCount) == 96);
	CHECK(sizeof(glm::vec4) * 6 == 96);  // planes 的宽度 = 期望偏移的唯一算式

	// ── env_prefilter.comp.slang `struct EnvPrefilterPush { uint level; uint mipLevels; uint faceSize; }` ──
	CHECK(sizeof(EnvPrefilterPush) == 12);
	CHECK(offsetof(EnvPrefilterPush, level) == 0);
	CHECK(offsetof(EnvPrefilterPush, mipLevels) == 4);
	CHECK(offsetof(EnvPrefilterPush, faceSize) == 8);

	// ── evssm_downsample.comp.slang `struct EvssmDownPush { uint level; uint cols; uint baseTileTexels; }` ──
	CHECK(sizeof(EvssmDownPush) == 12);
	CHECK(offsetof(EvssmDownPush, level) == 0);
	CHECK(offsetof(EvssmDownPush, cols) == 4);
	CHECK(offsetof(EvssmDownPush, baseTileTexels) == 8);

	// ── evssm_moments.comp.slang `struct EvssmMomentsPush { uint mapSize; uint cols; float exponent; }` ──
	// 🔴 变异红线 ① 的落点：换序 mapSize/cols 后这两条会互相指错（mapSize 期望 @0 实得 @4）。
	CHECK(sizeof(EvssmMomentsPush) == 12);
	CHECK(offsetof(EvssmMomentsPush, mapSize) == 0);
	CHECK(offsetof(EvssmMomentsPush, cols) == 4);
	CHECK(offsetof(EvssmMomentsPush, exponent) == 8);

	// ── stream_fanout.comp.slang `struct FanoutPush { uint sequenceCount; }` ──
	CHECK(sizeof(FanoutPush) == 4);
	CHECK(offsetof(FanoutPush, sequenceCount) == 0);

	// ── izb_create.comp.slang `struct IzbCreatePush { float4 invVPM[4]; float4 lightVP[4];
	//       int pixelW; int pixelH; int gridDim; float emptyDepth; }` ──
	// float4[4] = 4×16 = 64B（列主序 mat4 的内存同构体），两段矩阵 @0/@64，四个标量 @128/132/136/140。
	CHECK(sizeof(IzbCreatePush) == 144);
	CHECK(offsetof(IzbCreatePush, invVPM) == 0);
	CHECK(offsetof(IzbCreatePush, lightVP) == 64);
	CHECK(offsetof(IzbCreatePush, pixelW) == 128);
	CHECK(offsetof(IzbCreatePush, pixelH) == 132);
	CHECK(offsetof(IzbCreatePush, gridDim) == 136);
	CHECK(offsetof(IzbCreatePush, emptyDepth) == 140);
	// 矩阵段宽度的独立算式：手写版当年把 invVPM 写成 glm::mat4，生成物是 std::array<glm::vec4, 4>
	// ——两者必须同宽，否则 izb_shadow_impl 的逐列赋值会静默少拷一列而 sizeof 仍"对"。
	CHECK(sizeof(std::array<glm::vec4, 4>) == sizeof(glm::mat4));

	// ── izb_prime.comp.slang `struct IzbPrimePush { int gridDim; int pixelTotal; int _pad0; int _pad1; }` ──
	// 补齐位是 shader 侧的**真字段**（不是 C 侧 padding），所以它们进生成物、也进本断言：
	// 删掉 _pad0 = 后面所有字段整体上移，这条比 sizeof 更早点名。
	CHECK(sizeof(IzbPrimePush) == 16);
	CHECK(offsetof(IzbPrimePush, gridDim) == 0);
	CHECK(offsetof(IzbPrimePush, pixelTotal) == 4);
	CHECK(offsetof(IzbPrimePush, _pad0) == 8);
	CHECK(offsetof(IzbPrimePush, _pad1) == 12);

	// ── mip_gen.comp.slang `struct MipGenPush { uint srcMip; uint dstWidth; uint dstHeight; }` ──
	CHECK(sizeof(MipGenPush) == 12);
	CHECK(offsetof(MipGenPush, srcMip) == 0);
	CHECK(offsetof(MipGenPush, dstWidth) == 4);
	CHECK(offsetof(MipGenPush, dstHeight) == 8);
}

TEST_CASE("数量锚点：生成物里恰好 8 个 struct（漏登记一枚 shader / 漏一枚 push 契约都红）") {
	// 🔴 先钉"读到了真文件"，再钉总数——空文本上的 count==8 是假绿，而"文件读不到"恰恰是
	// 路径写错、生成物被删这类真事故的样子。
	const std::string text = readProduct();
	INFO("path = " << RGL_COMPUTE_PUSH_CPPM);
	REQUIRE(!text.empty());
	// 8 = cmake/RglReflection.cmake 里 rgl_reflect_compute_push 的 shader 名单长度。
	// 变异红线 ②：那条名单少一行 → 这里变 7 → 红。
	CHECK(countOccurrences(text, "export struct ") == 8);
	// 每个 struct 都得带尺寸 assert（模板按 size>0 才发；一条 size 静默变 0 = 那一枚的布局
	// 从此没有编译期守卫，而总数仍是 8）。
	CHECK(countOccurrences(text, "static_assert(sizeof(") == 8);
}

TEST_CASE("反向用例：生成物里没有 MeshletCullPush（同一个 push 契约只准有一个真相源）") {
	// meshlet_cull.comp.slang 的 push（`cullPush` → MeshletCullPush）已由 meshlet_cull_schema.lua
	// 那条 slang 管线产出（generated/meshlet_cull_types_generated.cppm）。compute-push 管线的名单里
	// 刻意不列它——列了就等于给同一份布局开两个生成来源，两边哪天漂了没人能判哪份对。
	// 变异红线 ③：把它加进名单 → 本用例与总数锚双双红。
	const std::string text = readProduct();
	REQUIRE(!text.empty());
	// 反向用例的输入形态必须**能**违反这条契约：先证明这份文本确实是那份生成物（里面有别的
	// struct），否则"没有 MeshletCullPush"在一份空文件上也成立。
	REQUIRE(text.find("export struct CullPush") != std::string::npos);
	CHECK(text.find("MeshletCullPush") == std::string::npos);
}

}  // namespace RGL
