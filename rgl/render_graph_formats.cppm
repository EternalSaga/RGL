module render_graph:formats;

import render_graph;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;

// 附件格式词表分区（2026-10-03 自 render_graph_impl 拆出，解该文件 500 代码行红线）：
// 颜色 / 深度两张**分立**词表 + 成员判定 + 物化。三个导出函数的**语义**权威注释在
// render_graph.cppm 接口（含"两表分立"的理由：合并会让 kind=offscreen 声明出深度格式
// → RT 缓存按颜色建深度图 = invalid usage，只有 VVL 抓得到）。
//
// 为什么表体住实现分区而不是接口：`vk::Format::eXxx` 枚举常量在模块接口分区里**不可见**
// （rlvk 的 GMF 头是"可达但名字不可见"），写出来直接编译报错。

namespace RGL {

/**
 * 格式词表（小写下划线 → vk::Format，接口注释是语义权威）：表驱动，扩展 = 加一行。
 * "inherit" 不在表内——语义 = 随 swapchain 格式，由 resolveRgFormat 单独短路。
 * @return 14 项词表（引用，静态存储）
 */
const std::array<std::pair<std::string_view, vk::Format>, 14>& rgFormatTable() {
	static const std::array<std::pair<std::string_view, vk::Format>, 14> table = {
		std::pair{"rgba8_srgb", vk::Format::eR8G8B8A8Srgb},
		std::pair{"rgba8_unorm", vk::Format::eR8G8B8A8Unorm},
		std::pair{"b8g8r8a8_srgb", vk::Format::eB8G8R8A8Srgb},
		std::pair{"rgb8_srgb", vk::Format::eR8G8B8Srgb},
		std::pair{"rg16_snorm", vk::Format::eR16G16Snorm},
		std::pair{"rg16_unorm", vk::Format::eR16G16Unorm},
		std::pair{"rgba16_sfloat", vk::Format::eR16G16B16A16Sfloat},
		std::pair{"rgb16_sfloat", vk::Format::eR16G16B16Sfloat},
		std::pair{"r32g32b32_sfloat", vk::Format::eR32G32B32Sfloat},
		std::pair{"rgba32_sfloat", vk::Format::eR32G32B32A32Sfloat},
		std::pair{"r32_sfloat", vk::Format::eR32Sfloat},
		std::pair{"rg32_sfloat", vk::Format::eR32G32Sfloat},
		// Phase 10 批 10.5：object-id attachment（picking pass 写 objectIndex+1）
		std::pair{"r32_uint", vk::Format::eR32Uint},
		// Phase 11 批 11.4：SSAO 遮蔽量（ao pass 写、tonemap 采；单通道 8bit 足够——
		// 强度曲线已在 shader 端 pow(·,2) 压过，8bit 量化误差 <0.4%）
		std::pair{"r8_unorm", vk::Format::eR8Unorm},
	};
	return table;
}

/** isKnownRgFormat 定义（接口注释见 render_graph.cppm）："inherit" 短路 + 词表 any_of。 */
bool isKnownRgFormat(const std::string& name) {
	if (name == "inherit")
		return true;
	return std::ranges::any_of(rgFormatTable(),
		[&](const auto& entry) { return entry.first == name; });
}

/**
 * 深度附件格式词表（kind=depthOffscreen 专用；与 rgFormatTable 颜色词表**分立**的原因见
 * render_graph.cppm::isKnownDepthRgFormat 注释）。与颜色词表同住在实现分区：
 * `vk::Format::eXxx` 枚举常量在模块接口分区不可见（rlvk GMF 头可达但名字不可见）。
 */
const std::array<std::pair<std::string_view, vk::Format>, 2>& rgDepthFormatTable() {
	static const std::array<std::pair<std::string_view, vk::Format>, 2> table = {
		// 主用档 = 与主深度图同格式（D32_SFLOAT 是 Vulkan 保证存在的深度格式；
		// **没有 D32_UNORM 这个格式**——写错时 clang 的"did you mean eD16Unorm"极易误导，
		// 2026-09-29 实测踩过）。精度对单级 1024² cascade 足够。
		std::pair{"d32_sfloat", vk::Format::eD32Sfloat},
		std::pair{"d16_unorm", vk::Format::eD16Unorm},  // 省带宽档（多级 cascade 的省内存选择）
	};
	return table;
}

/** isKnownDepthRgFormat 定义（接口注释见 render_graph.cppm）：深度词表 any_of（"inherit" 不算）。 */
bool isKnownDepthRgFormat(const std::string& name) {
	return std::ranges::any_of(rgDepthFormatTable(),
		[&](const auto& entry) { return entry.first == name; });
}

/** resolveRgFormat 定义（接口注释见 render_graph.cppm）："inherit"→fallback，具名先查颜色
 *  词表再查深度词表（kDepthRgFormats），漏网 eUndefined（调用方保证已 isKnown*）。 */
vk::Format resolveRgFormat(const std::string& name, vk::Format inheritFallback) {
	if (name == "inherit")
		return inheritFallback;
	for (const auto& [text, format] : rgFormatTable())
		if (text == name)
			return format;
	for (const auto& [text, format] : rgDepthFormatTable())
		if (text == name)
			return format;
	return vk::Format::eUndefined;  // 不可达（调用方保证已 isKnown*Format）
}

}  // namespace RGL
