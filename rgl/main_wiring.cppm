module;
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <filesystem>  // 监视源 configs/ 目录存在性探测（GMF 例外清单成员，同 main:scene）

// 入口模块 main 的分区实现单元（module main:wiring;）——**诊断/热重载挂点装配**三件套
// （wireHotReload / wireShaderHotReload / wirePicking）的定义。
//
// 【为什么从 main:scene 拆出来】批 14.5 第三批给 main:scene 加 AO 施加 pass 的装配时撞上
// 「单文件代码行 ≤500」红线（该文件此前已顶在 500）。红线不是要绕的，是要找缝的：这三个
// wire* 是**整文件里唯一与"启动期后端装配"零耦合**的一段——它们不碰本文件的 file-static
// helper（orThrow / deferredShadowSlotOf / evssmMomentsSlotOf），不碰 GPU 资源创建，
// 只做「构造服务 + emplace 进 registry.ctx + 闭包桥」。拆出去后两边各自单一职责：
// main:scene = 装配后端，main:wiring = 挂诊断钩子。
//
// 可见性同姊妹分区：声明在 main.cppm（export），main() 所在的 main:impl 经主接口可见本单元
// 定义（同模块分区，无需互 import）。
module main:wiring;

import main;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import vk_resources;
import render_target_cache;  // RenderTargetCache（wirePicking 形参）
import vk_renderer;  // Renderer（挂 picking 服务 + 取渲染图引用）
import shader_registry;  // ShaderRegistry（wireShaderHotReload 形参）
import scene_config;  // loadSceneConfig（scene.lua 热套用）
import scene_context;  // SceneSpin / SceneEnv（热套字段所在 ctx）
import camera_config;  // loadCameraConfig（hot_reload camera.lua 套用，Phase 10 批 10.3）
import camera;  // Camera ctx 类型（hot_reload 调参字段套用，10.3）
import hot_reload;  // HotReloader（配置热重载机制模块，10.3）
import shader_hot_reload;  // ShaderHotReloader（shader 热重载 L1，10.4）
import picking;  // PickingService（wirePicking 桥接，10.5）
import ecs;  // ConfigReloadHook / PickingHook ctx（帧边界挂点类型，10.3）
import diagnostics;
import rllogger;

