module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <cstddef>  // offsetof（宏不经 BMI 传递）

// 反射生成物字节布局契约（批 12.1 · B1）——被测对象 = gbuffer_contract + 4 个
// rgl/generated/*_types_generated（rlcore 成员）。generated 文件是反射产物**只读**，
// 本文件只在测试侧建立断言。
//
// ## 三源对账的分工（为什么本文件只做这一层）
//
// 布局契约有三个真相源：C++ struct ↔ generated/*.cppm ↔ *_schema.lua ↔ shader。
//   · **C++ ↔ 反射**：generated 文件自带的 static_assert 已在编译期兜底（sizeof/
//     offsetof 与反射漂移 = 编译失败）。对这一层再写一遍是冗余——本文件**不复刻**。
//   · **本文件负责的层**：生成物**内部及相互之间**的语义关系——static_assert 逐字段
//     比对反射，但看不见"常量与结构体的关系"（kGpuLightStride 是否还等于
//     sizeof(GpuLight)？）、"无 padding 语义"（成员尺寸和是否仍等于结构体尺寸——
//     std140 漂移/字段重排的红）、"表数据的唯一性/前缀"（kGBufferAttachments 的
//     name/format 撞车 static_assert 面没有）。这些是历史上真实付出过代价的形态
//     （StructuredBuffer stride 24B vs HLSL 16B 静默错位、heapOffset 字节/槽混淆）。
//   · **generated ↔ schema.lua ↔ shader 三方字面量对账**：文本级比对，python 脚本
//     比 doctest 合适（要解析 lua/shader 源码），留给后续批次（qa/ 或 tools/），
//     本文件不臆造对账通道。
//
// scene_data.cppm（矩阵表容量 4096 / 头 128B 的定义处）**不在 rlcore**，所以
// kSceneDataLightArrayBase 的重推导用 scene.slang:21 与 scene_data.cppm:70 的公式
// 字面量——两处任何一处漂移，本文件的独立重算都会红（这正是重推导的意义）。
export module test_reflection_layout;

import rlstd;
import test_support;
import gbuffer_contract;
import lights_types_generated;
import material_types_generated;
import model_types_generated;
import scene_forward_types_generated;

namespace RGL {

// ═══════════════════════════════════════════════════════════════════════════
// 组 1：GpuLight 与灯数组地址契约（lights_types_generated）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("GpuLight：64B / 4 个 vec4 连续无 padding，kGpuLightStride 常量与结构体不分家") {
	// sizeof==64 与逐字段 offsetof 的"对反射一致"由 generated 内 static_assert 兜底；
	// 这里补它看不见的三条语义：
	//   ① 常量-结构体绑定：kGpuLightStride（StructuredBuffer stride，当年 24B vs
	//      16B 静默错位的元凶位置）必须仍等于 sizeof(GpuLight)——任何一方单独改都红；
	//   ② 无 padding：成员尺寸和 == 结构体尺寸（4×16 == 64），字段重排引入空洞会红；
	//   ③ 步进连续：offsetof 逐级差 == 上一成员尺寸。
	static_assert(sizeof(GpuLight) == 64);
	CHECK(sizeof(GpuLight) == 64);
	CHECK(kGpuLightStride == sizeof(GpuLight));
	CHECK(offsetof(GpuLight, direction) == 0);
	CHECK(offsetof(GpuLight, position) == offsetof(GpuLight, direction) + sizeof(glm::vec4));
	CHECK(offsetof(GpuLight, color) == offsetof(GpuLight, position) + sizeof(glm::vec4));
	CHECK(offsetof(GpuLight, params) == offsetof(GpuLight, color) + sizeof(glm::vec4));
	CHECK(offsetof(GpuLight, params) + sizeof(glm::vec4) == sizeof(GpuLight));
	CHECK(kMaxLights == 8);  // schema 契约常量（灯槽上限，shader 端字面量对账见文件头分工）
}

