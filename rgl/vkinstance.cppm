module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
export module vk_instance;

import rlstd;
import rlvk;
import rllogger;
import diagnostics;  // no_vvl 诊断单例（configs/diagnostics.lua，main 启动期已 setDiagnostics）
import vulkan_utils;
import vulkan_window;
import vkerror;

namespace RGL {

/**
 * Vulkan 实例层（loader Context + 驱动实例 API 版本 + Instance + 验证层 + 调试消息器）；
 * main.cpp 初始化层构造顺序第 1 站（AppContext→DeviceContext→Swapchain→Resources→
 * ShaderRegistry→Renderer，见 main.cpp）。headless 扩展替换见构造函数。
 * **成员生命周期规则**（AGENTS.md"重要成员生命周期规则"）：engineInfo /
 * instanceCreateInfo / layers / instanceExtensions 必须为类成员，不可为构造体内局部
 * 变量——instanceCreateInfo 经 setPApplicationInfo 存 &engineInfo 指针、经
 * setPEnabledLayerNames/setPEnabledExtensionNames 存 vector.data() 指针（vulkan.hpp
 * ArrayProxyNoTemporaries，均不拷贝），且 instance 在成员初始化列表中直接由
 * instanceCreateInfo 构造（彼时构造体未执行，局部变量不可用）；声明序保证四者在
 * instance 构造前已就位。
 */
export class AppContext {
   public:
	/**
	 * 构建实例层。headless（VK_EXT_headless_surface）：脱离 SDL/compositor——SDL 窗口
	 * 可不建，instance 扩展用 VK_EXT_headless_surface 替代 SDL surface 扩展（surface 由
	 * createHeadlessSurfaceEXT 创建，见 vkswapchain.cppm）。headless=false 时行为不变。
	 * 构造序（成员初始化列表，按声明序）：headless → context（loader）→ apiVersion
	 * （驱动最大实例 API 版本）→ instance（buildInstanceCreateInfo）→
	 * debugMessenger（buildDebugMessenger）。
	 * @param window SDL 窗口；仅非 headless 时取 surface 扩展列表（headless 时 main 传
	 *   nullptr，本参数不被触碰）
	 * @param headless true = headless 模式（脱离 SDL/compositor）
	 * @note 失败抛 std::runtime_error（SDL surface 扩展获取失败 / 验证层缺失 / 调试消息器
	 *   创建失败）；raii Instance 创建失败抛 vk::SystemError（vulkan.hpp 默认路径）
	 */
	AppContext(RGL::SDLWindow* window, bool headless = false)
		: headless(headless), context(), apiVersion(context.enumerateInstanceVersion()), instance(context, buildInstanceCreateInfo(window, headless)), debugMessenger(buildDebugMessenger(instance)) {}

	/** @return raii Instance 引用（非 const / const 两重载，同一对象）；下游（DeviceContext 等）取实例入口。 */
	vk::raii::Instance& getInstance() { return instance; }
	const vk::raii::Instance& getInstance() const { return instance; }
	/** @return 驱动支持的最大实例 API 版本（vkEnumerateInstanceVersion；已写入 engineInfo.apiVersion = instance 创建版本）。 */
	uint32_t getApiVersion() const { return apiVersion; }
	/** @return true = headless 模式（脱离 SDL/compositor）。 */
	bool isHeadless() const { return headless; }

   private:
	bool headless;  // 模式标志（见构造函数）；headless = 不建 SDL 窗口、surface 走 headless 扩展

