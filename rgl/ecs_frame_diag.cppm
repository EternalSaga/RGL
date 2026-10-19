module;
#include <SDL3/SDL.h>       // SDL_GetTicksNS（FrameStatsTracker 帧尾时刻）
#include <entt/entt.hpp>    // entt::registry（LlmCaptureFlow 置 AppExit）
export module ecs:frame_diag;

/**
 * ecs:frame_diag —— 帧循环里的**诊断旁路**（批 14.3 自 :impl 拆出）
 *
 * 职责边界（为什么这两个 struct 不跟 system 待在一起）：
 * - `:impl` = **system 语义**：camera / animation / bounds / projection / taa / renderlist
 *   与 resize 处理——它们决定"这一帧画什么"。
 * - 本分区 = **观测旁路**：llm_debug 截图时序、帧时间统计。两者都由诊断门控制、
 *   默认关 = 早退零开销，且**不参与本帧画什么的决策**（截图只消费执行器已有的捕获
 *   产物，统计只读时钟）。
 *
 * 为什么单独拆而不是留在 :impl：批 14.3 给渲染网格引入 render extent 后，"这张截图是
 * 哪档 scale 出的"必须写进截图成功日志（判据③），而 :impl 已顶到文件长度红线。
 * 观测旁路与 system 语义的**变更频率和评审关注点都不同**，分开后各自的 diff 更干净。
 *
 * 可见性：本分区是 `ecs` 的**接口分区**，两个 struct 用 `export` 只为让 `:impl` 看见；
 * 主接口 `ecs` 不 `export import` 本分区 ⟹ 模块外不可见（等价于原来的匿名命名空间）。
 *
 * 搬移性质：**纯搬移，非重写**。两个 struct 体逐行原样（git diff 可逐字核），
 * 只加了 `export` 关键字与所在分区的 import 面。
 */

import ecs;        // AppExit
import rlstd;
import vkerror;
import vk_renderer;
import runtime_config;  // LlmDebugConfig
import diagnostics;     // diagnostics().dumpGbuf
import rllogger;

namespace RGL {

/** llm 帧捕获节奏（2026-09-08：截图 = 帧内回读本帧真实输出，离屏重渲退役）：
 * drawFrame 前请求（vkrenderer 帧内钩子，dump_gbuf 通道一并传入）；drawFrame 后
 * 仅当真有捕获产物时消费写 PNG 并请求退出（acquire 超时跳帧 = 请求保持 pending 重试）。 */
export struct LlmCaptureFlow {
	/** 截图配置（nullptr = llm_debug 未启用）。 */
	const RGL::LlmDebugConfig* llm;
	/** 捕获写盘回调（空 = 未启用；引用 renderSystem 形参，同生命周期）。 */
	std::function<std::expected<std::string, Error>(RGL::Renderer&)>& capture;
	/** 每帧计数（含跳帧），wantCapture 判定的基准（仅 llm 启用时递增）。 */
	uint32_t llmFrame = 0;

	/** 是否到捕获时机：llm 启用且已过 delayFrames 帧。 */
	bool wanted() const {
		return llm && capture && llmFrame >= llm->delayFrames;
	}

	/** drawFrame 前挂本帧回读请求（dump_gbuf 通道一并传入）；未到时机不动作。 */
	void requestBeforeDraw(RGL::Renderer& renderer) const {
		if (wanted()) {
			renderer.requestFrameCapture(diagnostics().dumpGbuf);
		}
	}

