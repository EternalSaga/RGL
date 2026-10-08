export module sampler_library;

import rlstd;
import rlvk;
import rllogger;  // 共享 sampler 各向异性实机参数打点（2026-10-03 AF 启用）
import vkerror;
import vk_device;
import vk_resources;  // 架构债务续账步骤 4a：sampler 内容策略从内核拆出——服务向内核引 sampler heap，单向
import mip_gen;  // mip-gen 供给（ensureMipGenReady 领槽 + MipGen 实例宿主，架构债务续账步骤 4b）

namespace RGL {

/**
 * 共享 sampler 库 + mip-gen 供给（2026-09-19 架构债务续账步骤 4a/4b：原
 * Resources::ensureSharedSampler / ensureMipGenReady + MipGen 实例整体搬出——REPEAT wrap
 * 决策与 mip 链基建是**内容策略**，不是资源内核职责）。
 * 生命周期：main 栈对象，构造于 Resources 之后、一切纹理上传（uploadTexture 链）与
 * 管线烘焙之前。**槽号不再自算**（2026-10-04）：两个 sampler 均经
 * `Resources::claimSamplerSlot(归属者名)` 申报领槽，台账保证不同归属者永不拿到同一槽。
 */
export class SamplerLibrary {
   public:
	/**
	 * @param kernel 资源内核（sampler heap / descriptor heap / allocSlot 领槽）
	 * @param device 设备层（sampler 描述符写入）
	 */
	SamplerLibrary(RGL::Resources& kernel, RGL::DeviceContext& device)
		: kernel(kernel), device(device), mipGen(device) {}

	/**
	 * 共享 sampler（批次 1：owner 从 TexturePool 构造移入，架构债务续账步骤 4a 再移入本类）：
	 * 一份 sampler 描述符（linear + mipmap LINEAR + **REPEAT**，maxLod=20，设备支持时开
	 * **各向异性过滤**，上限 min(16, 物理 maxSamplerAnisotropy)）写 sampler heap 首个可用槽，
	 * 全模型管线映射指向同一槽。wrap 定案 REPEAT（2026-09-13，
	 * 见git:调试历史）：bistro 49.5% primitive 平铺 UV 越界 [0,1]（最大 V 跨度 ~77000），
	 * CLAMP 把越界钳成边缘纹素拉平 = 大片纯色块；glb 405 sampler 全空 = glTF 默认
	 * REPEAT，工程单 sampler 与之对齐。Phase 8 ③b 起 2D 纹理带
	 * compute mip 链（mip_gen）——采样自动用 mip（maxLod 20 覆盖 ≤2^20 纹理链长，
	 * Vulkan 按各图实际 mip 范围钳制，单 sampler 通吃不同尺寸）。幂等（首次调用写入，
	 * 之后返回既有槽号）；调用点 = 创建 textured 管线前（main 启动期）。
	 * 各向异性（2026-10-03 拍板启用）：bistro 地面/长廊是**掠射面**，无 AF 时远景纹理
	 * 质量与稳定性都差——三线性只沿各向同性单轴平均，沿各向异性椭圆陡轴分辨率尽失（糊），
	 * 且 mip 选择随相机微动整档跳变（闪）。开关跟随 core 可选特性探测
	 * （features_config.json optional_features，设备不支持 = 保持关，不得硬开——
	 * 启用未 enable 的特性是 UB）；maxAnisotropy 取 min(16, 物理上限)：驱动对超上限值
	 * 静默钳制（VU 00651 要求落 [1, maxSamplerAnisotropy]），16 是桌面常见上限，显式取
	 * min 不赌钳制。AF 只参与隐式 LOD 选择与过滤，mip_gen 的 SampleLevel 显式级采样
	 * 不受影响。
	 * sampler create info 存成员（稳定存储）：本机 RADV 写时即解析
	 *（docs/archive/heap_texture_research.md §12.2，"采样期解引用"旧归因已证伪），
	 * 但 spec 允许驱动延迟解释，生命周期须覆盖运行期。
	 * @return 共享 sampler 在 sampler heap 的槽号（台账首个发放项，现行启动顺序下 = reservedSlots）
	 */
	[[nodiscard]] uint32_t ensureShared() {
		if (sharedSamplerReady)
			return sharedSamplerSlot;
		const auto& samplerHeap = kernel.getSamplerHeap();
		vk::SamplerCreateInfo info{};
		info.magFilter = vk::Filter::eLinear;
		info.minFilter = vk::Filter::eLinear;
		info.mipmapMode = vk::SamplerMipmapMode::eLinear;
		info.addressModeU = vk::SamplerAddressMode::eRepeat;
		info.addressModeV = vk::SamplerAddressMode::eRepeat;
		info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		// 各向异性（理由见本函数头注释）：开关 = core 可选特性探测位（createDevice 已按
		// 同一探测位 enable），上限显式夹到物理 limits.maxSamplerAnisotropy 以内
		const bool anisotropy = device.getOptionalFeatures().samplerAnisotropy;
		const float deviceMaxAnisotropy
			= device.getPhysicalDevice().getProperties().limits.maxSamplerAnisotropy;
		info.anisotropyEnable = anisotropy;  // Bool32 隐转（rlvk 不导出 vk::False 常量）
		if (anisotropy)
			info.maxAnisotropy = std::min(16.0f, deviceMaxAnisotropy);
		if (auto logger = RLLogger::getInstance())
			logger->info("Shared sampler: anisotropyEnable={} maxAnisotropy={} (device maxSamplerAnisotropy={})",
				anisotropy, info.maxAnisotropy, deviceMaxAnisotropy);
		info.compareEnable = false;  // Bool32 隐转（rlvk 不导出 vk::False 常量）
		info.minLod = 0.0f;
		info.maxLod = 20.0f;
		info.unnormalizedCoordinates = false;  // Bool32 隐转（rlvk 不导出 vk::False 常量）
		info.borderColor = vk::BorderColor::eIntTransparentBlack;
		// 槽号由台账发（不再自算 reservedSlots）——见 Resources::claimSamplerSlot 头注释：
		// 自算形态下本类与 Environment 曾各自算出 reservedSlots+1 并相撞，幂等守卫让后写者
		// 永久赢，env 镜面项因此丢了 mip 选择。台账按归属者发号，相撞在结构上不可能。
		auto slot = kernel.claimSamplerSlot("sampler.shared");
		if (!slot)
			throw std::runtime_error(slot.error().describe());
		sharedSamplerSlot = *slot;
		auto slotAddr = samplerHeap.getSlotCpuAddress(sharedSamplerSlot);
		if (!slotAddr)
			throw std::runtime_error(slotAddr.error().describe());
		vk::HostAddressRangeEXT dst{*slotAddr, samplerHeap.getDescriptorSize()};
		samplerCreateInfo = info;
		// hpp raii 内部 resultCheck（失败抛 LogicError）
		device.getDevice().writeSamplerDescriptorsEXT(samplerCreateInfo, dst);
		sharedSamplerReady = true;
		return sharedSamplerSlot;
	}

