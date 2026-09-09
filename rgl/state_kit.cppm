module;
#include <fmt/format.h>
export module state_kit;

import rlstd;
import rlvk;
import shader;

namespace RGL {

/**
 * pass 动态状态表（Phase 10 批次 0 手术 1：自 Renderer 摘出）。
 *
 * 持有四种命名状态的值集合 + edS3 能力门。状态名（"opaque"/"skybox"/
 * "transparent"/"light"）= render_graph pass.state 的词表，与本类键集合同步
 * （Renderer 构造期 validateRenderGraphForExecutor 校验图内名字全部可解析）。
 * Phase 10 阴影将新增状态条目（depth-only pass 等）——增改集中在本类一处。
 * viewport/scissor 依赖窗口尺寸，不在状态内（applyDynamicState 参数传入）。
 */
export class StateKit {
   public:
	/**
	 * 构建状态表。
	 * @param depthClipEnable edS3 能力门（DeviceContext 启动期一次探测；false =
	 *   扩展缺失，applyDynamicState 内 no-op 干净降级——"扩展缺失不 set"原则）
	 */
	explicit StateKit(bool depthClipEnable) : depthClipDynamic(depthClipEnable) {
		// 透明段状态（Phase 5）：不透明段之外的差异三项——不写深度 + 开 alpha 混合 +
		// 背面剔除。cull 必要：关深度写后封闭网格（如 glTF 盒）的正反两面同屏各混合
		// 一次 = 有效 alpha 翻倍（实测 0.25 → 1-(1-0.25)²=0.4375）；剔除背面后
		// 每像素单层混合，alpha 语义正确（opaque 段保持 eNone——硬编码 cube 绕序未验证）
		transparentState.depthWrite = false;
		transparentState.colorBlend = true;
		transparentState.cullMode = vk::CullModeFlagBits::eBack;
		// 天空盒段状态（Phase 5.5 批次 4）：大三角 z=1（far 平面）+ LEQUAL + 不写深度——
		// 只在未被几何占据的像素（深度 = clear 1.0）通过测试画天空；绕序不剔（cull None）
		skyboxState.depthWrite = false;
		skyboxState.depthCompareOp = vk::CompareOp::eLessOrEqual;
		// Phase 7 D1：deferred light pass —— 全屏终图，无深度附件（depth 无关）、cull none
		//（大三角绕序不剔）、不混合。depthTest 显式关：全屏终图语义（light pass 无深度
		// 附件、大三角 z=1 不测深）
		lightState.depthTest = false;
		lightState.depthWrite = false;
		lightState.cullMode = vk::CullModeFlagBits::eNone;
	}

	/**
	 * 图 pass.state 名 → 动态状态（Renderer 构造期校验保证图内名字全部可解析，
	 * 未知名 = 校验遗漏的编程错误，抛出）。
	 * @param name 状态名（"opaque"/"skybox"/"transparent"/"light"）
	 * @return 状态 const 引用（成员存储，录制期 applyDynamicState 消费）
	 */
	[[nodiscard]] const RenderState& stateFor(std::string_view name) const {
		if (name == "opaque") return opaqueState;
		if (name == "skybox") return skyboxState;
		if (name == "transparent") return transparentState;
		if (name == "light") return lightState;
		throw std::runtime_error(fmt::format(
			"StateKit::stateFor: unknown state '{}' (executor validation bypassed)", name));
	}

	/**
	 * edS3 depthClipEnable 动态状态能力门（applyDynamicState 第 4 参消费）。
	 * @return true = 设备支持 extendedDynamicState3DepthClipEnable，逐 pass 可 set
	 */
	[[nodiscard]] bool depthClipDynamicEnabled() const { return depthClipDynamic; }

   private:
	RenderState opaqueState{};      // 默认值 = cube 的 opaque 状态（全默认）
	RenderState transparentState{}; // 不写深度 + 混合 + 剔背面（见 ctor 注）
	RenderState skyboxState{};      // 大三角 z=1 只在空像素通过（见 ctor 注）
	RenderState lightState{};       // deferred light 全屏终图（见 ctor 注）
	bool depthClipDynamic = false;
};

}  // namespace RGL