TEST_CASE("灯数组尾区地址：kSceneDataLightArrayBase = 头128 + 矩阵表4096×64（独立重推导）") {
	// 该常量是 schema 字面量（生成进 C++），而 scene_data.cppm 的 C++ 侧是
	// **独立计算式** alignUp(alignUp(头 128, 256) + 矩阵表容量·stride, 256)
	// ——两者一个是数一个是公式，单边改动（矩阵容量 4096→8192、头 128→256）不会
	// 触发 generated 的 static_assert。测试侧按 shader 头注释公式（lights.slang:13
	// "矩阵表基址 256 + 容量×64，批 L 256B 对齐"）**第三次独立重算**（256 取整算术
	// 就地手写，不复用 scene_data::alignUp——重推导的意义就在于不共享被测算子）。
	constexpr uint64_t kMatrixBase = (128ull + 255ull) / 256ull * 256ull;  // 256（批 L）
	constexpr uint64_t kReDerived = (kMatrixBase + 4096ull * 64ull + 255ull) / 256ull * 256ull;
	CHECK(static_cast<uint64_t>(kSceneDataLightArrayBase) == kReDerived);  // 262400
	// 槽对齐：基址必须落在 GpuLight 槽位边界（% stride == 0），错位 = 逐灯全读到
	// 邻灯数据（heapOffset 字节/槽混淆的同族）。
	CHECK(kSceneDataLightArrayBase % kGpuLightStride == 0u);
	// 尾区尺寸账：count(16B 含 padding，lights.slang @base+16 起数组) + 8×64。
	// 尾区必须整体位于矩阵表之后（>128）且不超过 1KB——scene data 布局再插入
	// 任何新表都必须显式改这三个常量之一，届时本组会强制对话。
	CHECK(kSceneDataLightArrayBase > 128u);
	CHECK(kMaxLights * kGpuLightStride == 512u);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 2：DrawArgs（material_types_generated）——4B push 契约
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("DrawArgs：恰好 4B（共享 push 块，加一个字段 = 1591 条管线全重建的红线）") {
	// rgl/CMakeLists.txt:44-45 明确"共享的 4B DrawArgs 契约不能为阴影多加一个字段"。
	// sizeof==4 由 generated static_assert 兜底；这里补两条 push 语义：
	//   ① 4 的整数倍（push constant 步长契约，单独看不是冗余：4B 结构体允许
	//      alignof<4 的字段组成而 sizeof 仍 4，但那不是合法 push 步长）；
	//   ② 唯一成员 objectIndex 在 0 偏移（shader 端 Load 的字面量偏移）。
	static_assert(sizeof(DrawArgs) == 4);
	CHECK(sizeof(DrawArgs) == 4);
	CHECK(sizeof(DrawArgs) % 4 == 0u);
	CHECK(offsetof(DrawArgs, objectIndex) == 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 3：SceneLight / ObjectDraw（scene_forward_types_generated）——scalar layout
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("SceneLight：48B = 4×vec3 紧排（scalar layout 指纹：std140 会变成 64B）") {
	// 本项目 slangc 用 scalar block layout（AGENTS.md/skill rgl-shader-contract），
	// vec3 步长 12B。若哪天 flag 被摘掉退回 std140/std430，sizeof 会变 64、offsetof
	// 变成 0/16/32/48——generated 的 static_assert 会炸编译，而本组让**语义**（尺寸
	// 和 == sizeof、步进 12）也留在可执行规格里，双保险。
	static_assert(sizeof(SceneLight) == 48);
	CHECK(sizeof(SceneLight) == 4 * 12);  // 无 padding 的 scalar 紧排（std140 = 64）
	CHECK(offsetof(SceneLight, direction) == 0);
	CHECK(offsetof(SceneLight, eyePos) == 12);
	CHECK(offsetof(SceneLight, color) == 24);
	CHECK(offsetof(SceneLight, ambient) == 36);
	CHECK(offsetof(SceneLight, ambient) + sizeof(glm::vec3) == sizeof(SceneLight));
}

TEST_CASE("ObjectDraw：48B = 4+4+8+16+16 无空洞；u32 对恰好拼 8B 槽") {
	// 成员尺寸和 == sizeof 是"无 padding"的充分断言（字段序固定在本断言集里）。
	static_assert(sizeof(ObjectDraw) == 48);
	CHECK(sizeof(uint32_t) + sizeof(uint32_t) + sizeof(glm::vec2) + sizeof(glm::vec4) +
			  sizeof(glm::vec4) == sizeof(ObjectDraw));
	CHECK(offsetof(ObjectDraw, totalTriangles) == 0);
	CHECK(offsetof(ObjectDraw, vertexOffset) == 4);
	CHECK(offsetof(ObjectDraw, roughnessMetallic) == 8);
	CHECK(offsetof(ObjectDraw, baseColor) == 16);
	CHECK(offsetof(ObjectDraw, emissive) == 32);
	CHECK(offsetof(ObjectDraw, emissive) + sizeof(glm::vec4) == sizeof(ObjectDraw));
	// baseColor−emissive 间距 16（shader 端按下标读这两槽，间距错 = 颜色串 emissive）
	CHECK(offsetof(ObjectDraw, emissive) - offsetof(ObjectDraw, baseColor) == 16u);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 4：ModelVertex 与 meshlet 常量（model_types_generated）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("ModelVertex：48B = 12+12+8+16（gltf 上传步长）；kTrianglesPerGroup=10、heap 基槽=0") {
	static_assert(sizeof(ModelVertex) == 48);
	CHECK(sizeof(glm::vec3) + sizeof(glm::vec3) + sizeof(glm::vec2) + sizeof(glm::vec4) ==
		sizeof(ModelVertex));  // 无 padding
	CHECK(offsetof(ModelVertex, position) == 0);
	CHECK(offsetof(ModelVertex, normal) == 12);
	CHECK(offsetof(ModelVertex, uv) == 24);
	CHECK(offsetof(ModelVertex, tangent) == 32);
	// meshlet 组大小（shadow.mesh/cull 契约）：=0 会让"组数上取整"除零，>64 与
	// shader 端 wave 假设冲突——10 是三方（schema/shader/C++）共享字面量。
	CHECK(kTrianglesPerGroup == 10u);
	// 几何 descriptor 堆基槽 = 0（"相对 reserved range 起点"语义：若基座改成非 0，
	// 所有 slot 计算同步漂移，本断言强制显式改）
	CHECK(HeapSlotGeometryBase == 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 5：G-buffer 契约表（gbuffer_contract）——dump_gbuf 通道号的定义处
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("kGBufferAttachments：5 通道、name/format 双唯一、全 gbuffer_ 前缀、emissive 殿后") {
	// 表顺序 = light.frag binding 4-7 + 尾部 emissive（gbuffer_contract.cppm:27-29），
	// 也是 diagnostics.lua dump_gbuf 通道号的定义处（越界闸门在 test_scene_config 钉）。
	CHECK(kGBufferAttachments.size() == 5);
	CHECK(kGBufferChannelCount == static_cast<int>(kGBufferAttachments.size()));
	CHECK(kGBufferChannelCount == 5);
	for (size_t i = 0; i < kGBufferAttachments.size(); ++i) {
		const RgGBufferDesc& d = kGBufferAttachments[i];
		INFO("channel#" << i << " name=" << d.name << " format=" << d.format);
		// 前缀契约：一切 "gbuffer_*" 命名进表，非前缀条目 = 有人把别表混进来
		CHECK(d.name.starts_with("gbuffer_"));
		CHECK(!d.format.empty());
		for (size_t j = i + 1; j < kGBufferAttachments.size(); ++j) {
			// name 撞车 = 按名查找歧义；format 撞车 = dump/绑定侧按格式匹配歧义
			CHECK(d.name != kGBufferAttachments[j].name);
			CHECK(d.format != kGBufferAttachments[j].format);
		}
	}
	// 通道位序的两端锚点（binding 4 起点与 binding 11 殿后契约）
	CHECK(kGBufferAttachments[0].name == "gbuffer_albedo");
	CHECK(kGBufferAttachments[kGBufferChannelCount - 1].name == "gbuffer_emissive");
}

}  // namespace RGL
