module;
#include <fmt/format.h>
export module queue_family_manager;

import rlstd;
import rlvk;
import rllogger;
import vkerror;

namespace RGL {

/**
 * 队列 family 角色（选择/分配的最小单位）：Graphics（图形）、Compute（计算）、
 * Transfer（传输）。三者可能落在同一个 family（核显常见）。
 */
export enum class QueueFamilyRole {
	Graphics,
	Compute,
	Transfer
};

/** 一个角色的队列分配结果：family 下标 + family 内物理队列下标。 */
struct QueueAllocation {
	uint32_t familyIndex;
	uint32_t baseQueueIndex;
};

/**
 * 队列 family 选择与分配（vkdevice 层）：按 Graphics/Compute/Transfer 三个角色
 * 各选一个 family + 一个物理队列下标，并产出 vk::DeviceQueueCreateInfo
 *（priorities 为类成员，须比 vkCreateDevice 长寿）。
 * 选择逻辑（构造期）：
 * - Graphics/Compute = 首个带对应 flag 的 family（按序取第一个）；
 * - Transfer 优先**专用** transfer family（仅带 eTransfer、不带 graphics/compute），
 *   无专用则回落任意带 eTransfer 的 family，再回落 graphics family；
 *   Compute 无匹配时同样回落 graphics family；
 * - 同一 family 被多角色共享时在 hardware queueCount 内分配不同队列下标，
 *   超出 queueCount 时钳到 queueCount-1（共享末队列）。
 */
export class QueueFamilyManager {
   public:
	/**
	 * 工厂入口（成功语义封装）：构造 QueueFamilyManager 并包进 expected。
	 * @param physicalDevice 物理设备（读 queue family 属性）
	 * @return 构造好的管理器（当前实现恒成功，失败路径预留）
	 */
	static std::expected<QueueFamilyManager, Error>
	create(const vk::raii::PhysicalDevice& physicalDevice) {
		QueueFamilyManager mgr{physicalDevice};
		return mgr;
	}

	/**
	 * 构造：扫描 queue family 属性，按类注释的选择逻辑填充 allocations 与
	 * queueCreateInfos（每 family 一条 create info，优先级全 1.0f）。
	 * @param physicalDevice 物理设备（引用成员，须比本对象长寿）
	 */
	QueueFamilyManager(const vk::raii::PhysicalDevice& physicalDevice)
		: physicalDevice(physicalDevice) {
		auto queueFamilies = physicalDevice.getQueueFamilyProperties();
		std::map<QueueFamilyRole, uint32_t> roleToFamilies;
		logger = RLLogger::getInstance();

		for (uint32_t i = 0; i < queueFamilies.size(); ++i) {
			const auto& props = queueFamilies[i];

			if (!roleToFamilies.contains(QueueFamilyRole::Graphics) &&
				(props.queueFlags & vk::QueueFlagBits::eGraphics)) {
				roleToFamilies[QueueFamilyRole::Graphics] = i;
			}
			if (!roleToFamilies.contains(QueueFamilyRole::Compute) &&
				(props.queueFlags & vk::QueueFlagBits::eCompute)) {
				roleToFamilies[QueueFamilyRole::Compute] = i;
			}
			// Prefer dedicated transfer queue
			if (!roleToFamilies.contains(QueueFamilyRole::Transfer) &&
				(props.queueFlags & vk::QueueFlagBits::eTransfer) &&
				!(props.queueFlags & vk::QueueFlagBits::eGraphics) &&
				!(props.queueFlags & vk::QueueFlagBits::eCompute)) {
				roleToFamilies[QueueFamilyRole::Transfer] = i;
			}
		}

		// Fallback: reuse existing family if dedicated not available
		if (!roleToFamilies.contains(QueueFamilyRole::Transfer)) {
			for (uint32_t i = 0; i < queueFamilies.size(); ++i) {
				if (queueFamilies[i].queueFlags & vk::QueueFlagBits::eTransfer) {
					roleToFamilies[QueueFamilyRole::Transfer] = i;
					break;
				}
			}
			if (!roleToFamilies.contains(QueueFamilyRole::Transfer)) {
				roleToFamilies[QueueFamilyRole::Transfer] = roleToFamilies[QueueFamilyRole::Graphics];
			}
		}
		if (!roleToFamilies.contains(QueueFamilyRole::Compute)) {
			roleToFamilies[QueueFamilyRole::Compute] = roleToFamilies[QueueFamilyRole::Graphics];
		}

		// Allocate physical queue indices within hardware queueCount limits
		std::map<uint32_t, uint32_t> familyAllocatedCount;
		std::array<QueueFamilyRole, 3> roleOrder = {
			QueueFamilyRole::Graphics, QueueFamilyRole::Compute, QueueFamilyRole::Transfer};

		for (QueueFamilyRole role : roleOrder) {
			uint32_t familyIdx = roleToFamilies[role];
			uint32_t maxAvailable = queueFamilies[familyIdx].queueCount;
			uint32_t currentAllocated = familyAllocatedCount[familyIdx];

			if (currentAllocated < maxAvailable) {
				allocations[role] = {familyIdx, currentAllocated};
				familyAllocatedCount[familyIdx]++;
			} else {
				allocations[role] = {familyIdx, maxAvailable - 1};
			}
		}

		// Build vk::DeviceQueueCreateInfo with dynamic priority arrays
		// priorities must outlive vkCreateDevice, so stored as class member
		priorities.clear();
		queueCreateInfos.clear();

		for (const auto& [familyIdx, count] : familyAllocatedCount) {
			size_t startIdx = priorities.size();
			for (uint32_t i = 0; i < count; ++i) {
				priorities.push_back(1.0f);
			}

			queueCreateInfos.emplace_back(
				vk::DeviceQueueCreateFlags{},
				familyIdx,
				count,
				&priorities[startIdx]);
		}
	}