namespace RGL {

/**
 * wireHotReload 定义（契约见 main.cppm 接口注释）。三条监视链 = 帧边界
 * HotReloader::poll 触发；回调只碰 ctx 值字段（不触碰 GPU 资源/资产表/registry 结构），
 * 解析失败 warn + 保留旧值。reloader 所有权在 hook 闭包内（ctx 活多久它活多久）。
 */
void wireHotReload(entt::registry& registry) {
	if (!diagnostics().configReload)
		return;  // 总开关默认关 = 零行为变化（红线）
	// 监视源：RGL_CONFIG_DIR 覆盖时优先（测试用一次性配置目录），否则回退编译期源树
	// configs/（D7——build 副本会被 just build 覆盖复原，监听它无意义）。
	// env 已设 = 该目录由调用方负责存在性与生命周期。
	std::filesystem::path sourceDir;
	if (const char* dir = std::getenv("RGL_CONFIG_DIR"); dir && *dir) {
		sourceDir = dir;
	} else {
#ifdef RGL_SOURCE_CONFIG_DIR
		sourceDir = RGL_SOURCE_CONFIG_DIR;
#else
		if (auto* logger = RLLogger::getInstance())
			logger->warn("hot_reload: 编译期未注入 RGL_SOURCE_CONFIG_DIR，热重载关闭");
		return;
#endif
	}
	std::error_code ec;
	if (!std::filesystem::is_directory(sourceDir, ec)) {
		if (auto* logger = RLLogger::getInstance())
			logger->warn("hot_reload: 源 configs 目录不存在（热重载关闭）: {}", sourceDir.string());
		return;
	}

	auto reloader = std::make_shared<HotReloader>();
	reloader->watch(sourceDir / "scene.lua", [&registry](const std::string& path) {
		auto cfg = loadSceneConfig(path);
		auto* logger = RLLogger::getInstance();
		if (!cfg) {
			if (logger)
				logger->warn("hot_reload: scene.lua 解析失败（保留旧值）: {}", cfg.error().message);
			return;
		}
		registry.ctx().get<SceneSpin>().speed = cfg->spin ? 1.0f : 0.0f;
		// 帧驱动 spin 同路热载（门默认 0 = 关；见 SceneSpin / main_impl 同段注释）
		registry.ctx().get<SceneSpin>().frameAngle = cfg->spinFrameAngle;
		registry.ctx().get<SceneSpin>().axis = cfg->spinAxis;
		auto& env = registry.ctx().get<SceneEnv>();
		env.exposure = cfg->exposure;
		env.aoStrength = cfg->aoStrength;  // Phase 11 批 11.4：SSAO 强度（热重载同路）
		env.intensity = cfg->environment.intensity;  // env 未上传时 mipRange=0 = shader 端无贡献，安全
		if (logger)
			logger->info("hot_reload: scene.lua 已套用 spin={} exposure={} env_intensity={}（entities/灯不热载）",
				cfg->spin, cfg->exposure, cfg->environment.intensity);
	});
	reloader->watch(sourceDir / "camera.lua", [&registry](const std::string& path) {
		auto cfg = loadCameraConfig(path);
		auto* logger = RLLogger::getInstance();
		if (!cfg) {
			if (logger)
				logger->warn("hot_reload: camera.lua 解析失败（保留旧值）: {}", cfg.error().message);
			return;
		}
		const Camera& src = cfg->camera;
		auto& cam = registry.ctx().get<Camera>();
		cam.fov = src.fov;
		cam.nearPlane = src.nearPlane;
		cam.farPlane = src.farPlane;
		cam.fpsSpeed = src.fpsSpeed;
		cam.fpsSensitivity = src.fpsSensitivity;
		cam.trackballSensitivity = src.trackballSensitivity;
		cam.zoomSpeed = src.zoomSpeed;
		cam.keys = src.keys;  // 键位表（CameraState 无键相关缓存，换表安全）
		if (logger)
			logger->info("hot_reload: camera.lua 已套用 fov={}（姿态/mode 不热套，重启生效）",
				glm::degrees(cam.fov));
	});
	reloader->watch(sourceDir / "log_level.json", [](const std::string& path) {
		RLLogger::reloadLevels(path);
	});

	registry.ctx().emplace<ConfigReloadHook>(ConfigReloadHook{
		.poll = [r = std::move(reloader)] { r->poll(); }});
	if (auto* logger = RLLogger::getInstance())
		logger->info("hot_reload: 已启动，监视源树 {}（scene.lua / camera.lua / log_level.json）",
			sourceDir.string());
}

/**
 * wireShaderHotReload 定义（契约见 main.cppm 接口注释）。复用 ConfigReloadHook
 * 帧边界挂点：config_reload 已挂 = 组合追加（配置链先跑），否则独占 emplace；
 * reloader 所有权在闭包内（ctx 活多久它活多久，同 10.3 模式）。init 失败
 * （编译期三件套缺失 / 基线 SPV 读不到）= 降级关，ShaderHotReloader 内部已日志。
 */
void wireShaderHotReload(entt::registry& registry, ShaderRegistry& pipelines,
	RenderBackEnd& backEnd) {
	if (!diagnostics().shaderReload)
		return;  // 总开关默认关 = 零行为变化（红线）
	auto reloader = std::make_shared<ShaderHotReloader>(pipelines, backEnd.renderer,
		backEnd.dgc.get(), backEnd.dgcStatic.get());
	if (!reloader->init())
		return;
	if (auto* hook = registry.ctx().find<ConfigReloadHook>(); hook && hook->poll) {
		auto prev = std::move(hook->poll);
		hook->poll = [prev = std::move(prev), r = std::move(reloader)] {
			prev();
			r->poll();
		};
	} else {
		registry.ctx().emplace<ConfigReloadHook>(ConfigReloadHook{
			.poll = [r = std::move(reloader)] { r->poll(); }});
	}
}

/**
 * wirePicking 定义（契约见 main.cppm 接口注释）：门关早退；门开 = 构造服务 +
 * 挂 Renderer + 双向函数桥（ecs 零依赖 picking 模块，同 ConfigReloadHook 闭包
 * 模式）。requestPick 只在左键按下产请求（右键 = 相机拖拽）；takeResult = 服务侧
 * 一次性消费。
 */
std::unique_ptr<PickingService> wirePicking(entt::registry& registry, Resources& resources,
	RenderTargetCache& rts, Renderer& renderer) {
	if (!diagnostics().picking)
		return nullptr;
	auto service = std::make_unique<PickingService>(resources, rts,
		renderer.getRenderGraphRef());
	renderer.setPickingService(service.get());
	PickingHook hook;
	hook.requestPick = [svc = service.get()](float x, float y) {
		svc->request(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
	};
	hook.takeResult = [svc = service.get()] { return svc->take(); };
	registry.ctx().emplace<PickingHook>(std::move(hook));
	return service;
}

}  // namespace RGL
