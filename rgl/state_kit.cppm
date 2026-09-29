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
 * 持有九种命名状态的值集合 + edS3 能力门。状态名（"opaque"/"skybox"/
 * "transparent"/"light"/"tonemap"/"ao"/"picking"/"highlight"/"shadow"）= render_graph
 * pass.state 的词表，与本类键集合同步（Renderer 构造期 validateRenderGraphForExecutor
 * 校验图内名字全部可解析）。Phase 11 阴影新增 shadow 条目（depth-only pass）——增改集中在
 * 本类一处。viewport/scissor 依赖窗口尺寸，不在状态内（applyDynamicState 参数传入；
 * 批 11.2 起 viewport 尺寸随 pass 渲染区域走，阴影图用其独立分辨率）。
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
		// Phase 8 批次 ①：tonemap pass —— 与 light 同形（全屏终图、无深度附件、绕序不剔、
		// 不混合；scene_hdr→swapchain 全屏覆盖，深度无关）
		tonemapState.depthTest = false;
		tonemapState.depthWrite = false;
		tonemapState.cullMode = vk::CullModeFlagBits::eNone;
		// Phase 11 批 11.4：ao pass —— 与 tonemap 同形（全屏终图、**不挂深度附件**
		// （深度只作 sampleRead 输入，渲染期不参与深度测试/写入）、绕序不剔、不混合）。
		aoState.depthTest = false;
		aoState.depthWrite = false;
		aoState.cullMode = vk::CullModeFlagBits::eNone;
		// Phase 10 批 10.5：picking pass —— depth **只测不写**（复用 scene depth 得可见层，
		// 不污染后续 transparent 段）；其余 = opaque 同（cull 后端面剔与 opaque 一致即可，
		// id 归属 = 可见前表面）
		pickingState.depthWrite = false;
		pickingState.depthCompareOp = vk::CompareOp::eLessOrEqual;  // 与 scene depth 同面等值（eLess 全拒 = 实测踩坑）
		// Phase 10 批 10.7：highlight pass —— depth 只测不写（与 picking 同理：被高亮物体
		// 已在 scene depth 里，此处只借它做可见性测试）+ 开 alpha 混合（高亮是叠加在成片
		// 上的半透明边缘光）+ 剔背面（同 transparent：双面材质若不剔，正反两面各混合一次
		// = 边缘光翻倍）。depthCompareOp 保持 eLessOrEqual（与已渲面等值，见 picking 踩坑）。
		highlightState.depthWrite = false;
		highlightState.depthCompareOp = vk::CompareOp::eLessOrEqual;
		highlightState.colorBlend = true;
		highlightState.cullMode = vk::CullModeFlagBits::eBack;
		// Phase 11 批 11.2 基建：阴影深度 pass 状态——**depth-only**（无色附件，写
		// depthOffscreen 阴影图）。显式写死三项意图，不靠 RenderState 默认值兜：
		// ① depthWrite = true + ② depthCompare = LESS（阴影图 = 光视最近深度，clear 1.0）；
		// ③ cullMode = **None**（双面写入）——配合"RADV 无保守光栅外扩余量"的结论
		// （docs/phase11_plan.md §9：maxExtraPrimitiveOverestimationSize = 0，薄几何/单面片
		// 无法靠 fattening 挡漏光）。**尚未调**：slope-scaled depth bias 与采样侧 normal
		// offset 要等深度-only 管线接上后按实测调（现在填数就是猜），见 §10.4 取舍 1。
		shadowState.depthWrite = true;
		shadowState.depthCompareOp = vk::CompareOp::eLess;
		shadowState.cullMode = vk::CullModeFlagBits::eNone;
	}

	/**
	 * 图 pass.state 名 → 动态状态（Renderer 构造期校验保证图内名字全部可解析，
	 * 未知名 = 校验遗漏的编程错误，抛出）。
	 * @param name 状态名（"opaque"/"skybox"/"transparent"/"light"/"tonemap"/"ao"/
	 *   "picking"/"highlight"）
	 * @return 状态 const 引用（成员存储，录制期 applyDynamicState 消费）
	 */
	[[nodiscard]] const RenderState& stateFor(std::string_view name) const {
		if (name == "opaque") return opaqueState;
		if (name == "skybox") return skyboxState;
		if (name == "transparent") return transparentState;
		if (name == "light") return lightState;
		if (name == "tonemap") return tonemapState;
		if (name == "ao") return aoState;
		if (name == "picking") return pickingState;
		if (name == "highlight") return highlightState;
		if (name == "shadow") return shadowState;
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
	RenderState tonemapState{};     // Phase 8 ①：HDR→LDR tonemap 全屏终图（与 light 同形，见 ctor 注）
	RenderState aoState{};          // Phase 11 批 11.4：SSAO 全屏终图（与 tonemap 同形，见 ctor 注）
	RenderState pickingState{};     // Phase 10 批 10.5：id pass 复用 scene depth 只测不写（见 ctor 注）
	RenderState highlightState{};   // Phase 10 批 10.7：高亮 pass 叠加成片（混合 + 剔背面，见 ctor 注）
	RenderState shadowState{};      // Phase 11 批 11.2：阴影 depth-only pass（写 depthOffscreen，见 ctor 注）
	bool depthClipDynamic = false;
};

}  // namespace RGL
