module;
#include <fmt/format.h>
export module state_kit;

import rlstd;
import rlvk;
import shader;
import pass_vocabulary;  // kAllPassStates（状态名词表单一真相源）+ stateNamesCoverAll

namespace RGL {

/**
 * pass 动态状态表（Phase 10 批次 0 手术 1：自 Renderer 摘出）。
 *
 * 持有全部命名状态的**值集合**（名单不在本注释抄写，真相源 = kAllPassStates）+ edS3 能力门。状态名 = pass_vocabulary 的
 * `kAllPassStates`（单一真相源：名字与类别都在那张表，本类的键集合由
 * `stateNamesCoverAll` 在编译期与它绑定）；图侧"哪些名字合法"按类别分档校验
 * （Renderer 构造期 validateRenderGraphForExecutor）。viewport/scissor 依赖窗口尺寸，
 * 不在状态内（applyDynamicState 参数传入；批 11.2 起 viewport 尺寸随 pass 渲染区域走，
 * 阴影图用其独立分辨率）。
 *
 * **加一个状态的流程**：pass_vocabulary 表加一行（含 scope）→ 本类加成员 + ctor 里
 * 写值 + kStateSlots 加一条。漏了最后一条 = static_assert 编译不过（本类曾与执行器
 * 词表、单测、Lua 头注释各抄一份名单，批 11.3-1d 加 izb_mask 时就漏了四处）。
 */
export class StateKit {
   public:
	/**
	 * 构建状态表。
	 * @param depthClipEnable edS3 能力门（DeviceContext 启动期一次探测；false =
	 *   扩展缺失，applyDynamicState 内 no-op 干净降级——"扩展缺失不 set"原则）
	 */
	explicit StateKit(bool depthClipEnable, bool conservativeDynamic = false)
		: depthClipDynamic(depthClipEnable), conservativeDynamic(conservativeDynamic) {
		// 透明段状态（Phase 5）：不透明段之外的差异三项——不写深度 + 开 alpha 混合 +
		// 背面剔除。cull 必要：关深度写后封闭网格（如 glTF 盒）的正反两面同屏各混合
		// 一次 = 有效 alpha 翻倍（实测 0.25 → 1-(1-0.25)²=0.4375）；剔除背面后
		// 每像素单层混合，alpha 语义正确。
		// opaque 段保持 eNone：**绕序已于批 12.1 核验**（tests/test_cube_geometry.cppm：
		// 内建 cube 的法线朝内，从外面看是背面）。故本段 eBack 剔掉的是朝外面、
		// 保留内部面——封闭网格轮廓与"单层混合"目标都仍达成，但**带纹理的透明体会
		// 显示内侧**。glTF 资产绕序由外部数据决定，本段不因 cube 的绕序而改变语义。
		transparentState.depthWrite = false;
		transparentState.colorBlend = true;
		transparentState.cullMode = vk::CullModeFlagBits::eBack;
		// 天空盒段状态（skyboxState，大三角 z=1 + LEQUAL + 不写深度）已随 forward 绘制
		// 机制删除（2026-10-18 图变体收敛刀②.2）。
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
		// 批 12.6：taa pass —— 与 ao 同形（全屏、无深度附件参与（深度只作 sampleRead
		// 输入做重投影）、绕序不剔、不混合；写 taa_out 由 tonemap 消费）
		taaState.depthTest = false;
		taaState.depthWrite = false;
		taaState.cullMode = vk::CullModeFlagBits::eNone;
		// 批 14.5：ao_apply pass —— 与 ao/taa 同形（全屏、不挂深度附件、绕序不剔、不混合；
		// 写 ao_applied 供时域滤波器（FSR）消费。见 shaders/ao_apply.frag.slang 头注释）
		aoApplyState.depthTest = false;
		aoApplyState.depthWrite = false;
		aoApplyState.cullMode = vk::CullModeFlagBits::eNone;
		// 批 15.3-B：ssr_trace pass —— 与 ao/taa 同形（半分辨率全屏 march、不挂深度附件、
		// 绕序不剔、**不混合**：写 ssr_trace RT 的是预乘结果，不是叠加项）。
		// 批 15.1：ssr pass —— 与 ao/taa 同形（全分辨率、**不挂深度附件**（深度/G-buffer 只作
		// sampleRead 输入）、绕序不剔、不混合；模糊后 additive 合成写 ssr_applied。
		// 见 shaders/ssr_trace.frag.slang / ssr_apply.frag.slang 头注释）
		ssrTraceState.depthTest = false;
		ssrTraceState.depthWrite = false;
		ssrTraceState.cullMode = vk::CullModeFlagBits::eNone;
		ssrState.depthTest = false;
		ssrState.depthWrite = false;
		ssrState.cullMode = vk::CullModeFlagBits::eNone;
		// 批 14.4：velocity pass —— 与 ao/taa 同形（全屏、**不挂深度附件**（深度只作
		// sampleRead 输入做重投影）、绕序不剔、不混合；写 velocity RT 供 FSR / 诊断消费）
		velocityState.depthTest = false;
		velocityState.depthWrite = false;
		velocityState.cullMode = vk::CullModeFlagBits::eNone;
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
		// 批 11.3-1d：IZB mask pass —— 零附件光空间重放（viewport = 光网格 G×G）：
		// 无深度测试/写入（遮挡判定在 frag 走链，光栅只出覆盖）、双面（投影体背面也
		// 遮光）、**OVERESTIMATE 保守覆盖**（FTIZB 正确性命门：cell 被三角"擦边"必须
		// 出 fragment，判定精度由 edge test 收，1d spike 判据③' 钉死）。
		izbMaskState.depthTest = false;
		izbMaskState.depthWrite = false;
		izbMaskState.cullMode = vk::CullModeFlagBits::eNone;
		izbMaskState.conservative = true;
	}

