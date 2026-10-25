module;
/**
 * test_ssr_viewproj —— Phase 15 批 15.1 SSR 尾区（kSceneDataSsrOffset，载体 =
 *  ConstantBuffer<SsrParamsBlock> binding 31）的字节布局契约 + 抖动口径契约（纯 CPU、无设备）。
 *
 * ## 钉什么、不钉什么
 *
 * (a) **布局/互逆**：按 vkrenderer_impl::writeSceneStaging 的口径把 ssrViewProj 随
 *     `SsrParamsBlock` 整块写进 SSR 区、skyInvViewProj 写进 @64，再按 **shader 侧提取链**
 *     （scene.slang readSsrViewProj / clipToWorld 的「列 c 在 base+16c」）读回，断言
 *     读回的 ssrViewProj ≡ inverse(读回的 skyInvViewProj)。抓的是**偏移错 / 转置错 /
 *     步长错**——glm 列主序 vs shader 列组合是本项目反复出事的口径缝（5a-8c 教训：
 *     "内存第 c 个 vec4 = 第 c 列"，本测试用真 memcpy + load4 复刻，不经 glm 下标）。
 * (b) **抖动口径**（真护栏，(a) 在填充点几乎恒真）：SSR 读的是 scene_hdr（抖动网格），
 *     矩阵口径一旦漂成非抖动，采样位置整体偏移而画面「看着还行」= 静默漂移。
 *     开抖动 ⟹ 填值 **≠** 非抖动 proj·view（且差量有闭式解，见下）；关抖动 ⟹ **逐位 =**。
 *
 * ## 两源结构（迁移批 S-3 后 = 「区基址 + 手抄字段偏移」，先例 = test_wet_params 批 1）
 *
 * 写入侧 = C++ 常量 kSceneDataSsrOffset（镜像 vkrenderer 的 memcpy 基址），**读回侧 =
 * 基址 + 手抄 struct 字段偏移 0u**（镜像 scene.slang `sceneSsr.viewProj` 的字段位置）。
 * 迁移前这里是「常量写、字节字面量 723712u 读」；字节字面量随类型化载体删除后，两源
 * 落在**字段偏移**这一层：只测「常量写、常量读」则整体漂移测试永远绿（假绿），故字段
 * 偏移仍是本文件手抄的镜像，编译期由 `offsetof(SsrParamsBlock, viewProj)` 双钉。
 *
 * ⚠ 措辞纪律：手抄字段偏移钉的是「手抄 == 契约生成物」；scene.slang 那一侧由 reflect 期
 * validator 交叉核对钉（shader struct ↔ schema ↔ 生成物三方比 + 构建 SHA1 护栏逼重跑），
 * 不在本测试进程内。端到端护栏 = 红线 A 锚 + just ssr-check。
 *
 * ## 闭式差量（(b) 的独立期望值，不抄被测码）
 *
 * applyProjectionJitter 的口径（taa_jitter.cppm 头注释）= 只改 proj 第 3 列：
 * ΔP[2][0] = −2jx/W、ΔP[2][1] = −2jy/H，其余为 0。于是
 *   ssr − unjittered = (ΔP·V) ⟹ 差量[c][r] = ΔP[2][r] · V[c][2]
 * 即 行 0 差 = (−2jx/W)·view[c][2]、行 1 差 = (−2jy/H)·view[c][2]、行 2/3 **逐位等**。
 * 期望值出处 = 抖动注入的闭式解，不是「再算一遍 proj·view」。
 *
 * ## 变异红线声明（哪个变异让我红）
 *
 *   ① 本文件手抄字段偏移单侧漂（0u 改 +16）→ **编译期红**（offsetof static_assert 炸
 *      编译）；改 scene.slang SsrParamsBlock 字段序/类型 → reflect 期 validator 交叉核对
 *      红；schema+shader 同步换序 → 生成物字段偏移变 ⟹ 本条钉红。运行时等价形态「读回
 *      偏移与写入偏移不一致」由把读回字段偏移改 +64 复现 → (a) 全红（读回全 0）。
 *      ⚠ 改 kSceneDataSsrOffset（整区搬家）**不该红**——写侧 memcpy 与 binding 31 描述符
 *      基址同源同值，画面逐位不变（字节字面量已删，这正是迁移批 S-3 的目的）。
 *   ② fillSsrViewProj 填非抖动 proj（viewProj.projUnjittered * viewProj.view）
 *      → (b)「≠ 非抖动」红 + 闭式差量红（差量恒 0 ≠ 预测）+ (a) 互逆断言红
 *      （inverse(抖动后逆) ≠ 非抖动正）。
 *   ③ fillSsrViewProj 填 glm::transpose(...) → (a) 互逆断言红（锚点机位下 proj·view
 *      非对称，转置差 O(1) ≫ 容差）+ (b) 关抖动「逐位 =」红。
 *   （附）提取链步长 16 改 32 / 列序打乱 → (a)「读回逐位 = 写入」红。
 */