	/** drawFrame 后消费：真产物时调 consumeShot 写 PNG + 置 AppExit；llm 启用时
	 * 无论是否命中每帧递增 llmFrame（在消费之后，与逐帧时序一致）。
	 * @return 写盘失败错误（调用方记录 lastError；成功或无产物 = 空） */
	std::optional<Error> afterDraw(entt::registry& rg, RGL::Renderer& renderer,
		RLLogger* logger) {
		if (llm && capture) {
			std::optional<Error> error;
			if (wanted() && renderer.hasFrameCaptureFailure()) {
				// 回读源**注定**不存在（`dump_gbuf` 通道越界 / 图里没这个附件）⟹ 必须退出。
				// `wanted()` 过了 delayFrames 就恒真，只等产物的写法在这里得到的是
				// "进程永不退出 + 每帧一条 warn"——与 RPC `/shot` 端点被一次拼错打死是
				// 同一个缺陷（都拿"暂时未就绪"的循环去等一个永久结论），2026-10-19 一并收。
				const auto failure = renderer.takeFrameCaptureFailure();
				// 本路径不是 HTTP，只吃 message（类别是给 RPC 层挑状态码用的，这里没有状态码可挑）
				const std::string why = failure ? failure->message : std::string("unknown");
				if (logger)
					logger->error("llm_debug: 截图回读源不可用（{}）——请求退出，不再空等", why);
				rg.ctx().get<AppExit>().requested = true;
				return Error{ErrorCode::Unknown, "llm_debug: capture source unavailable: " + why};
			}
			if (wanted() && renderer.hasFrameCapture()) {
				error = consumeShot(rg, renderer, logger);
			}
			++llmFrame;  // 每帧计数（含跳帧），wantCapture 判定的基准
			return error;
		}
		return {};
	}

	/** 真有产物时调捕获回调写 PNG + 成功/失败日志（文案原样）+ 置 AppExit。
	 * @return 写盘失败错误（成功 = 空） */
	std::optional<Error> consumeShot(entt::registry& rg, RGL::Renderer& renderer,
		RLLogger* logger) {
		auto shot = capture(renderer);
		std::optional<Error> error;
		if (shot) {
			// 出处口径（批 14.3，判据③）：PNG 是**后缓冲**像素，而它可能是内部网格放大来的。
			// 把两个 extent 与倍率打在同一个成功行里，事后看一张图就能确定"这是哪档 scale
			// 出的"——否则 0.5 档的"变软"会被后来人当成抗锯齿/TAA 回归去查。
			const auto& sc = renderer.getSwapchainRef();
			if (logger)
				logger->info(
					"llm_debug: 截图已保存 ({}) — render {}x{} / swapchain {}x{} (scale {})",
					*shot, sc.getRenderExtent().width, sc.getRenderExtent().height,
					sc.getExtent().width, sc.getExtent().height, sc.getRenderScale());
		} else {
			if (logger) logger->error("llm_debug: 截图失败 ({})", shot.error().describe());
			error = shot.error();
		}
		rg.ctx().get<AppExit>().requested = true;
		return error;
	}
};

/** 帧时间统计（diagnostics.frame_stats = N：每 N 帧一行 avg/min/max，0 = 关）：
 * 帧循环整周期（input+systems+drawFrame），headless 无 present 限帧时 ≈ GPU/提交吞吐。 */
export struct FrameStatsTracker {
	/** 打印周期 N（0 = 关闭）。 */
	uint32_t n;
	/** 上一帧帧尾时刻（ns；0 = 首帧只记起点、不累积）。 */
	uint64_t prevNs = 0;
	/** 本周期帧时长累加（ms）。 */
	double sumMs = 0.0;
	/** 本周期最小帧时长（ms）。 */
	double minMs = 1e30;
	/** 本周期最大帧时长（ms）。 */
	double maxMs = 0.0;
	/** 本周期已累积帧数（达 N 打印并清零）。 */
	uint32_t count = 0;

	/** 帧尾调用：累积本帧整周期；周期满 N 帧打一行 avg/min/max 并复位。 */
	void tick() {
		if (n == 0) {
			return;
		}
		const uint64_t nowNs = SDL_GetTicksNS();  // 帧尾 = 本帧整周期终点
		if (prevNs != 0) {
			const double ms = static_cast<double>(nowNs - prevNs) / 1'000'000.0;
			sumMs += ms;
			++count;
			minMs = std::min(minMs, ms);
			maxMs = std::max(maxMs, ms);
		}
		prevNs = nowNs;
		if (count >= n) {
			if (auto logger = RLLogger::getInstance())
				logger->info("frame stats: avg={:.3f}ms min={:.3f} max={:.3f} ({} 帧)",
					sumMs / count, minMs, maxMs, count);
			sumMs = 0.0;
			count = 0;
			minMs = 1e30;
			maxMs = 0.0;
		}
	}
};
}  // namespace RGL