	/**
	 * 屏幕空间 point sampler（Phase 11 批 11.4，AO 专用）：**NEAREST + 无 mipmap +
	 * CLAMP_TO_EDGE**，槽号经台账申报（归属者 "sampler.aoPoint"）。
	 *
	 * 为何不能复用 ensureShared 的共享 sampler：
	 *   ① 共享 sampler 是 LINEAR + mipmap LINEAR，而 **D32Sfloat 深度图在 Vulkan 里
	 *      不可过滤**（depth/stencil 格式无 filterable 能力）——用 linear sampler 采样
	 *      深度是 invalid usage，驱动行为未定义；
	 *   ② AO 必须 point 采样：深度是**逐像素可见性数据**，插值出来的深度没有物理意义
	 *      （会在物体轮廓上产生假的中间深度），而且 AO 采样核的抖动本身就要求硬取样；
	 *   ③ REPEAT wrap 对屏幕空间坐标同样有害（uv 越界会绕回另一侧采到无关像素）。
	 * AO 逐像素算好遮蔽量、无需滤波，故此 sampler 只被 ao.frag 消费。
	 * 幂等（首次调用写入，之后返回既有槽号）。
	 * @return AO point sampler 在 sampler heap 的槽号
	 */
	[[nodiscard]] uint32_t ensurePointSampler() {
		if (pointSamplerReady)
			return pointSamplerSlot;
		const auto& samplerHeap = kernel.getSamplerHeap();
		vk::SamplerCreateInfo info{};
		info.magFilter = vk::Filter::eNearest;
		info.minFilter = vk::Filter::eNearest;
		info.mipmapMode = vk::SamplerMipmapMode::eNearest;  // 单 mip 图，实际恒 level 0
		info.addressModeU = vk::SamplerAddressMode::eClampToEdge;  // 屏幕空间采样，边缘钳制
		info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		info.anisotropyEnable = false;  // Bool32 隐转（rlvk 不导出 vk::False 常量）
		info.compareEnable = false;  // Bool32 隐转（rlvk 不导出 vk::False 常量）
		info.minLod = 0.0f;
		info.maxLod = 0.0f;  // 硬钳 level 0：不给驱动任何 mip 选择的余地
		info.unnormalizedCoordinates = false;  // Bool32 隐转（rlvk 不导出 vk::False 常量）
		info.borderColor = vk::BorderColor::eIntTransparentBlack;
		// 槽号由台账发（归属者 "sampler.aoPoint"）。⚠ 本 sampler 的语义（NEAREST + maxLod=0）
		// 对单 mip 屏幕空间图是**正确**的，2026-10-04 那次事故的错处只在槽号——它曾与
		// Environment 的 sky sampler 算出同一个 reservedSlots+1，把 env 的 LINEAR + maxLod=10
		// 覆写掉。改的是发号，不是这里的参数，别顺手"统一"成 LINEAR。
		auto slot = kernel.claimSamplerSlot("sampler.aoPoint");
		if (!slot)
			throw std::runtime_error(slot.error().describe());
		pointSamplerSlot = *slot;
		auto slotAddr = samplerHeap.getSlotCpuAddress(pointSamplerSlot);
		if (!slotAddr)
			throw std::runtime_error(slotAddr.error().describe());
		vk::HostAddressRangeEXT dst{*slotAddr, samplerHeap.getDescriptorSize()};
		pointSamplerCreateInfo = info;
		device.getDevice().writeSamplerDescriptorsEXT(pointSamplerCreateInfo, dst);
		pointSamplerReady = true;
		return pointSamplerSlot;
	}

