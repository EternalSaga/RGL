module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // glm::perspective（与 projectionSystem 同源）
#include <glm/gtc/matrix_inverse.hpp>    // glm::inverse（与 render_list_impl L408 同源）
#include <initializer_list>              // 花括号列表（modules 下不从 import 带进来）
#include <cmath>                         // std::sqrt（自由函数不经 BMI 保证传递，GMF 自带）

// shadow.slang 级联选级 forward 提取式的数学回归（批 12.3d-C，5a-8c 回归）——
// 被测公式 = rgl/shaders/shadow.slang L65-67：
//   camRight = asfloat(sceneData.Load4(64)).xyz;   // invVP 第 0 列前三分量
//   camUp    = asfloat(sceneData.Load4(80)).xyz;   // invVP 第 1 列前三分量
//   forward  = normalize(cross(camUp, camRight));
//
// ## 诚实声明（本测试钉什么、不钉什么）
//
// 钉的是**数学同一性**：对 invVP = inverse(proj·view)（列主序 memcpy 进 scene data
// @kSceneDataSkyOffset），"字节偏移→列→叉积"这条提取链恒等于 CPU 侧 cameraForward。
// 它**不解析 shader 源**——shadow.slang 该式（字节偏移 64/80、叉积顺序、取反）**任何
// 改动必须同步本测试**（本文件是恒等式的 C++ 侧镜像）。生产侧注释同步建议已列入
// 12.3d-C 交付报告（建议 shader 注释反向指回本文件）。
//
// ## 钉住的同源链（断言按源码写，不照记忆写）
//
//   · CPU 写入侧：render_list_impl.cppm L408 `skyInvViewProj = glm::inverse(viewProj.proj
//     * viewProj.view)`；view = cameraView(camera)（camera.cppm，fps 分支 = lookAt(pos,
//     pos+cameraForward, cameraUp)）、proj = glm::perspective(fov, aspect, near, far)
//     （ecs_impl.cppm projectionSystem；rlbase PUBLIC 定义 GLM_FORCE_DEPTH_ZERO_TO_ONE，
//     测试链 rlbase 同宏生效）——本测试逐环节走同一构造函数，不另起炉灶。
//   · 字节布局：vkrenderer_impl.cppm L208 memcpy(&skyInvViewProj → kSceneDataSkyOffset)；
//     glm mat4 列主序 → Load4(64+16c) = 第 c 列（5a-8c 教训 #2："内存第 c 个 vec4 =
//     第 c 列"，本测试用真 memcpy + load4 复刻，不经 glm 下标，混不了）。
//
// ## 钉住的历史坑（docs/调试历史.md 5a-8c）
//
//   首版把 `-normalize(Load4(96).xyz)`（第 2 列）当 forward，注释称"w=0 无平移项混入"
//   ——实测 w≠0：invVP 第 2 列 = eyePos/B（透视逆的平移泄漏），方向实为归一化眼位，
//   真机点积 −0.43 → 级联选级全错（90% 画面从未采过阴影图）。锚点用例把"col2.w ≠ 0"
//   "col2.xyz ∝ eyePos"、"错误形态在锚点机位与 forward 点积 = −eye.z/|eye|"三条钉死；
//   随机用例给正确式 dot ≥ 1−1e-5。
//
// ## 变异红线声明（改复刻式哪一处 → 哪条红）
//
//   提取式换历史错误形态 `-normalize(load4(96).xyz)` → 随机用例红（各机位 dot 散布在
//   ±1 之间，无一过阈），锚点用例三条全红；
//   `cross(camUp, camRight)` 顺序颠倒 → dot ≈ −1，两用例全红；
//   字节偏移 64↔80 互换（= 叉积顺序颠倒的内存形态）→ col∝right/up 两条断言先红。

export module test_shadow_forward;

