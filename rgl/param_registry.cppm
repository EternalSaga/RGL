module;
#include <glm/glm.hpp>

export module param_registry;

import rlstd;

namespace RGL {

/**
 * 调试面板参数条目（Phase 10 批 10.2b；docs/archive/phase10_plan.md §3.2）。
 *
 * **单一事实来源约定**：条目只携带「语义层（label/range/speed）+ 读写回调对」，
 * **不镜像任何 GPU 布局**——字段结构由回调两端各自引用源头类型（ECS 组件 /
 * SceneEnv ctx / 反射生成物），布局改动自动跟随，注册表零副本。写入通道时序 =
 * 改 ctx/组件本体，`renderListSystem` 下一帧消费（生效 = 下一帧）。
 *
 * 条目由 `buildDebugParamPages(registry)` 每帧重建（调试路径，分配可接受；
 * 回调捕获的组件指针在重建帧内即取即用，无跨帧悬垂）。
 */

/** 标量滑条条目（曝光/intensity/range 类）。get 读源、set 写源（下一帧生效）。 */
export struct SliderEntry {
	std::string label;
	float minValue = 0.0f;
	float maxValue = 1.0f;
	float speed = 0.01f;  // 拖拽灵敏度（imgui DragSpeed 语义）
	std::function<float()> get;
	std::function<void(float)> set;
};

/** 颜色编辑条目（灯光色/ambient，HDR 线性域，0..1 滑面 + 自由拖拽）。 */
export struct Color3Entry {
	std::string label;
	std::function<glm::vec3()> get;
	std::function<void(const glm::vec3&)> set;
};

/** 三维拖拽条目（光方向类向量；speed 为 imgui DragFloat3 语义）。 */
export struct Drag3Entry {
	std::string label;
	float minValue = -10.0f;
	float maxValue = 10.0f;
	float speed = 0.05f;
	std::function<glm::vec3()> get;
	std::function<void(const glm::vec3&)> set;
};

/** 整型滑条条目（材质 id 选择器等离散量）。 */
export struct SliderIntEntry {
	std::string label;
	int minValue = 0;
	int maxValue = 0;
	std::function<int()> get;
	std::function<void(int)> set;
};

/** 只读文本行（信息提示：如"材质 M3 · 共享实体数 12"）。 */
export struct TextEntry {
	std::function<std::string()> text;
};

/**
 * 面板跨帧持久状态（registry context，main emplace；param_registry 契约类型）。
 * 选择器类 UI 态（当前页/当前材质）不在条目里（条目每帧重建），集中放这里由
 * buildDebugParamPages 读写。
 */
export struct DebugPanelState {
	uint32_t materialSel = 0;  // 材质页当前材质 id（buildDebugParamPages 每帧按 materialSize 钳位）
};

/** 参数页（面板一个 Tab 页；页名 = 参数组名，如"渲染"/"灯光"）。 */
export struct ParamPage {
	std::string name;
	std::vector<SliderEntry> sliders;
	std::vector<SliderIntEntry> intSliders;
	std::vector<Color3Entry> colors;
	std::vector<Drag3Entry> drags;
	std::vector<TextEntry> texts;
};

}  // namespace RGL