	/**
	 * 填 engineInfo（apiVersion = 驱动最大 / engineName "practice"）并组装 InstanceCreateInfo：
	 * 基础扩展 4 项（debug_utils / surface_maintenance1 / get_surface_capabilities2 /
	 * swapchain_color_space；后者被其他已启用扩展隐式依赖，不可随意删，见 AGENTS.md
	 * "隐式扩展依赖"）+（headless ? VK_KHR_surface + VK_EXT_headless_surface : SDL 窗口
	 * surface 扩展列表）+ 验证层可用性检查。
	 * @param window SDL 窗口（仅非 headless 用，取 surface 扩展列表）
	 * @param headless true = headless 模式
	 * @return 填好的 InstanceCreateInfo（类成员对象；内部指针指向 engineInfo / layers /
	 *   instanceExtensions，原因见类注释）
	 * @note 验证层缺失检查恒执行（no_vvl 开也抛——no_vvl 只跳过 setPEnabledLayerNames
	 *   启用，不跳过检查）；扩展向量必须先填完再 setPEnabled*（setter 存 data() 指针，
	 *   其后 vector 再扩容会使指针失效）
	 */
	vk::InstanceCreateInfo buildInstanceCreateInfo(RGL::SDLWindow* window, bool headless) {
		engineInfo.applicationVersion = vk::makeVersion(0, 1, 0);
		engineInfo.apiVersion = apiVersion;
		engineInfo.engineVersion = vk::makeVersion(0, 0, 1);
		engineInfo.pEngineName = "practice";

		instanceExtensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
			VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
			VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
			VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME};
		if (headless) {
			// headless：无 SDL surface 扩展，但 surface 族扩展（maintenance1/capabilities2/
			// colorspace/headless）都依赖 VK_KHR_surface，须显式启用
			instanceExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
			instanceExtensions.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
		} else {
			auto extensionsResult = window->getSurfaceExtensions();
			if (!extensionsResult) {
				std::string msg = extensionsResult.error().describe();
				throw std::runtime_error(msg);
			}
			auto [surfaceExtensions, surfaceCount] = *extensionsResult;
			std::ranges::copy_n(surfaceExtensions, surfaceCount,
				std::back_inserter(instanceExtensions));
		}

		const auto availableLayers = context.enumerateInstanceLayerProperties();
		if (!RGL::findMissingValidationLayers(availableLayers, layers).empty()) {
			throw std::runtime_error("Missing validation layers!");
		}

		instanceCreateInfo.setPApplicationInfo(&engineInfo);
		// DIAG（configs/diagnostics.lua no_vvl，原 RGL_NO_VVL env 2026-09-08 转配置）：
		// 关 VVL——VVL 的 descriptor heap wrapper 对同一 slot 双写（先零化后写实际描述符），
		// 排查黑屏/fault 时隔离嫌疑；默认 false = VVL 常开
		if (!diagnostics().noVvl)
			instanceCreateInfo.setPEnabledLayerNames(layers);
		instanceCreateInfo.setPEnabledExtensionNames(instanceExtensions);
		return instanceCreateInfo;
	}

	/**
	 * 创建调试消息器（经 RGL::setupDebugMessenger：Warning+Error ×
	 * General/Validation/Performance，回调接 RLLogger，见 utils.cppm）。
	 * @param inst raii Instance
	 * @return RAII DebugUtilsMessengerEXT（预期恒成功；raii 构造失败路径预留 Error）
	 * @note 失败抛 std::runtime_error（setupDebugMessenger 返回 unexpected 时）
	 */
	vk::raii::DebugUtilsMessengerEXT buildDebugMessenger(vk::raii::Instance& inst) {
		auto result = RGL::setupDebugMessenger(inst);
		if (!result) {
			throw std::runtime_error(result.error().describe());
		}
		return std::move(*result);
	}

	vk::raii::Context context;  // loader 入口（最先构造）；enumerateInstanceVersion 查驱动最大实例 API 版本
	uint32_t apiVersion;  // 驱动支持的最大实例 API 版本（vkEnumerateInstanceVersion）；写入 engineInfo.apiVersion
	vk::ApplicationInfo engineInfo;  // 必须类成员：instanceCreateInfo.pApplicationInfo = &engineInfo（指针，非拷贝）
	vk::InstanceCreateInfo instanceCreateInfo;  // 必须类成员：instance 在初始化列表直接由它构造（构造体未执行，不可用局部）；
	                                           //   持有指向 engineInfo / layers / instanceExtensions 的指针
	std::vector<const char*> layers = {"VK_LAYER_KHRONOS_validation"};  // 必须类成员：setPEnabledLayerNames 只存 vector.data()（不拷贝）；
	                                                                   //   唯一验证层，可用性恒检查（缺失抛错，no_vvl 也不例外）
	std::vector<const char*> instanceExtensions;  // 必须类成员：setPEnabledExtensionNames 只存 vector.data()（不拷贝）；
	                                              //   内容 = 基础 4 项 + headless/SDL surface 扩展（buildInstanceCreateInfo 填）
	vk::raii::Instance instance;  // raii Instance（初始化列表由 context + instanceCreateInfo 构造）
	vk::raii::DebugUtilsMessengerEXT debugMessenger;  // 调试消息器（instance 之后构造；回调接 RLLogger，见 utils.cppm）
};

}  // namespace RGL