	/** @return mip 链生成器（Phase 8 ③b；启动期一次性基础设施，构造于本对象构造期）。
	 * uploadTexture 2D 路径经它把 mip 链（level ≥1）录进该纹理的上传 CB：UNORM =
	 * compute 链（descriptor heap + shader object，须先经 ensureMipGenReady 建齐 level
	 * shader/槽）/ sRGB = LINEAR blit 链（见 mip_gen 模块头注释：RADV SRGB 无
	 * STORAGE_IMAGE 能力）。 */
	[[nodiscard]] RGL::MipGen& getMipGen() { return mipGen; }

	/**
	 * 备齐 mip-gen 的 heap 基础设施（首张 UNORM compute 链纹理上传前调用，幂等增量）。
	 * 职责：首次注入 heap 引用 + 复用 ensureShared 的 LINEAR sampler（init）；
	 * 若已建 level shader 数 < levels，逐个 allocSlot 领取 src/dst 描述符槽 + MipGen
	 * 建对应 level 的 shader object（每 level 一 shader object + 一对独立固定槽，跨纹理
	 * fence 分隔复用）。
	 * **段契约**：src/dst 走运行时段，不再推进几何段或纹理段游标；纹理上传顺序不再受
	 * mip-gen 辅助槽位置影响。
	 * @param levels 需要覆盖的最大 mip level 数（= 当纹理 mipLevels；只增不减）
	 * @return void；Error = allocSlot 越界 或 shader object 创建失败
	 */
	[[nodiscard]] std::expected<void, Error> ensureMipGenReady(uint32_t levels) {
		if (levels <= 1)
			return {};  // 单级图无需 mip 链
		if (!mipGenBound) {
			const uint32_t samplerSlot = ensureShared();  // 幂等：LINEAR/CLAMP，mip-gen 共用
			mipGen.init(kernel.getDescriptorHeap(), kernel.getSamplerHeap(), samplerSlot);
			mipGenBound = true;
		}
		while (mipGen.levelCount() < levels) {
			auto srcSlot = kernel.allocSlot(RGL::SlotSegment::Runtime, "mipgen.src");
			if (!srcSlot)
				return std::unexpected(std::move(srcSlot).error());
			auto dstSlot = kernel.allocSlot(RGL::SlotSegment::Runtime, "mipgen.dst");
			if (!dstSlot)
				return std::unexpected(std::move(dstSlot).error());
			if (auto added = mipGen.addLevel(*srcSlot, *dstSlot); !added)
				return std::unexpected(std::move(added).error());
		}
		return {};
	}

   private:
	RGL::Resources& kernel;  // 资源内核引用（heap / allocSlot）
	RGL::DeviceContext& device;  // 设备层引用（writeSamplerDescriptorsEXT）
	RGL::MipGen mipGen;  // Phase 8 ③b：compute mip 链生成器（heap + shader object，启动期一次性）
	bool mipGenBound = false;  // mip-gen heap 引用已注入标记（ensureMipGenReady 首建守卫；幂等）
	// 共享 sampler 的 create info 稳定存储（ensureShared 写入；
	// 本机 RADV 写时即解析、spec 允许驱动延迟解释，生命周期须覆盖运行期，见其注释）
	vk::SamplerCreateInfo samplerCreateInfo{};
	uint32_t sharedSamplerSlot = 0;   // 共享 sampler 在 sampler heap 的槽号（ensureShared 启动期写入）
	bool sharedSamplerReady = false;  // 共享 sampler 就绪标记（ensureShared 幂等守卫）
	// AO point sampler（Phase 11 批 11.4）：槽号经台账申报（"sampler.aoPoint"），
	// create info 稳定存储（与共享 sampler 同生命周期理由）
	vk::SamplerCreateInfo pointSamplerCreateInfo{};
	uint32_t pointSamplerSlot = 0;
	bool pointSamplerReady = false;
};

}  // namespace RGL