import rlstd;
import test_support;
import camera;      // Camera / cameraForward / cameraRight / cameraUp / cameraView（rlbase，生产同源）
import scene_data;  // kSceneDataSkyOffset / kSceneDataHeaderBytes（字节偏移真相源，rlscene）

namespace RGL {
namespace {

using testing::expectNear;
using testing::Lcg;

// shader 端 Load4(64) 字面量与 scene_data 常量的同步锚（漂移 = 编译期红）。
static_assert(kSceneDataSkyOffset == 64);

/// scene data 头（128B）仿真：vkrenderer_impl writeSceneStaging 同款 memcpy 写入 invVP。
using SceneHeader = std::array<std::byte, kSceneDataHeaderBytes>;

SceneHeader makeHeader(const glm::mat4& invVP) {
	SceneHeader header{};
	std::memcpy(header.data() + kSceneDataSkyOffset, &invVP, sizeof(glm::mat4));
	return header;
}

/// shader `sceneData.Load4(byteOffset)` 等价形态：取字节偏移起的 16B（列主序 mat4
/// 的第 c 列在 64 + 16c，5a-8c 教训 #2 的"列 ≠ 下标行"由真 memcpy 保证）。
glm::vec4 load4(const SceneHeader& h, uint32_t byteOffset) {
	glm::vec4 v;
	std::memcpy(&v, h.data() + byteOffset, sizeof(v));
	return v;
}

/// 机位 → invVP，生产同源三段：cameraView（fps 分支 lookAt）+ glm::perspective
/// （projectionSystem L270-273）+ glm::inverse(proj·view)（render_list_impl L408）。
/// fov/aspect/near/far 显式入参（projectionSystem 的自适应 near/far 与本恒等式无关，
/// 直接给最终生效值）。
struct Pose {
	Camera cam;      ///< fov 已写入（cameraForward 等读它无碍，方向只由 yaw/pitch 定）
	glm::mat4 invVP;
};

Pose makePose(Camera cam, float fov, float aspect, float nearZ, float farZ) {
	cam.fov = fov;
	const glm::mat4 view = cameraView(cam);
	const glm::mat4 proj = glm::perspective(fov, aspect, nearZ, farZ);
	return Pose{cam, glm::inverse(proj * view)};
}

}  // namespace

TEST_SUITE("shadow_forward") {

TEST_CASE("锚点机位：提取式 = (0,0,−1) 约定值 + col2 是平移泄漏（5a-8 假言反例锚点）") {
	// yaw = pitch = 0 的解析可写形态：invVP col0 ∝ +x、col1 ∝ +y、col2 = eye/B（B<0）
	// → 正确式精确 (0,0,−1)；错误式 −normalize(col2.xyz) = normalize(eye)。
	Camera cam;
	cam.mode = CameraMode::Fps;
	cam.position = glm::vec3(3.0f, 4.0f, 5.0f);
	cam.yaw = 0.0f;
	cam.pitch = 0.0f;
	const Pose p = makePose(cam, 45.0f * glm::pi<float>() / 180.0f, 1.5f, 0.1f, 100.0f);
	const SceneHeader h = makeHeader(p.invVP);

	const glm::vec3 camRight(load4(h, kSceneDataSkyOffset));      // shadow.slang L65（.xyz = 显式截断，glm 默认无 swizzle）
	const glm::vec3 camUp(load4(h, kSceneDataSkyOffset + 16));    // shadow.slang L66
	const glm::vec4 col2 = load4(h, kSceneDataSkyOffset + 32);    // 历史错误取材点（L96）
	const glm::vec3 forward = glm::normalize(glm::cross(camUp, camRight));  // shadow.slang L67

	// 约定锚点：forward = (0,0,−1)（camera.cppm L108 注释的同款约定）。
	expectNear(forward.x, 0.0, 0.0, 1e-6);
	expectNear(forward.y, 0.0, 0.0, 1e-6);
	expectNear(forward.z, -1.0, 1e-6);

	// 5a-8 假言反例："w=0 无平移项混入"——col2.w 恒 ≠ 0（= 1/B，B = proj[3][2] < 0）。
	CHECK(col2.w != 0.0f);
	// col2.xyz ∝ eyePos：−normalize(col2.xyz) 恰为归一化眼位（"它是平移，不是方向"）。
	// shader 形态 = 先 `.xyz` 截断再 normalize（顺序反过来会把 w 除进 xyz，实测偏 ~1%）。
	const glm::vec3 wrongForm = -glm::normalize(glm::vec3(col2));
	const glm::vec3 eyeDir = glm::normalize(p.cam.position);
	expectNear(glm::dot(wrongForm, eyeDir), 1.0, 1e-5);
	// 错误形态与真 forward 的点积解析值 = −eye.z/|eye| = −5/√50 ≈ −0.707
	//（历史实测 −0.43 是另一机位；同一失效形态、不同数值）。
	expectNear(glm::dot(wrongForm, cameraForward(p.cam)), -5.0 / std::sqrt(50.0), 1e-5);
}

TEST_CASE("随机 500 机位（Lcg 写死种子）：normalize(cross(col@80, col@64)) ≡ cameraForward") {
	// 采样域：yaw 全域、pitch ±1.4 rad（≈±80°，生产 kPitchLimit=±89° 之内的稠密覆盖）、
	// fov ∈ (0.2, 2.2) rad、aspect ∈ [0.4, 2.5]、near/far 比 10²~10⁴。
	// 恒等式与 fov/aspect/near/far **无关**（invP 第 0/1 列是正标量 × 基向量），随机域
	// 扫的就是这条无关性。
	Lcg rng(0x5A08Cull);
	for (int i = 0; i < 500; ++i) {
		Camera cam;
		cam.mode = CameraMode::Fps;
		cam.position = glm::vec3(static_cast<float>(rng.uniform(-64.0, 64.0)),
			static_cast<float>(rng.uniform(-64.0, 64.0)),
			static_cast<float>(rng.uniform(-64.0, 64.0)));
		cam.yaw = static_cast<float>(rng.uniform(-glm::pi<double>(), glm::pi<double>()));
		cam.pitch = static_cast<float>(rng.uniform(-1.4, 1.4));
		const float fov = static_cast<float>(rng.uniform(0.2, 2.2));
		const float aspect = static_cast<float>(rng.uniform(0.4, 2.5));
		const float nearZ = static_cast<float>(rng.uniform(0.02, 0.5));
		const float farZ = nearZ * static_cast<float>(std::pow(10.0, rng.uniform(2.0, 4.0)));

		const Pose p = makePose(cam, fov, aspect, nearZ, farZ);
		const SceneHeader h = makeHeader(p.invVP);

		const glm::vec3 camRight(load4(h, kSceneDataSkyOffset));
		const glm::vec3 camUp(load4(h, kSceneDataSkyOffset + 16));
		const glm::vec3 forward = glm::normalize(glm::cross(camUp, camRight));

		INFO("i=" << i << " yaw=" << cam.yaw << " pitch=" << cam.pitch << " fov=" << fov
			<< " aspect=" << aspect << " near=" << nearZ << " far=" << farZ
			<< " dotFwd=" << glm::dot(forward, cameraForward(p.cam)));

		// 主断言：shader 提取式 ≡ 生产口径 forward（view 就是拿它做的 lookAt 视线轴）。
		CHECK(glm::dot(forward, cameraForward(p.cam)) >= 1.0 - 1e-5);
		// 列→向量映射两半（64=右、80=上；互换 64/80 或改取材点在本条先红）。
		CHECK(glm::dot(glm::normalize(camRight), cameraRight(p.cam)) >= 1.0 - 1e-5);
		CHECK(glm::dot(glm::normalize(camUp), cameraUp(p.cam)) >= 1.0 - 1e-5);
	}
}

}  // TEST_SUITE("shadow_forward")

}  // namespace RGL