	/**
	 * 图 pass.state 名 → 动态状态（Renderer 构造期校验保证图内名字全部可解析，
	 * 未知名 = 校验遗漏的编程错误，抛出）。
	 *
	 * 表驱动（2026-10-02）：名字集合 = pass_vocabulary::kAllPassStates，编译期由
	 * `stateNamesCoverAll` 绑定（本表加名 / 词表加名而另一侧没跟 = 编译不过）。
	 * @param name 状态名（全量词表见 pass_vocabulary::kAllPassStates）
	 * @return 状态 const 引用（成员存储，录制期 applyDynamicState 消费）
	 */
	[[nodiscard]] const RenderState& stateFor(std::string_view name) const {
		for (const auto& slot : kStateSlots)
			if (slot.name == name)
				return this->*(slot.state);
		throw std::runtime_error(fmt::format(
			"StateKit::stateFor: unknown state '{}' (executor validation bypassed)", name));
	}

	/**
	 * edS3 depthClipEnable 动态状态能力门（applyDynamicState 第 4 参消费）。
	 * @return true = 设备支持 extendedDynamicState3DepthClipEnable，逐 pass 可 set
	 */
	[[nodiscard]] bool depthClipDynamicEnabled() const { return depthClipDynamic; }

	/**
	 * 批 11.3-1d：EDS3 保守光栅模式动态位能力门（applyDynamicState 第 6 参消费）。
	 * @return true = 设备启用 extendedDynamicState3ConservativeRasterizationMode
	 */
	[[nodiscard]] bool conservativeDynamicEnabled() const { return conservativeDynamic; }

   private:
	RenderState opaqueState{};      // 默认值 = cube 的 opaque 状态（全默认）
	RenderState transparentState{}; // 不写深度 + 混合 + 剔背面（见 ctor 注）
	RenderState lightState{};       // deferred light 全屏终图（见 ctor 注）
	RenderState tonemapState{};     // Phase 8 ①：HDR→LDR tonemap 全屏终图（与 light 同形，见 ctor 注）
	RenderState aoState{};          // Phase 11 批 11.4：SSAO 全屏终图（与 tonemap 同形，见 ctor 注）
	RenderState velocityState{};    // 批 14.4：逐像素速度场 pass（与 ao 同形，见 ctor 注）
	RenderState taaState{};         // 批 12.6：TAA 全屏 pass（与 ao 同形，见 ctor 注）
	RenderState aoApplyState{};     // 批 14.5：AO 施加专用全屏 pass（与 ao 同形，见 ctor 注）
	RenderState ssrTraceState{};    // 批 15.3-B：半分辨率 SSR march pass（与 ao 同形，见 ctor 注）
	RenderState ssrState{};         // 批 15.1：屏幕空间反射全屏 pass（与 ao 同形，见 ctor 注）
	RenderState pickingState{};     // Phase 10 批 10.5：id pass 复用 scene depth 只测不写（见 ctor 注）
	RenderState highlightState{};   // Phase 10 批 10.7：高亮 pass 叠加成片（混合 + 剔背面，见 ctor 注）
	RenderState shadowState{};      // Phase 11 批 11.2：阴影 depth-only pass（写 depthOffscreen，见 ctor 注）
	RenderState izbMaskState{};     // 批 11.3-1d：IZB mask 光空间重放（零附件 + 保守覆盖，见 ctor 注）
	bool depthClipDynamic = false;
	bool conservativeDynamic = false;  ///< 批 11.3-1d：EDS3 保守光栅动态位能力门

	/** 状态名 → 成员存储（stateFor 表驱动查表用；名字侧真相在 pass_vocabulary）。 */
	struct StateSlot {
		std::string_view name;
		const RenderState StateKit::* state;
	};

	/**
	 * 名字 ↔ 成员指针表（顺序 = kAllPassStates 声明序，长度由词表定）。
	 * 与词表的同名同数由下面的 static_assert 保证：**加状态名忘了在这里登记
	 * （或反之）= 编译不过**，不再是"运行到那条 pass 才抛 / 永远走不到的死名"。
	 */
	static constexpr std::array<StateSlot, kAllPassStates.size()> kStateSlots{{
		{"opaque", &StateKit::opaqueState},
		{"transparent", &StateKit::transparentState},
		{"picking", &StateKit::pickingState},
		{"highlight", &StateKit::highlightState},
		{"shadow", &StateKit::shadowState},
		{"light", &StateKit::lightState},
		{"ao", &StateKit::aoState},
		{"velocity", &StateKit::velocityState},
		{"taa", &StateKit::taaState},
		{"ao_apply", &StateKit::aoApplyState},
		{"ssr_trace", &StateKit::ssrTraceState},
		{"ssr", &StateKit::ssrState},
		{"tonemap", &StateKit::tonemapState},
		{"izb_mask", &StateKit::izbMaskState},
	}};
	static_assert(stateNamesCoverAll(kStateSlots),
		"StateKit::kStateSlots 与 pass_vocabulary::kAllPassStates 必须同名同数"
		"（加状态 = 两处各一行；词表还决定它被哪档图校验承认）");
};

}  // namespace RGL
