module;
#include <fmt/format.h>
export module queue_family_manager;

import rlstd;
import rlvk;
import rllogger;
import vkerror;

namespace RGL {

/**
 * 队列 family 选择与分配（vkdevice 层）：**只申请一条图形队列**，产出
 * vk::DeviceQueueCreateInfo（priorities 为类成员，须比 vkCreateDevice 长寿）。
 *
 * ## 为什么只有一个角色（2026-10-14 定案 → `docs/compute_调研.md` §3 / §6 D3）
 * 旧版按 Graphics / Compute / Transfer 三个角色各选一条队列，但**任何有图形队列的桌面 / APU
 * 上 family 0 必然带 COMPUTE** ⟹ Compute 的"首个带 eCompute 的族"恒命中 family 0，Transfer 的
 * "专用族"判据又要求不含 graphics/compute 而恒不命中 ⟹ 三角色全被回落钳到**同一条物理队列**
 *（本机 family 0 `queueCount = 1`，三角色坍缩成 `{0, 0}`）。那两个多出来的句柄因此是"从没被
 * 读过的死状态"，而注释「保留给未来使用」会误导读代码的人以为"有独立队列，只是没用"。
 *
 * 🔴 **独立 compute 队列不是设备保证，是运行期查询结果**（很多设备的族是全功能的），所以它
 * 不得建模成常驻成员。将来真要第二条队列时的形态三条，缺一不做：
 * ① 能力查询返回 `optional` + **查不到就打日志出声**（"本机无独立 compute 族"），禁常驻成员；
 * ② 提交面分叉的两个必要条件——查询真的拿到独立族 **且** 图派生的"本帧无消费者"集合非空
 *（后者 = `docs/compute_调研.md` §7 批次 B2 的产物）；
 * ③ 跨界资源的 queue-family 所有权转移要一并补（全仓现为 0 处，barrier 里恒 `QUEUE_FAMILY_IGNORED`）。
 * 且注意 **独立族存在 ≠ 独立执行资源存在**（同文 §9 N4）：收益只取决于后者。
 *
 * 选择逻辑（构造期）：取首个带 `eGraphics` 的 family，申请其 index 0 一条队列。
 */
export class QueueFamilyManager {
   public:
	/** 物理设备上找不到带 eGraphics 的 family 时的哨兵值。 */
	static constexpr uint32_t kNoFamily = 0xFFFFFFFFu;

	/**
	 * 工厂入口（成功语义封装）：构造 QueueFamilyManager 并包进 expected。
	 * @param physicalDevice 物理设备（读 queue family 属性）
	 * @return 构造好的管理器；无图形 family 时返回 QueueNotAllocated
	 */
	static std::expected<QueueFamilyManager, Error>
	create(const vk::raii::PhysicalDevice& physicalDevice) {
		QueueFamilyManager mgr{physicalDevice};
		if (mgr.familyIndex == kNoFamily) {
			return std::unexpected(Error{ErrorCode::QueueNotAllocated,
				"No queue family with eGraphics on this physical device!"});
		}
		return mgr;
	}

	/**
	 * 构造：扫描 queue family 属性，取首个带 eGraphics 的 family 并申请其 index 0 一条队列，
	 * 产出 queueCreateInfos（一条，优先级 1.0f）。
	 * @param physicalDevice 物理设备（引用成员，须比本对象长寿）
	 */
	explicit QueueFamilyManager(const vk::raii::PhysicalDevice& physicalDevice)
		: physicalDevice(physicalDevice) {
		logger = RLLogger::getInstance();
		const auto props = physicalDevice.getQueueFamilyProperties();
		const auto firstGraphics = std::ranges::find_if(props,
			[](const vk::QueueFamilyProperties& p) {
				// 不写 `!= vk::QueueFlags{}`：rlvk 只导出 vulkan_enums.hpp（原始枚举），
				// vk::QueueFlags 这个 bitmask 别名名不可见（缺名该补 rlvk 的 export using，
				// 但这里用 Flags 的 operator bool 就够，不为省一个 cast 去扩导出面）
				return static_cast<bool>(p.queueFlags & vk::QueueFlagBits::eGraphics);
			});
		if (firstGraphics == props.end())
			return;  // familyIndex 留 kNoFamily，出声权在 create()

		familyIndex = static_cast<uint32_t>(std::ranges::distance(props.begin(), firstGraphics));
		// priorities 此后不再增长 ⟹ data() 指的那块堆内存在本对象存活期内稳定（vector 移动
		// 转移的是缓冲区指针，元素地址不变，故 create() 按值返回也不悬空）
		priorities.assign(1, 1.0f);
		queueCreateInfos.emplace_back(vk::DeviceQueueCreateFlags{}, familyIndex, 1, priorities.data());
	}

	/** vkCreateDevice 用的 queue create info 列表（priorities 由本对象持有）。 */
	const std::vector<vk::DeviceQueueCreateInfo>& getQueueCreateInfos() const {
		return queueCreateInfos;
	}

	/**
	 * 取本引擎唯一的逻辑队列句柄——图形队列（它同时带 compute / transfer 能力：桌面 / APU 的
	 * family 0 是全功能族，这也是"独立 compute 队列"在本类里不成立的原因，见类注释）。
	 * @param device 已创建的设备
	 * @return vk::Queue；无图形 family 时返回 QueueNotAllocated
	 */
	[[nodiscard]] std::expected<vk::Queue, Error> getQueue(const vk::Device& device) const {
		if (familyIndex == kNoFamily) {
			return std::unexpected(Error{ErrorCode::QueueNotAllocated,
				"Graphics queue family not allocated!"});
		}
		return device.getQueue(familyIndex, 0);
	}

	/**
	 * 图形队列的 family 下标（present 能力检查 / 命令池创建 / timestampValidBits 族判定用）。
	 * @return family 下标；未找到图形 family 时为 kNoFamily
	 */
	uint32_t getFamilyIndex() const { return familyIndex; }

	/**
	 * 检查 graphics family 能否向 surface present。
	 * @param surface 交换链 surface
	 * @return 空值 = 可 present；不可时打 critical 日志并返回 SurfaceNotPresentable
	 */
	[[nodiscard]] std::expected<void, Error> checkPresentAbility(const vk::raii::SurfaceKHR& surface) const {
		uint32_t graphicsFamilyIdx = getFamilyIndex();

		if (!physicalDevice.getSurfaceSupportKHR(graphicsFamilyIdx, *surface)) {
			const auto errMsg = fmt::format(
				"Graphics queue family (Index: {}) cannot present to surface",
				graphicsFamilyIdx);
			if (logger)
				logger->critical("{}", errMsg);
			return std::unexpected(Error{ErrorCode::SurfaceNotPresentable, errMsg});
		}
		return {};
	}

   private:
	std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
	std::vector<float> priorities;
	uint32_t familyIndex = kNoFamily;
	const vk::raii::PhysicalDevice& physicalDevice;
	RLLogger* logger;
};

}  // namespace RGL