#include <doctest/doctest.h>
#include <cstddef>           // offsetof（契约生成物字段偏移双钉）
#include <initializer_list>  // 必须：modules 下 std::initializer_list 不从 import 带进来
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // glm::perspective（与 projectionSystem 同源）
#include <glm/gtc/matrix_inverse.hpp>    // glm::inverse（与 render_list_impl 的 @64 填充同源）

export module test_ssr_viewproj;

import rlstd;
import test_support;
import render_list;      // fillSsrViewProj（可测接缝，12.3d 同款）
import frame_draw_list;  // FrameDrawList / SceneFrameData.ssrViewProj
import scene_context;    // ViewProjection（proj = 抖动后 / projUnjittered 双份的真相源）
import scene_data;       // kSceneDataSsrOffset / kSceneDataSsrBytes / kSceneDataSkyOffset / kSceneDataBytes
import scene_forward_types_generated;  // SsrParamsBlock（契约生成物，字段偏移双钉的对象）
import camera;           // Camera / cameraView（rlbase，生产同源 view）
import taa_jitter;       // taaJitterPixels / applyProjectionJitter（生产同源抖动注入）

namespace RGL {
namespace {

/// scene.slang `sceneSsr.viewProj` 的 struct 字段偏移（两源结构的 shader 侧镜像：手抄
/// scene.slang SsrParamsBlock 声明顺序，勿改成常量引用——镜像失手抄价值即失效）。
constexpr uint32_t kSlangSsrViewProjFieldOffset = 0u;

// 编译期双保险（两源链）：手抄字段偏移 == 契约生成物字段偏移、区宽 == struct 宽
//（变异①在此炸；shader ↔ schema ↔ 生成物三方核对在 reflect 期，见文件头）。
static_assert(kSlangSsrViewProjFieldOffset == offsetof(SsrParamsBlock, viewProj),
	"scene.slang SsrParamsBlock.viewProj field offset drifted from hand-copied mirror");
static_assert(sizeof(SsrParamsBlock) == kSceneDataSsrBytes,
	"SsrParamsBlock width drifted from kSceneDataSsrBytes (sub-range descriptor range)");
// SSR 区紧贴湿面区之前（批 15.2 子步 A 在 SSR 之后追加湿面区 ⟹「最后尾区」的旧断言改钉
// 相邻关系，无重叠无空洞；湿面区自身的末尾断言 = tests/test_wet_params）；不与头 128B 重叠。
// 批 L（§9.7）后相邻不变式 = 「下一区基址 = alignUp(前区尾, 256)」。
static_assert(alignUp(kSceneDataSsrOffset + kSceneDataSsrBytes, kSceneRegionAlign) ==
		kSceneDataWetOffset,
	"SSR region must sit right before the wet tail region (batch 15.2A appended after it)");
static_assert(kSceneDataSsrOffset >= kSceneDataSkyOffset + sizeof(glm::mat4),
	"SSR region overlaps the @64 skyInvViewProj header slot");

/// staging 仿真缓冲：大小 = vkrenderer 写入面（头 + 全部尾区，SSR 区恰在末尾）。
using StageBuffer = std::vector<std::byte>;

/// shader `sceneData.Load4(byteOffset)` 等价形态（test_shadow_forward 同款：真 memcpy，
/// 不经 glm 下标，"内存第 c 个 vec4 = 第 c 列"由字节保证）。
glm::vec4 load4(const StageBuffer& buf, uint32_t byteOffset) {
	glm::vec4 v;
	std::memcpy(&v, buf.data() + byteOffset, sizeof(v));
	return v;
}

/// scene.slang readSsrViewProj / clipToWorld 的提取链复刻：列 c 在 base + 16c。
glm::mat4 loadMat4(const StageBuffer& buf, uint32_t base) {
	return glm::mat4(load4(buf, base), load4(buf, base + 16u),
		load4(buf, base + 32u), load4(buf, base + 48u));
}

/// 非退化机位（**刻意不用 identity view**：5a-8c 教训——view = 单位阵时 inverse 的
/// 两种乘法顺序恰好同值，转置/顺序错全被掩盖）。
ViewProjection makeViewProj() {
	Camera cam;
	cam.mode = CameraMode::Fps;
	cam.position = glm::vec3(3.0f, 4.0f, 5.0f);
	cam.yaw = 0.6f;
	cam.pitch = -0.25f;
	cam.fov = 45.0f * glm::pi<float>() / 180.0f;
	ViewProjection vp;
	vp.view = cameraView(cam, glm::dvec3{0.0});
	vp.proj = glm::perspective(cam.fov, 1.6f, 0.1f, 250.0f);
	vp.projUnjittered = vp.proj;  // projectionSystem 刚写的就是未抖那份（ecs_impl 同款）
	return vp;
}

/// 逐元素相对 + 绝对双阈比较（expectNear 的 mat4 展开；失败信息由 expectNear 自带）。
void expectNearMat4(const glm::mat4& actual, const glm::mat4& expected, double relTol,
	double absTol) {
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			testing::expectNear(actual[c][r], expected[c][r], relTol, absTol);
}

}  // namespace

TEST_CASE("SSR 尾区布局：读回 ssrViewProj ≡ inverse(读回 skyInvViewProj)（偏移/转置/步长）") {
	// 生产口径两份矩阵：SSR 区 viewProj = proj·view（接缝填）、@64 = inverse(proj·view)
	// （render_list_impl 同款表达式——本用例钉的是**字节布局**，@64 表达式本体归
	// test_shadow_forward 的恒等链）。
	const ViewProjection vp = makeViewProj();
	FrameDrawList list;
	fillSsrViewProj(list, vp);
	const glm::mat4 invVP = glm::inverse(vp.proj * vp.view);

	// staging 写入：vkrenderer_impl::writeSceneStaging 同款——SSR 区按生成 struct 整块写
	//（迁移批 S-3；mat4 → ObjectMatrix 列直拷与旧的 memcpy 64B 逐字节同）+ @64 一条 memcpy。
	StageBuffer stage(kSceneDataSsrOffset + kSceneDataSsrBytes, std::byte{0});
	std::memcpy(stage.data() + kSceneDataSkyOffset, &invVP, sizeof(glm::mat4));
	const SsrParamsBlock ssrBlock {
		.viewProj = ObjectMatrix {list.sceneData.ssrViewProj[0], list.sceneData.ssrViewProj[1],
		                          list.sceneData.ssrViewProj[2], list.sceneData.ssrViewProj[3]},
	};
	std::memcpy(stage.data() + kSceneDataSsrOffset, &ssrBlock, sizeof(ssrBlock));

	// shader 侧读回：binding 31 子范围描述符基址（= kSceneDataSsrOffset，C++ 写描述符同源）
	// + 手抄字段偏移（两源结构，见文件头）；@64 那份仍是字节基址（SceneHeader 未迁）。
	const glm::mat4 ssr = loadMat4(stage, kSceneDataSsrOffset + kSlangSsrViewProjFieldOffset);
	const glm::mat4 inv = loadMat4(stage, 64u);

	// 提取链恒等（memcpy + 列组合读 = 逐位还原写入值）：步长错 / 列序打乱在本条红。
	CHECK(ssr == list.sceneData.ssrViewProj);
	CHECK(inv == invVP);

	// 阳性锚点：矩阵**非对称**（变异③ transpose 的判据前提——对称矩阵下转置恒等，
	// 互逆断言会失去区分力）且非单位阵（缓冲没读成全 0 / 全默认）。
	bool asymmetric = false;
	for (int c = 0; c < 4 && !asymmetric; ++c)
		for (int r = 0; r < 4; ++r)
			if (ssr[c][r] != ssr[r][c]) asymmetric = true;
	CHECK(asymmetric);
	CHECK(ssr != glm::mat4(1.0f));

	// 主断言：两区严格互逆（float inverse 往返相对误差 ~1e-6，容差 1e-4 留两个数量级；
	// 转置/错偏移的偏差是 O(1) 或整块 0，≫ 容差）。
	expectNearMat4(ssr, glm::inverse(inv), 1e-4, 1e-5);
	expectNearMat4(glm::inverse(ssr), inv, 1e-4, 1e-5);
}

TEST_CASE("SSR 抖动口径：填值 = 抖动后 proj·view——开抖动 ≠ 非抖动（闭式差量），关抖动逐位 =") {
	const ViewProjection base = makeViewProj();
	const glm::mat4 unjittered = base.proj * base.view;  // 非抖动期望（口径漂的对照端）
	const glm::vec2 extent(1280.0f, 720.0f);

	// ① 开抖动：16 个 Halton 相位全打（taaJitterPixels 的 +1 跳全零点 ⟹ 每相位抖动非 0，
	//   下方 CHECK 钉住这个前提，前提坏 = 用例自己先红）。
	for (uint32_t frameIndex = 0; frameIndex < kTaaJitterPeriod; ++frameIndex) {
		const glm::vec2 jitter = taaJitterPixels(frameIndex, 1.0f);
		INFO("frameIndex=" << frameIndex << " jitter=(" << jitter.x << "," << jitter.y << ")");
		CHECK(jitter != glm::vec2(0.0f));

		ViewProjection vp = base;
		vp.proj = applyProjectionJitter(base.proj, jitter, extent);  // taaSystem 同款注入
		FrameDrawList list;
		fillSsrViewProj(list, vp);

		// 填值取的就是注入抖动那一份 proj（逐位；变异「填 projUnjittered」在此红）。
		CHECK(list.sceneData.ssrViewProj == vp.proj * vp.view);
		// 主护栏：≠ 非抖动 proj·view（口径漂成非抖动 = 本项目反复踩的静默漂移）。
		CHECK_FALSE(list.sceneData.ssrViewProj == unjittered);

		// 闭式差量（独立期望值，出处 = applyProjectionJitter 的注入式，见文件头推导）：
		// 行 0/1 差 = (−2j/extent)·view[c][2]；行 2/3 逐位等（ΔP 只动第 3 列的 0/1 行）。
		for (int c = 0; c < 4; ++c) {
			testing::expectNear(list.sceneData.ssrViewProj[c][0] - unjittered[c][0],
				(-2.0 * static_cast<double>(jitter.x) / extent.x) * base.view[c][2], 1e-3, 1e-4);
			testing::expectNear(list.sceneData.ssrViewProj[c][1] - unjittered[c][1],
				(-2.0 * static_cast<double>(jitter.y) / extent.y) * base.view[c][2], 1e-3, 1e-4);
			CHECK(list.sceneData.ssrViewProj[c][2] == unjittered[c][2]);
			CHECK(list.sceneData.ssrViewProj[c][3] == unjittered[c][3]);
		}
	}

	// ② 关抖动：taaSystem **不调** applyProjectionJitter（proj 与 projUnjittered 同值）
	//   ⟹ 填值逐位 = 非抖动 proj·view（变异③ transpose 在此红：锚点机位矩阵非对称）。
	FrameDrawList list;
	fillSsrViewProj(list, base);
	CHECK(list.sceneData.ssrViewProj == unjittered);
}

}  // namespace RGL