	/** vkCreateDevice 用的 queue create info 列表（priorities 由本对象持有）。 */
	const std::vector<vk::DeviceQueueCreateInfo>& getQueueCreateInfos() const {
		return queueCreateInfos;
	}

	/**
	 * 按角色取逻辑队列句柄。
	 * @param device 已创建的设备
	 * @param role 队列角色
	 * @return 该角色的 vk::Queue；角色未分配时返回 QueueNotAllocated
	 */
	[[nodiscard]] std::expected<vk::Queue, Error>
	getQueue(const vk::Device& device, QueueFamilyRole role) const {
		auto it = allocations.find(role);
		if (it == allocations.end()) {
			return std::unexpected(Error{ErrorCode::QueueNotAllocated,
				"Requested queue role not allocated!"});
		}
		return device.getQueue(it->second.familyIndex, it->second.baseQueueIndex);
	}

	/**
	 * 角色对应的 queue family 下标。
	 * @param role 队列角色
	 * @return family 下标
	 * @note 角色未分配时 std::map::at 抛 std::out_of_range（调用点均保证已分配）
	 */
	uint32_t getFamilyIndex(QueueFamilyRole role) const {
		return allocations.at(role).familyIndex;
	}

	/**
	 * 检查 graphics family 能否向 surface present。
	 * @param surface 交换链 surface
	 * @return 空值 = 可 present；不可时打 critical 日志并返回 SurfaceNotPresentable
	 */
	[[nodiscard]] std::expected<void, Error> checkPresentAbility(const vk::raii::SurfaceKHR& surface) const {
		uint32_t graphicsFamilyIdx = getFamilyIndex(QueueFamilyRole::Graphics);

		if (!physicalDevice.getSurfaceSupportKHR(graphicsFamilyIdx, *surface)) {
			const auto errMsg = fmt::format(
				"Graphics queue family (Index: {}) cannot present to surface",
				graphicsFamilyIdx);
			if (logger)
				logger->critical(errMsg);
			return std::unexpected(Error{ErrorCode::SurfaceNotPresentable, errMsg});
		}
		return {};
	}

   private:
	std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
	std::vector<float> priorities;
	std::map<QueueFamilyRole, QueueAllocation> allocations;
	const vk::raii::PhysicalDevice& physicalDevice;
	RLLogger* logger;
};

}  // namespace RGL