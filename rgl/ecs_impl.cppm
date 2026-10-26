module;
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>  // taaSystem 的 spin inverse（GMF include 不随 import 可见）
#include <ranges>
module ecs:impl;

import ecs;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import :frame_input;  // 帧首外部状态入册分区（事件翻译 / resize / 配置热重载 / picking）
import :frame_diag;  // 批 14.3：LlmCaptureFlow / FrameStatsTracker（帧循环诊断旁路，兄弟分区）
import rlstd;
import vkerror;
import vk_swapchain;
import vk_resources;
import asset_table;  // AssetTable（renderSystem 参数透传；F 拆出）
import vk_renderer;
import frame_draw_list;
import render_list;
import runtime_config;  // LlmDebugConfig（renderSystem 参数类型）
import diagnostics;     // texel_canary 诊断单例（configs/diagnostics.lua）
import input_probe;     // 脚本化输入探针（2026-09-28 输入链回归护栏，帧循环 tick）
import profiler;        // Phase 9 帧相 zone（frameBegin + 帧循环拦截点，docs/archive/phase9_plan.md）
import gpu_canary;
import device_fault;  // GPU fault masked 轮询（diagnostics.fault_poll 门，默认关 = 零调用）
import shutdown_ledger;  // 诊断账注册表（device-lost 之路上也要结算，见 faultCaptureOnLost）
import scene_context;
import taa_jitter;  // 批 12.6 TAA：halton 抖动 / jitter→投影 / 跳切判据（纯函数层）
import coordinate_rebase;  // 批 4 第一刀：motionWithOriginShift（跨帧原点差复合）
import camera;
import rllogger;

