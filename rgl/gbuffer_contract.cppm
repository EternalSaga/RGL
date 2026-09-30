module;
export module gbuffer_contract;

import rlstd;

// G-buffer 附件契约表（**叶子模块**）——2026-09-30 从 render_graph 抽出。
//
// ## 为什么抽出来
//
// 这张表是纯 constexpr 数据（名字 + 格式串，无任何 Vulkan 类型），消费方里有一个
// 是 **diagnostics_impl**——它只为 `kGBufferAttachments.size()` 这一个数字而
// `import render_graph`，从而把整条 Vulkan 栈（图编译器 / swapchain / RT 缓存 /
// descriptor heap）拖进一个**配置解析器**。后果不只是编译慢：
//
//   1. diagnostics 的配置加载无法进单元测试（测试 target 一旦链上它就得链整个引擎）；
//   2. 依赖图上出现"配置层 → 渲染后端"的边，与 AGENTS.md「配置模块不依赖执行模块」
//      的分层原则直接冲突（当时注释里写的"render_graph 不依赖本模块，无环"只查了
//      环，没查方向）。
//
// ## 抽出的收益
//
// diagnostics_impl 退回干净闭包 → diagnostics + profiler（含 Phase 9 帧相统计）
// 一起可进单元测试；render_graph 侧零改动（`export import` 转出口，见下）。
//
// ## 契约不变
//
// 表内容与顺序**逐字不动**：顺序即 light.frag binding 4-7 + 尾部 11 = emissive
// （见 light.frag.slang），且是 diagnostics.lua `dump_gbuf` 通道号的定义处。改动
// 本表 = 改 G-buffer 通道语义，必须走 shader 契约流程（skill rgl-shader-contract）。

namespace RGL {

/** G-buffer 附件契约条目（名字 + 格式串）。 */
export struct RgGBufferDesc {
	std::string_view name;
	std::string_view format;
};

/**
 * Phase 7 D1：G-buffer 附件契约表（**单一真相源**）——消费方 =
 * deferredRenderGraphDesc（geometry outputs / light inputs 按本表顺序生成）、
 * render_graph_rtslot ensureDeferredGBufferSlots、drawFrame 帧内捕获（dump_gbuf）、
 * diagnostics（dump_gbuf 通道号越界闸门）。
 * 格式串 = isKnownRgFormat 词表可解析名（render_graph::resolveRgFormat 物化）。
 * 顺序即 light.frag binding 4-7 + 尾部 11 = emissive（albedo/normal/position/
 * metalrough/emissive——Phase 8 剩余批次 B 加的第 5 通道 emissive，RGBA16F 保 HDR 域）。
 */
export inline constexpr std::array<RgGBufferDesc, 5> kGBufferAttachments{{
	{"gbuffer_albedo", "rgba8_srgb"},
	{"gbuffer_normal", "rg16_snorm"},
	{"gbuffer_position", "rgba32_sfloat"},
	{"gbuffer_metalrough", "rgba8_unorm"},
	{"gbuffer_emissive", "rgba16_sfloat"},
}};

/** G-buffer 可 dump 通道数（= 表长；diagnostics 的 dump_gbuf 合法上界 + 1）。 */
export inline constexpr int kGBufferChannelCount =
	static_cast<int>(kGBufferAttachments.size());

}  // namespace RGL
