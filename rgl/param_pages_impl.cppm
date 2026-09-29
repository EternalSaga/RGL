module;
#include <entt/entt.hpp>  // registry ctx/view（分区实现单元同 GMF）
#include <glm/glm.hpp>    // glm::vec3/vec4（控件颜色写通道）

module param_pages:impl;

import param_pages;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import param_registry;        // ParamPage 条目结构
import asset_table;           // AssetTable/MaterialAsset（材质写通道）
import param_semantics_generated;  // 面板控件语义元数据（label/range/speed，批 10.6）
import lights_types_generated;      // kMaxLights（灯槽数组上界）
import ecs;               // SelectedObject ctx
import scene_context;     // Light / MaterialRef / SceneEnv 组件与 ctx

namespace RGL {

/** 材质五参数控件块（批 10.2c 材质页 / 批 10.5 "选中"页共用；调用方保证
 *  matId < assets.materialSize()）。写通道 = getMaterialMut（renderListSystem 每帧
 *  读本表写 ObjectDraw = 下一帧生效）；材质表仅注册期增长 = 引用地址稳定。
 *  语义（label/range/speed）= param_semantics_generated（批 10.6，数据源
 *  rgl/param_semantics.json）——本函数只留写入通道。 */
void appendMaterialControls(ParamPage& page, RGL::AssetTable& assets, uint32_t matId) {
	MaterialAsset& mat = assets.getMaterialMut(matId);
	page.colors.push_back({.label = std::string(semantics::kMatBaseColor.label),
		.get = [&mat] { return glm::vec3(mat.baseColor); },
		.set = [&mat](const glm::vec3& v) { mat.baseColor = glm::vec4(v, mat.baseColor.a); }});
	page.sliders.push_back({.label = std::string(semantics::kMatAlpha.label),
		.minValue = semantics::kMatAlpha.minValue, .maxValue = semantics::kMatAlpha.maxValue,
		.speed = semantics::kMatAlpha.speed,
		.get = [&mat] { return mat.baseColor.a; },
		.set = [&mat](float v) { mat.baseColor.a = v; }});
	page.sliders.push_back({.label = std::string(semantics::kMatRoughness.label),
		.minValue = semantics::kMatRoughness.minValue, .maxValue = semantics::kMatRoughness.maxValue,
		.speed = semantics::kMatRoughness.speed,
		.get = [&mat] { return mat.roughness; },
		.set = [&mat](float v) { mat.roughness = v; }});
	page.sliders.push_back({.label = std::string(semantics::kMatMetallic.label),
		.minValue = semantics::kMatMetallic.minValue, .maxValue = semantics::kMatMetallic.maxValue,
		.speed = semantics::kMatMetallic.speed,
		.get = [&mat] { return mat.metallic; },
		.set = [&mat](float v) { mat.metallic = v; }});
	page.colors.push_back({.label = std::string(semantics::kMatEmissive.label),
		.get = [&mat] { return mat.emissive; },
		.set = [&mat](const glm::vec3& v) { mat.emissive = v; }});
	page.sliders.push_back({.label = std::string(semantics::kMatAlphaCutoff.label),
		.minValue = semantics::kMatAlphaCutoff.minValue, .maxValue = semantics::kMatAlphaCutoff.maxValue,
		.speed = semantics::kMatAlphaCutoff.speed,
		.get = [&mat] { return mat.alphaModeMask ? mat.alphaCutoff : 0.0f; },
		.set = [&mat](float v) { mat.alphaCutoff = v; }});
}

/** buildDebugParamPages 定义（接口注释见 param_pages.cppm）。 */
std::vector<ParamPage> buildDebugParamPages(entt::registry& rg, RGL::AssetTable& assets) {
	ParamPage renderPage{.name = "渲染"};
	auto& env = rg.ctx().get<SceneEnv>();
	renderPage.sliders.push_back({.label = std::string(semantics::kEnvExposure.label),
		.minValue = semantics::kEnvExposure.minValue, .maxValue = semantics::kEnvExposure.maxValue,
		.speed = semantics::kEnvExposure.speed,
		.get = [&env] { return env.exposure; },
		.set = [&env](float v) { env.exposure = v; }});
	// Phase 11 批 11.4：SSAO 强度（ao.strength → scene data @60；0 = 逐位旁路）
	renderPage.sliders.push_back({.label = std::string(semantics::kAoStrength.label),
		.minValue = semantics::kAoStrength.minValue, .maxValue = semantics::kAoStrength.maxValue,
		.speed = semantics::kAoStrength.speed,
		.get = [&env] { return env.aoStrength; },
		.set = [&env](float v) { env.aoStrength = v; }});
	renderPage.sliders.push_back({.label = std::string(semantics::kEnvIntensity.label),
		.minValue = semantics::kEnvIntensity.minValue, .maxValue = semantics::kEnvIntensity.maxValue,
		.speed = semantics::kEnvIntensity.speed,
		.get = [&env] { return env.intensity; },
		.set = [&env](float v) { env.intensity = v; }});
	renderPage.sliders.push_back({.label = std::string(semantics::kEnvMipRange.label),
		.minValue = semantics::kEnvMipRange.minValue, .maxValue = semantics::kEnvMipRange.maxValue,
		.speed = semantics::kEnvMipRange.speed,
		.get = [&env] { return env.mipRange; },
		.set = [&env](float v) { env.mipRange = v; }});

	ParamPage lightPage{.name = "灯光"};
	std::array<Light*, kMaxLights> slots{};
	rg.view<Light>().each([&slots](Light& L) {
		if (L.index < kMaxLights)
			slots[L.index] = &L;  // 组件池地址稳定（面板不增删 entity）
	});
	for (uint32_t i = 0; i < kMaxLights; ++i) {
		Light* p = slots[i];
		if (!p)
			continue;  // 画不画 = 实体有无（灯位空缺 = 无该 index 的 entity）
		const std::string tag = std::to_string(i);
		lightPage.sliders.push_back({.label = std::string(semantics::kLightIntensity.label) + tag,
			.minValue = semantics::kLightIntensity.minValue,
			.maxValue = semantics::kLightIntensity.maxValue,
			.speed = semantics::kLightIntensity.speed,
			.get = [p] { return p->intensity; },
			.set = [p](float v) { p->intensity = v; }});
		lightPage.colors.push_back({.label = std::string(semantics::kLightColor.label) + tag,
			.get = [p] { return p->color; },
			.set = [p](const glm::vec3& v) { p->color = v; }});
		lightPage.drags.push_back({.label = std::string(semantics::kLightDirection.label) + tag,
			.minValue = semantics::kLightDirection.minValue,
			.maxValue = semantics::kLightDirection.maxValue,
			.speed = semantics::kLightDirection.speed,
			.get = [p] { return p->direction; },
			.set = [p](const glm::vec3& v) { p->direction = v; }});
		if (i == 0)  // 主光专属：场景共享 ambient（scene data @36 仅此灯生效）
			lightPage.colors.push_back({.label = std::string(semantics::kLightAmbient.label),
				.get = [p] { return p->ambient; },
				.set = [p](const glm::vec3& v) { p->ambient = v; }});
	}

	std::vector<ParamPage> out;
	out.push_back(std::move(renderPage));
	out.push_back(std::move(lightPage));

	// 材质页（Phase 10 批 10.2c）：id 选择器（DebugPanelState 持久）→ 选中材质五参数
	//（baseColor rgb+α / metallic / roughness / emissive / alphaCutoff）+ 共享实体数提示
	//（材质值去重共享，编辑影响全部引用者——面板明示）。
	ParamPage matPage{.name = "材质"};
	if (auto* panel = rg.ctx().find<DebugPanelState>(); panel != nullptr && assets.materialSize() > 0) {
		panel->materialSel = std::min(panel->materialSel, assets.materialSize() - 1);
		const uint32_t sel = panel->materialSel;
		matPage.intSliders.push_back({.label = "材质 id",
			.minValue = 0, .maxValue = static_cast<int>(assets.materialSize()) - 1,
			.get = [panel] { return static_cast<int>(panel->materialSel); },
			.set = [panel](int v) { panel->materialSel = static_cast<uint32_t>(v); }});
		uint32_t sharedCount = 0;
		rg.view<MaterialRef>().each(
			[&](const MaterialRef& m) { if (m.material == sel) ++sharedCount; });
		matPage.texts.push_back({.text = [sel, sharedCount] {
			return "材质 M" + std::to_string(sel) + " · 共享该材质的实体数 " +
				std::to_string(sharedCount); }});
		appendMaterialControls(matPage, assets, sel);
	}

	out.push_back(std::move(matPage));

	// "选中"页（Phase 10 批 10.5，D5 点选实体上下文）：SelectedObject ctx（pickingTick
	// 反查写入）→ 实体信息行 + 该实体材质五参数（appendMaterialControls 复用，编辑
	// 影响共享该材质的全部实体——同材质页语义）。未选中/钩子未挂 = 空页提示。
	ParamPage selPage{.name = "选中"};
	if (auto* picked = rg.ctx().find<SelectedObject>();
		picked && picked->entity != static_cast<entt::entity>(entt::null)) {
		const auto entity = picked->entity;
		const auto slot = picked->objectSlot;
		const uint32_t matId = rg.get<MaterialRef>(entity).material;
		// 批 10.7：共享实体数（材质是共享资产，编辑波及全部引用者——与「材质」页同一
		// 口径明示范围；高亮也按材质染，同材质实体一起亮）
		uint32_t sharedCount = 0;
		rg.view<MaterialRef>().each(
			[&](const MaterialRef& m) { if (m.material == matId) ++sharedCount; });
		selPage.texts.push_back({.text = [slot, matId, sharedCount] {
			return "点中 objectSlot " + std::to_string(slot) + " → 材质 M"
				+ std::to_string(matId) + " · 共享该材质的实体数 "
				+ std::to_string(sharedCount); }});
		if (matId < assets.materialSize())
			appendMaterialControls(selPage, assets, matId);
		// 批 10.7：点选跳转——本帧有新选中时把本页标为跳转目标（消费即清零，故不会
		// 把用户从别的页反复弹回）。imgui 层只认这个标记，不认页名字符串。
		if (auto* panel = rg.ctx().find<DebugPanelState>();
			panel != nullptr && panel->pendingSelectJump) {
			selPage.selectOnPick = true;
			panel->pendingSelectJump = false;
		}
	} else {
		selPage.texts.push_back({.text = [] {
			return std::string("未选中——picking 开启后左键点击模型（面板外区域）；"
				"Esc 取消选中"); }});
	}
	out.push_back(std::move(selPage));
	return out;
}

}  // namespace RGL