namespace RGL {

// 帧循环各 system + renderSystem 定义（2026-09-02 编译提速 A 方案）：system 体移出
// 接口 TU——BMI 只含 renderSystem 声明与 context struct；本单元是叶子边。

/**
 * 场景整体时间旋转（SceneSpin）：angle += dt * speed；matrix = rotate(axis, angle)。
 * speed = 0 → 直接返回，matrix 不更新（configs/scene.lua 的 spin 开关，main
 * 启动期转成 speed）。现状承载"全场景同一旋转"（renderListSystem 乘进各
 * MVP）；per-entity 旋转需求出现时改写 Transform 组件，结构同构，无需二次重构
 *（ecs_compliance_review E4.1）。
 *
 * 帧驱动门（`spin_frame_angle` ≠ 0，默认关）：门开走 angle = 帧号纯函数
 * （frameDrivenSpinAngle，单测钉），dt 累积停用——时域判据要跨进程渲染**同一帧序列**，
 * dt 累积随帧时长漂、测不出可复现的数（tools/imgqa/temporal.cpp 头注释）。
 * 门红线：frameAngle = 0（默认）时本分支不进，门关路径与今天逐位相同。
 * @param rg registry（读写 SceneSpin）
 * @param dt 帧间隔（秒，调用方已限幅到 0.1s；门开时不消费）
 */
void animationSystem(entt::registry& rg, float dt) {
	auto& spin = rg.ctx().get<SceneSpin>();
	if (spin.frameAngle != 0.0f) {  // 帧驱动门（默认 0 = 关）
		spin.frameIndex += 1u;
		spin.angle = frameDrivenSpinAngle(spin.frameAngle, spin.frameIndex);
		spin.matrix = glm::rotate(glm::mat4(1.0f), spin.angle, spin.axis);
		return;
	}
	if (spin.speed == 0.0f) {
		return;
	}
	spin.angle += spin.speed * dt;
	spin.matrix = glm::rotate(glm::mat4(1.0f), spin.angle, spin.axis);
}

/**
 * union 包围球（标准公式；radius = 0 视为空球）：a 包含 b → a；b 包含 a → b；
 * 否则 r = (ra+rb+d)/2，中心从 a 向 b 滑动 t = ra/(d+r)。
 * @param a 包围球 A（空球 = radius 0，直接返回 b）
 * @param b 包围球 B（空球 = radius 0，直接返回 a）
 * @return 同时包含两者的包围球
 */
SceneBounds unionSphere(const SceneBounds& a, const SceneBounds& b) {
	if (a.radius == 0.0f) return b;
	if (b.radius == 0.0f) return a;
	const float d = glm::length(b.center - a.center);
	if (a.radius >= d + b.radius) return a;
	if (b.radius >= d + a.radius) return b;
	const float r = 0.5f * (a.radius + b.radius + d);
	const float t = (d + r) > 0.0f ? a.radius / (d + r) : 0.0f;
	return SceneBounds{a.center + (b.center - a.center) * t, r};
}

/**
 * 聚合全部 Bounds 组件 → SceneBounds context（unionSphere 逐个合并，空场景 =
 * radius 0）。现状场景静态（Bounds 仅导入期写入），每帧重聚合为未来动态场景
 * 变更留接缝。
 * @param rg registry（遍历 Bounds view，写 SceneBounds）
 */
void boundsSystem(entt::registry& rg) {
	auto& sceneBounds = rg.ctx().get<SceneBounds>();
	SceneBounds acc{};
	rg.view<Bounds>().each([&](const Bounds& b) {
		acc = unionSphere(acc, SceneBounds{b.center, b.radius});
	});
	sceneBounds = acc;
}

/**
 * 投影 system 定义（接口声明、搬家理由与门红线 → ecs.cppm）。
 *
 * 🔴 函数体内的**语句顺序就是本批的裁决**（near/far → proj → 粒度 → 原点 → view），
 * 它把「先有粒度、再有原点、再有 view」从注释契约变成词法顺序。调换任意两段的后果
 * 不是「图变」而是「门关照绿、门开静默错位」，所以判据住在 tests/test_render_origin.cppm
 * 的闭式用例（不是截图锚）。
 * @param rg registry（读 Camera/SceneBounds/RenderExtent，写 RenderOrigin / ViewProjection）
 */
void projectionSystem(entt::registry& rg) {
	// 批 14.3：宽高比取**渲染**网格。按窗口比例算投影、按渲染网格铺像素 = 各向异性拉伸。
	auto& extent = rg.ctx().get<RenderExtent>();
	const auto& camera = rg.ctx().get<Camera>();
	const auto& sceneBounds = rg.ctx().get<SceneBounds>();
	// 渲染原点：本 system 是它的**生产者**（批 5 第 1 刀，原 originSystem 搬进来），
	// 消费者（本 system 自己的 view、后面的 taaSystem / renderListSystem）读的都是本帧这份。
	auto& origin = rg.ctx().get<RenderOrigin>();
	auto& viewProj = rg.ctx().get<ViewProjection>();

	// ── ① 自适应 near/far ──
	// 距离判据的**输出**是 float 深度界（near/far 住 float），但**减法必须在 double 域**
	//（债-151 同族，2026-10-26）：`camera.position` 是绝对层 double，旧写法先把它收窄成
	// float32 再减 `sceneBounds.center` ⟹ 一个几米长的距离被掺进 0.0625 m 的量化误差
	//（|p| = 1e6 处 float32 的 ULP），而 near/far 正是要**贴着**场景包围球收的。
	// ⚠ 本条只能修到「不再额外收窄」，修不到「绝对层真 double」：`Bounds::center` 本身就是
	//   float 存的绝对层量（1e6 处同样 ULP 0.0625 m），把它的生产链 double 化 = 相对坐标计划
	//   批 3 第二刀那笔没做的账，本批不做。
	// 🔴 两项都是**绝对量**、rebase 根本不碰它们 ⟹ 开/关两态 near/far 逐位相同
	// （裁决与凭据 → docs/相对坐标地基_plan.md §3 批 4「裁决一」；不许顺手减原点，
	//  反向半边由 tests/test_view_light_rebase.cppm 的 near/far 不变性断言守着）。
	float nearPlane = camera.nearPlane;
	float farPlane = camera.farPlane;
	if (sceneBounds.radius > 0.0f) {
		const double dist = glm::length(
			camera.position - glm::dvec3(sceneBounds.center));
		nearPlane = std::max(0.05f, static_cast<float>(dist - sceneBounds.radius));
		farPlane = static_cast<float>(dist + sceneBounds.radius);
	}
	// ── ② 投影矩阵 + 实际生效深度界写回（级联切分用；自适应值不等于 camera.nearPlane/farPlane，
	// 外部若直接拿 camera 的会切错分界）──
	viewProj.proj =
		glm::perspective(camera.fov,
			static_cast<float>(extent.width) / static_cast<float>(extent.height),
			nearPlane, farPlane);
	viewProj.nearPlane = nearPlane;
	viewProj.farPlane = farPlane;

	// ── ③ 渲染原点（门关早退；门开 = 期望原点 snap 到粒度）──
	if (origin.enabled) {
		// 粒度 = rebaseSnapGranularity(本帧四级纹素)，三个环节全吃现成函数（接缝注释里写了
		// 为什么不许另推闭式、为什么喂单位 view）。期望原点 = 相机位置本身（债-85 ①：决定
		// float32 抖动量级的是相机离原点多远 ⟹ 把原点搬到相机就是这笔工作的全部收益）。
		const double granularity = rebaseOriginGranularity(viewProj.proj, nearPlane, farPlane);
		// 🔴 NaN 必须响亮 throw：NaN 原点会一路灌进 cameraView 的平移列 ⟹ 整个场景被裁掉、
		// 不报任何错（相机凭空消失），那是本项目最难查的形态。判据取「非正 / 非有限」而不是
		// `== NaN`：rebaseSnapGranularity 的非法输入语义就是 NaN，而 0 粒度会让 snap 退化成
		// 不 snap（正是那条护栏点名要拦的静默退化），两者都不许往下走。
		if (!(granularity > 0.0) || !std::isfinite(granularity))
			throw std::runtime_error(
				"coordinate.rebase = true 但本帧算不出原点吸附粒度（rebaseSnapGranularity 给出 "
				"NaN / 非正值 ⟹ 级联纹素坏到选不出格距，通常是 near/far 退化）——宁可进程退出，"
				"也不拿 NaN 原点渲染（相机凭空消失、无报错）；关掉 configs/runtime.lua 的 "
				"coordinate.rebase 即回到门关态（出厂默认已是门开，门关是归因对照态）");
		origin.position = snapOriginToGranularity(camera.position, granularity);
	} else {
		origin.position = glm::dvec3{0.0};  // 门关：减法恒等，交付图逐位不变
	}

	// ── ④ view（🔴 必须在 ③ 之后：它吃的就是刚写好的那份原点）──
	viewProj.view = cameraView(camera, origin.position);
}

/**
 * TAA 帧间 system（批 12.6；顺序 = projection 之后、renderlist 之前）。
 *
 * 干三件事：① 把亚像素抖动注进 `ViewProjection::proj`（未抖那份存 projUnjittered）；
 * ② 判本帧历史可用性（首帧 / resize / 相机跳切 ⇒ false ⇒ shader 逐位旁路）；
 * ③ 算本帧 scene data 载荷（上一帧**抖动后** viewProj + 全场景刚体运动补偿矩阵）并推进簿记。
 *
 * 批 4 第一刀改的是 ③ 里那条运动补偿：它现在除了自旋比，还**精确复合两帧渲染原点之差**
 * （`RenderOrigin::position` 与本 system 上帧尾记的 `TaaState::prevOrigin`）。原因与
 * 「为什么修 motion 而不是修 prevViewProj」的裁决写在下面 ③ 的注释里，闭式推导在
 * coordinate_rebase.cppm::motionWithOriginShift。
 *
 * **门红线**：`env.taaEnabled == false` 时完全不碰 `proj`（不是「加 0」而是不调用）
 * ⇒ 下游 objectMvps / @64 / 剔除 逐位不变。
 * **门关（`coordinate.rebase = false`，需显式 `--set` 才到达）红线**：原点恒 (0,0,0) ⇒ 原点差恰为零 ⇒
 * motionWithOriginShift 逐位返回自旋比 ⇒ 载荷与改前逐位相同。出厂默认已是门开（2026-10-26 翻），
 * 这条早退分支因此**不是出厂路径**——它是阴影/时域归因的对照态，别当死码删。
 *
 * 抖动为什么加在 proj 而不是各消费方：见 ViewProjection::jitterPx 注释（一处抖，
 * 深度缓冲与所有重建该深度的消费者自动同口径）。
 * @param rg registry（读 SceneEnv/SceneSpin/Camera/WindowExtent/SceneBounds/ViewProjection/
 *        RenderOrigin，读写 TaaState）
 */
void taaSystem(entt::registry& rg) {
	auto& viewProj = rg.ctx().get<ViewProjection>();
	auto& taa = rg.ctx().get<TaaState>();
	const auto& env = rg.ctx().get<SceneEnv>();
	const auto& spin = rg.ctx().get<SceneSpin>();
	const auto& camera = rg.ctx().get<Camera>();
	// 批 14.3：jitter 幅度按像素计、历史可用性的"尺寸变了没"，两者都必须落在**渲染**网格上。
	const auto& extent = rg.ctx().get<RenderExtent>();
	const auto& bounds = rg.ctx().get<SceneBounds>();
	// 渲染原点（相对坐标批 4 第一刀）：**本帧**那份。批 5 第 1 刀起生产者是 projectionSystem
	// 自己（帧序 camera→…→projection→taa，本 system 排在它之后 ⟹ 读到的必是本帧终值；
	// 原点在 projectionSystem 内部还要晚于 view 吗？不 —— view 也在那函数里、写完原点才算，
	// 所以本 system 看到的 view 与本帧原点是同一份）。
	// 门关恒 (0,0,0) ⟹ 下面 motion 的原点差项恰为零 ⟹ 逐位退化回纯旋转式子。
	const auto& origin = rg.ctx().get<RenderOrigin>();

	viewProj.projUnjittered = viewProj.proj;  // projectionSystem 刚写的就是未抖那份
	const glm::ivec2 now{static_cast<int>(extent.width), static_cast<int>(extent.height)};
	// TAA 的簿记与跳切判据住在 float 域（TaaState::prevCamPos 是 vec3、cameraJumps 的
	// 位移阈值是 float 容差）：绝对层相机位在这里**显式收窄**后交给它，算式与收窄前逐位
	// 相同。⚠ 这里收窄的是**相机位姿**（绝对层数值，与原点无关），不是世界坐标 ——
	// 「上一帧的世界与本帧不是同一个空间」那件事由下面的 payloadMotion 承担
	// （docs/相对坐标地基_plan.md §4 表首行）。
	const glm::vec3 camPos{camera.position};
	const auto forward = cameraForward(camera);

	// ① 抖动注入（关 = 不调用 applyProjectionJitter ⇒ proj 逐位不变）
	//
	// 批 14.5 第四批：抖动与「谁做时域滤波」**解耦**——此前它和 AO 施加点一样被硬编码成
	// TAA 专属（`env.taaEnabled` 一关投影就不抖），而 FSR3-upscaler 是**抖动相机**升频器，
	// 无抖动的输入不在它的设计点上。
	//
	// **TAA 在 = 一切走原路**（周期 kTaaJitterPeriod=16、幅度 env.taaJitterScale）
	// ⟹ TAA 档逐位不变（本文件头那条门红线的加强版：不只是关的时候不变，开着也不能被我改坏）。
	// 只有「TAA 不在而 FSR 要抖动」才用 vendor 的相位数与**满幅**（±0.5px = 设计点，
	// 不是 taa_jitter_scale——那个是 TAA 的调参旋钮，拿它当 FSR 的实验变量等于两个旋钮互相决定）。
	const bool fsrJitterOnly = !env.taaEnabled && env.fsrJitter;
	viewProj.jitterPx = env.taaEnabled
		? taaJitterPixels(taa.frameIndex, env.taaJitterScale)
		: (fsrJitterOnly
			? taaJitterPixels(taa.frameIndex, 1.0f,
				env.fsrJitterPeriod ? env.fsrJitterPeriod : kTaaJitterPeriod)
			: glm::vec2{0.0f});
	if (env.taaEnabled || fsrJitterOnly)
		viewProj.proj = applyProjectionJitter(viewProj.proj, viewProj.jitterPx,
			{static_cast<float>(extent.width), static_cast<float>(extent.height)});

	// ② 历史可用性（判据本体 = taaHistoryUsable 纯函数，单测覆盖）。
	// **帧序契约**：本段必须在推进簿记之前算（用的还是上一帧的值）。
	taa.historyValid = taaHistoryUsable(env.taaEnabled, taa.frameIndex, taa.prevExtent, now,
		taa.prevCamPos, camPos, taa.prevForward, forward, bounds.radius);

	// ②b 本帧权重（scene data @723584.x = taaParams.x）**不经 TaaState**：批 13.14b 删掉
	// 静态机位权重提升（static boost）后 taaSystem 不再产出 feedback，载荷直接取
	// env.taaFeedback（写载荷处 = `render_list_impl.cppm::fillTaaData`，它拿得到 env）。
	// ⚠ 方向红线：它是**本帧权重**（taa.frag: lerp(clipped, cur, x)）⟹ **调小 = 更信历史**。

	// ③ 本帧载荷。prevViewProj = 上一帧**抖动后** proj·view —— 口径必须跟历史图的
	// **内容**一致：taa_hist 由 taa_out 逐像素 1:1 拷来，而 taa_out 的颜色输入是抖动投影
	// 渲的 ⇒ 历史图按「第 t 帧抖动后」的屏幕系索引。存未抖那份会让采样点系统性偏掉
	// -jitter_t（±0.5px 且每帧随机变），静止机位下也照样搅动历史。批 12.6 T4 实测抓到：
	// S1/S2 = 0.48×（开滤波逐帧差反而**翻倍**，闭式该是 3.95×）。
	// ⚠ 旧注释写的「两侧口径统一 ⇒ 历史查找位不漂 jitter」是把「历史图是像素中心对齐的」
	// 误读成「历史图在未抖网格里」——像素中心对齐 ≠ 未抖网格对齐。
	// 🔵 **批 13.5 补充（读改动前必读）**：本裁决**仍然成立、未被反转**——本字段至今产出
	//   **抖动网格**上的坐标。变的是**下游**：历史图自 13.5 起住在**基空间**（输入侧已
	//   un-jitter），故 shader 拿到抖动网格坐标后要**减一帧内容位移 c_{t−1}** 才落到基网格
	//   （`taa.frag.slang` 的 prevUv 修正）。⟹ **两处必须成对**：把本字段改成未抖 = 采样点
	//   少减一次 c_{t−1} = 系统性偏 c_{t−1}（静止机位 0.667px 游动原样回来）。
	//   若将来改存未抖那份，**必须同时删掉 shader 侧的减法**，二选一，不可都做也不可都不做。
	// motion = prevSpin·inverse(curSpin)：把「本帧世界坐标」刚体搬回「上一帧世界坐标」
	// ——本项目全场景共用一个旋转矩阵，所以一条 mat4 就是**精确**补偿（不是近似）。
	// 🔴 批 4 第一刀：那条纯旋转式子只在**渲染原点不动**时才够。原点每移动一次，上一帧的
	// 「世界」与这一帧的「世界」就不是同一个空间（差 O_t − O_{t−1}），静止场景会被判成运动
	// ⟹ 历史被丢、时域噪声回来。正解 = 把两帧原点差精确复合进 motion（闭式推导
	// → coordinate_rebase.cppm::motionWithOriginShift），门关 ΔO = 0 ⟹ 逐位退化回上面那条。
	// ⚠ **原点那一项不受 `taaMotionCompensation` / `historyValid` 门控**，这是刻意的：
	// velocity.frag → FSR 那条链**不吃 historyValid**（每帧照算照输出），重定位当帧不能靠
	// 「丢弃历史」蒙过去。门关两者重合（ΔO = 0）⟹ 门红线（关 = 逐位不变）自动成立。
	// 🔴 **prevViewProj 这一侧刻意不修**（本批的口径裁决，二选一，两边都做 = 位移算两遍）：
	//   选 motion 承担的理由 = ① motion 的语义本来就是「本帧世界 → 上一帧世界」，原点差正是
	//   这个映射的一部分，加在这里名实相符；② prevViewProj 的语义是「渲染出历史图的那个矩阵」，
	//   与历史图内容**同口径**（上面批 12.6 T4 实测钉死的那条），改它 = 让那个字段说谎，而
	//   它同时被 taa.frag / velocity.frag / ssr 三条消费者读，一处改三处漂；③ shader 侧
	//   「prevViewProj 抖动后 ⟺ 必须减 c_{t−1}」是成对红线，动 prevViewProj 就要重推那三条。
	//   另一条（CPU 侧把 prevViewProj 修正成相对本帧原点）为什么没选：它把「坐标系」这件事
	//   塞进「内容指纹」字段，且要求所有 prevViewProj 消费者都理解这层修正 —— 射程比 motion
	//   大得多，而本批只被授权动 motion。
	taa.payloadPrevViewProj = taa.prevViewProj;
	taa.payloadPrevContentShift = taa.prevContentShift;
	const glm::mat4 spinRatio =
		(env.taaMotionCompensation && taa.historyValid)
			? taa.prevSpin * glm::inverse(spin.matrix)
			: glm::mat4{1.0f};
	taa.payloadMotion = motionWithOriginShift(spinRatio, taa.prevOrigin, origin.position);
	// 重投影消融门（`taa.reprojection`，默认 true = 本分支不进 = 逐位今天）。
	// 关 = 三份载荷联合改成本帧恒等形态：prevViewProj := **本帧抖动后** viewProj（此刻
	// viewProj.proj 已含本帧 jitter，与 @64 的 invViewProj 配套）+ 位移清零 + motion 单位阵
	// ⟹ shader 端 prevUvBase ≡ baseUv（world 由 (baseUv, depth) 经 invViewProj 重建、再被
	// 同一矩阵投影回去，浮点上也是同一对矩阵，采样点落回**同基位置**）。
	// 相机运动激励下历史错位 = 每帧像素位移 ⟹ 读数必须显著变差（temporal-motion 的 M3
	// 阳性锚点）；静止机位下 prevViewProj ≡ 本帧矩阵 ⟹ 关与不关逐位相同。
	// ⚠ 勿与 `motion_compensation` 混淆：那条只关 prevSpin 场景旋转补偿，相机动、spin 关
	//   时它是恒等阵 = 消融无效（这正是 temporal-motion 从 spin 换相机激励后必须新增本门
	//   的原因，见 tools/imgqa/temporal_motion.cpp 头注释）。
	// ⚠ 本门**连原点差一起抹掉**（motion := 单位阵，不是 motionWithOriginShift(I, …)）：
	//   本门的语义是「完全不重投影」= prevUv ≡ baseUv，留着 ΔO 就不是消融而是换算法。
	//   门开态下这条会让 velocity 在重定位当帧给出假静止 —— 那是本门应有的行为（它与
	//   `coordinate.rebase` 不同时开；批 5 立图像级判据时若要同开需重审这一行）。
	if (!env.taaReprojection) {
		taa.payloadPrevViewProj = viewProj.proj * viewProj.view;
		taa.payloadPrevContentShift = glm::vec2{0.0f};
		taa.payloadMotion = glm::mat4{1.0f};
	}

	// ④ 簿记推进（必须在 ②③ 之后；下游 renderListSystem 读的是 payload*）
	// 存**抖动后**那份（此刻 viewProj.proj 已含本帧 jitter）⇒ 下一帧拿到的正是
	// 「渲染出这张历史图的那个矩阵」。门关时 proj 未被碰过 ⇒ 与未抖那份逐位相同 ⇒
	// 门红线（关 = 基线逐位不变）自动成立，jitter_scale=0 时同理。
	taa.prevViewProj = viewProj.proj * viewProj.view;
	// 内容位移与 prevViewProj **同生同灭**：它俩是同一件事（「渲染出上一帧那张图的变换」）
	// 的矩阵表达与平移表达。TAA 关 / jitter_scale=0 ⇒ jitterPx = 0 ⇒ c = 0 ⇒ 减法逐位恒等。
	taa.prevContentShift = contentShiftPx(viewProj.jitterPx);
	taa.prevSpin = spin.matrix;
	// 渲染原点（批 4 第一刀）：与 prevSpin 同位推进 —— 下一帧的 motion 要用「上一帧在哪个
	// 空间里渲染」这一件事。门关 origin.position 恒 (0,0,0) ⇒ 本行写的是逐位零 ⇒ ΔO 恒 0。
	taa.prevOrigin = origin.position;
	taa.prevCamPos = camPos;
	taa.prevForward = forward;
	taa.prevExtent = now;
	++taa.frameIndex;
}


/**
 * 每帧 system（执行顺序 = 注册顺序，即数据流契约；新增 system = 此处 push_back）。
 */
struct FrameSystem {
	const char* name;  // system 名（文档/日志标识，执行路径不读取）
	std::function<void(entt::registry&, float dt)> fn;
};

namespace {

/** texel 回读金丝雀单次触发（docs/archive/heap_texture_research.md §12.7 ②）：
 * texel_canary=N（帧号，默认关）——帧 N 的 drawFrame 前重读白纹理已知 texel 比对，
 * 纹理 BO 死 = 立即报错（GPU 侧 UAF 签名，ASan 全盲；诊断路径单次 GPU 停顿，
 * 正常路径零影响）。texel_canary = configs/diagnostics.lua（原 RGL_TEXEL_CANARY
 * env 2026-09-08 转配置）。触发时机 = 本帧 drawFrame 前（waitForIdle 已在函数内
 * 排空 in-flight）；fired 由调用方持有，保证至多触发一次。 */
void texelCanaryOnce(bool& fired, uint64_t canaryFrame, uint32_t frame,
	RGL::Renderer& renderer, const RGL::AssetTable& assets) {
	if (fired || frame < canaryFrame) {
		return;
	}
	fired = true;
	texelCanaryCheck(renderer.getResourcesRef(), assets, renderer.getDeviceContextRef(),
		renderer.getGraphicsCommandPoolRef(), frame);
}

/** GPU fault masked 轮询一拍（device_fault；`diagnostics.fault_poll` 门在调用方读，
 * 关时连 device 都不取 = 零 Vulkan 调用）。挂在 drawFrame 前 = 本帧之前所有已提交工作的
 * fault 在这一拍收；timeout=0 不阻塞。抽成函数而非内联 if：renderSystem 的认知复杂度
 * 已贴上限（style-check 点名），且"门 + 动作"这对语义本来就该有个名字。 */
void faultPollTick(RGL::Renderer& renderer, bool faultPoll) {
	if (!faultPoll)
		return;
	// zone = 本通路自身成本的量具：帧墙钟的 run-to-run 带 ~1ms（VVL 下 CPU-bound），
	// 而一次 timeout=0 的查询是 µs 级，拿 frame_p50 的 A/B 永远"分辨不出"。profiler 的
	// 逐 zone cpu= 列才量得动（`just cpu --arms fault-poll` + profiler 窗口）。
	auto z = zone("faultPoll");
	pollDeviceFaults(renderer.getDeviceContextRef().getDevice(),
	    renderer.getDeviceContextRef().getDeviceFaultSupport());
}

/** device lost 时抓一次 GPU fault 报告（device_fault；调用点判据见 drawFrameChecked 内注释）。
 * 与 faultPollTick 同属"诊断旁路"，区别只在门：轮询要 `diagnostics.fault_poll`，抓取**恒尝试**
 * ——设备都丢了，这是唯一一次问到"当时在访问哪个地址"的机会，没有理由再设门。
 * 通路未启用时它自己打一行 INFO 点名"没这能力"，不会静默。
 * 🔴 这里**不许**先调一次 pollDeviceFaults：KHR 规定每条报告只返回一次（"Each individual
 * fault report is returned exactly once"），先轮后抓 = 把报告吃掉，接着出口 ③ 那条"驱动
 * 可见面无 fault"就会打成**假负读数**（明明有报告，被上一次轮询消费掉了）。两条只能取其一。 */
void faultCaptureOnLost(RGL::Renderer& renderer, std::string_view context, uint32_t frame) {
	const auto& device = renderer.getDeviceContextRef();
	// 帧号进 vendor dump 的文件名（`faultdumps/rgl-fault-frame185.bin`）——同一进程内如果
	// 驱动给了两次 dump，不带帧号就会互相覆盖，而"哪一帧留过现场"正是这条数据要回答的。
	reportDeviceFaultOnLost(device.getDevice(), device.getDeviceFaultSupport(), context,
	    "frame" + std::to_string(frame));  // 本文件 GMF 没有 fmt（modules 坑），用 std::to_string
	// 🔴 崩溃现场的 heap 槽 dump（债-140 夜间批）：devcoredump 只说 fault@0、不说哪条 binding，
	// 而描述符堆在 host 可见映射上、device lost 后仍读得到 —— 必须在**这里**读，晚一步
	// （异常展开过 DeviceContext）就连 device 都没了。
	renderer.logMdiEntrySlotsOnLost();  // （device 是 DeviceContext，槽在 Renderer 手里）
	// 🔴 诊断账必须在这里也结算一遍：main 的退出收口在 device lost 时**执行不到**（收尾
	// waitIdle 抛 vk::SystemError，异常展开跳过它），而"审计/轮询到底跑了多少帧"这个数
	// 只在崩溃的轮次里才有意义——只在正常退出打印 = 系统性排除掉正在调查的那些轮。
	// 注册表按 reporter 去重（`ShutdownLedger`），正常退出时每条账仍只出一遍。
	shutdownLedger().run();
}

/** resize 处理 + 失败日志（文案原样）：转调 resizeSystem；失败打 "Resize 错误: {}"
 * 并把错误原样上抛（调用方 waitForIdle 后退出循环返回）。 */
std::expected<void, Error> resizeChecked(entt::registry& rg, RGL::Swapchain& swapchain,
	RGL::Resources& resources, RLLogger* logger) {
	auto result = resizeSystem(rg, swapchain, resources);
	if (!result && logger) {
		logger->error("Resize 错误: {}", result.error().describe());
	}
	return result;
}

/** drawFrame + 失败日志（文案原样）：成功返回空 optional；失败打 "渲染错误: {}"
 * 并把错误上抛（调用方记录 lastError 后 break）。 */
std::optional<Error> drawFrameChecked(RGL::Renderer& renderer, entt::registry& rg,
	RLLogger* logger, uint32_t frame) {
	auto result = renderer.drawFrame(rg.ctx().get<FrameDrawList>());
	if (result) {
		return {};
	}
	if (logger) {
		logger->error("渲染错误: {}", result.error().describe());
	}
	// 🔴 device fault 抓取**只能在这里**，不能放到 main 的退出路径：设备 lost 之后收尾的
	// waitIdle 会抛 vk::SystemError，异常展开时 DeviceContext 已析构，届时没有任何 device
	// 可问（2026-10-20 实测踩到：hang 那次的日志停在「错误: vk::raii::Device::waitIdle:
	// ErrorDeviceLost」，device_fault 模块一行未出）。本函数是帧循环所有 device-lost 来源
	// （submit / present / fence wait / cull wait）汇流的唯一一点，且此刻 device 还活着。
	if (result.error().code == RGL::ErrorCode::DeviceLost)
		faultCaptureOnLost(renderer, result.error().message, frame);
	return result.error();
}

// LlmCaptureFlow / FrameStatsTracker（帧循环诊断旁路）已拆至 ecs_frame_diag.cppm（批 14.3 文件长度红线；职责边界 = system 语义 vs 观测旁路）

}  // namespace

/** 实现流水（语义/契约见接口声明）：构建显式 system 列表（camera→origin→animation→bounds→
 * projection→renderlist）→ 主循环：eventSystem → AppExit 判定 → dt clamp [0, 0.1s]
 * → resizeSystem（失败 = waitForIdle 后退出循环返回 Error）→ system 链 →
 * texel 金丝雀（帧 N 一次性，drawFrame 前）→ llm 捕获请求（drawFrame 前，
 * dump_gbuf 通道一并传入）→ drawFrame（失败 = 记录 lastError 退出循环）→
 * llm 捕获消费（真有产物时调 llmCapture 写 PNG + 置 AppExit，跳帧时请求保持
 * pending 重试）/ onFrame 回调 → 循环外 waitForIdle 后返回 lastError（若有）。 */
std::expected<void, Error> renderSystem(entt::registry& rg, RGL::Renderer& renderer,
	const RGL::AssetTable& assets,
	const RGL::LlmDebugConfig* llm,
	std::function<std::expected<std::string, Error>(RGL::Renderer&)>
		llmCapture,
	std::function<void(uint32_t)> onFrame) {
	auto& swapchain = renderer.getSwapchainRef();
	auto& resources = renderer.getResourcesRef();
	auto* logger = RLLogger::getInstance();
	std::optional<Error> lastError;

	RenderListState renderListState;
	uint32_t frame = 0;  // 每帧无条件计数（onFrame DIAG 回调用；llmFrame 在捕获流内、仅 llm 启用时递增）
	// texel 回读金丝雀：帧号门控 + 单次触发标记（触发逻辑见 texelCanaryOnce）
	const uint64_t canaryFrame = diagnostics().texelCanary;
	bool canaryFired = false;
	// GPU fault masked 轮询门（device_fault；默认关 = 每帧零 Vulkan 调用，见下面调用点）
	const bool faultPoll = diagnostics().faultPoll;

	// 帧时间统计（状态与打印见 FrameStatsTracker；tick 挂在帧尾）
	FrameStatsTracker stats{diagnostics().frameStats};
	// llm 帧捕获节奏（请求/消费时序与状态见 LlmCaptureFlow）
	LlmCaptureFlow capture{llm, llmCapture};

	// system 顺序（数据流契约）：
	//   camera（InputEvents→Camera，消费即清空）
	//   → animation（SceneSpin）→ bounds（Bounds→SceneBounds）
	//   → projection（Camera+SceneBounds+WindowExtent→ViewProjection，**并生产 RenderOrigin**：
	//   相对坐标批 5 第 1 刀起原点是本 system 内部算的（粒度 ← 级联球半径 ← near/far，三者
	//   唯一同在处），批 4 第二刀那份「view 减原点」现在吃的是它自己刚写的值）
	//   → taa（抖动注入 + 帧间簿记，批 12.6）→ renderlist（→FrameDrawList，实例矩阵
	//   批 3 起、灯数据 eyePos/position 批 4 第二刀起、阴影盒心回减批 5 第 1 刀起吃原点）
	// 🔴 批 4 第一刀新增、批 5 第 1 刀改写的次序约束：**projection 必须排在 taa 与 renderlist
	//   之前** —— 那两者读的都是 projection **本帧**写的原点（taa 还要拿它跟自己记的上一帧原点
	//   算跨帧差）。原来这条由「origin 排在 taa 之前」保证，而 origin 又必须排在 camera 之后；
	//   搬进 projection 之后两个约束合成一个，少一个可错点。
	//   把这几条的相对位置改错的后果不是「图变」而是「图在门关态也不变、门开态静默错位」，
	//   所以它由 tests/test_coordinate_rebase.cppm / test_taa_cross_frame.cppm /
	//   test_view_light_rebase.cppm / test_render_origin.cppm 的闭式用例守（不是由截图锚守）。
	std::vector<FrameSystem> systems{
		{"camera",     [](entt::registry& rg, float dt) { cameraSystem(rg, dt); }},
		{"animation",  [](entt::registry& rg, float dt) { animationSystem(rg, dt); }},
		{"bounds",     [](entt::registry& rg, float) { boundsSystem(rg); }},
		{"projection", [](entt::registry& rg, float) { projectionSystem(rg); }},
		{"taa",        [](entt::registry& rg, float) { taaSystem(rg); }},
		{"renderlist", [&](entt::registry& rg, float) { renderListSystem(rg, resources, renderer.getEnvironmentRef(), assets, renderListState,
			// ★M-1b：本帧录制槽（drawFrame **之前**取；slot 只在 drawFrame 末尾 advance ⟹
			// 采集期与录制期同值，执行器 prepareMdiShadowRows 有机器核对，失配 = 抛）
			renderer.currentFrameSlot()); }},
	};

	float prevTicks = static_cast<float>(SDL_GetTicks());

	while (true) {
		profilerFrameBegin();  // Phase 9：帧槽推进 + 帧墙钟基准（off = 早退）
		{
			auto z = zone("events");  // 拦截点 1（phase9_plan §2.3）：SDL 事件轮询
			eventSystem(rg);
		}
		if (rg.ctx().get<AppExit>().requested) {
			break;
		}

		// 配置热重载帧边界（Phase 10 批 10.3）：本帧任何 system/draw 之前套用变更
		configReloadTick(rg);

		// picking 结果帧边界（Phase 10 批 10.5）：消费上帧回读 → 反查 entity → ctx
		pickingTick(rg);

		// 脚本化输入探针（2026-09-28 输入链回归护栏；配置门控，默认关 = 早退零开销）
		// 债-27：input_probe 住 rlbase 不认识 SDL，合成事件在此适配成 SDL_Event 入队
		// （SDL3 include 面归属 = 拥有该 TU 的库层，见 rgl/CMakeLists.txt 头注释）
		auto pushProbeEvent = [](const ProbeEvent& e) {
			SDL_Event ev{};
			switch (e.kind) {
				case ProbeEventKind::KeyDown:
					ev.type = SDL_EVENT_KEY_DOWN;
					ev.key.key = e.key;
					break;
				case ProbeEventKind::KeyUp:
					ev.type = SDL_EVENT_KEY_UP;
					ev.key.key = e.key;
					break;
				case ProbeEventKind::MouseLeftDown:
				case ProbeEventKind::MouseLeftUp:
					ev.type = e.kind == ProbeEventKind::MouseLeftDown
						? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
					ev.button.button = SDL_BUTTON_LEFT;
					ev.button.x = static_cast<Sint32>(e.x);
					ev.button.y = static_cast<Sint32>(e.y);
					break;
			}
			SDL_PushEvent(&ev);
		};
		uiClickProbeTick(pushProbeEvent);
		cameraProbeTick(rg, pushProbeEvent);

		float ticks = static_cast<float>(SDL_GetTicks());
		// dt 限幅：窗口卡顿/切后台回来时防止相机瞬移
		float dt = std::clamp((ticks - prevTicks) / 1000.0f, 0.0f, 0.1f);
		prevTicks = ticks;

		{
			auto z = zone("resize");  // swapchain/深度重建（罕见路径，触发时单列）
			auto resizeResult = resizeChecked(rg, swapchain, resources, logger);
			if (!resizeResult) {
				renderer.waitForIdle();
				return std::unexpected(std::move(resizeResult).error());
			}
		}

		for (auto& system : systems) {
			auto z = zone(system.name);  // 拦截点 1 核心：显式 system 列表全量计时
			system.fn(rg, dt);
		}

		// texel 金丝雀（本帧 drawFrame 前，waitForIdle 已在函数内排空 in-flight）
		texelCanaryOnce(canaryFired, canaryFrame, frame, renderer, assets);

		// GPU fault masked 轮询（默认关；判据与"为什么在 drawFrame 前"→ faultPollTick 注释）
		faultPollTick(renderer, faultPoll);

		// llm 帧捕获：drawFrame 前请求 → drawFrame → 真产物才消费（时序契约见 LlmCaptureFlow）
		capture.requestBeforeDraw(renderer);
		{
			auto z = zone("drawFrame");  // CPU 命令录制主段（vs 9.2 GPU 跨度对比）
			if (auto drawError = drawFrameChecked(renderer, rg, logger, frame)) {
				lastError = std::move(drawError);
				break;
			}
		}
		{
			auto z = zone("capture");  // llm 帧回读（真截图帧才有 GPU 同步等待）
			if (auto captureError = capture.afterDraw(rg, renderer, logger)) {
				lastError = std::move(captureError);
			}
		}
		if (onFrame)
			onFrame(frame);
		++frame;

		stats.tick();
	}

	renderer.waitForIdle();
	if (lastError) {
		return std::unexpected(std::move(*lastError));
	}
	return {};
}

}  // namespace RGL
