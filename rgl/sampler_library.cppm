export module sampler_library;

import rlstd;
import rlvk;
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
 * 管线烘焙之前。sampler heap 首槽契约（slot = reservedSlots）与原实现逐字节一致。
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
	 * 一份 sampler 描述符（linear + mipmap LINEAR + **REPEAT**，maxLod=20）写
	 * sampler heap 首个可用槽，全模型管线映射指向同一槽。wrap 定案 REPEAT（2026-09-13，
	 * 见调试历史）：bistro 49.5% primitive 平铺 UV 越界 [0,1]（最大 V 跨度 ~77000），
	 * CLAMP 把越界钳成边缘纹素拉平 = 大片纯色块；glb 405 sampler 全空 = glTF 默认
	 * REPEAT，工程单 sampler 与之对齐。Phase 8 ③b 起 2D 纹理带
	 * compute mip 链（mip_gen）——采样自动用 mip（maxLod 20 覆盖 ≤2^20 纹理链长，
	 * Vulkan 按各图实际 mip 范围钳制，单 sampler 通吃不同尺寸）。幂等（首次调用写入，
	 * 之后返回既有槽号）；调用点 = 创建 textured 管线前（main 启动期）。
	 * sampler create info 存成员（稳定存储）：本机 RADV 写时即解析
	 *（docs/archive/heap_texture_research.md §12.2，"采样期解引用"旧归因已证伪），
	 * 但 spec 允许驱动延迟解释，生命周期须覆盖运行期。
	 * @return 共享 sampler 在 sampler heap 的槽号（首个可用槽 = reservedSlots）
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
		info.anisotropyEnable = false;  // Bool32 隐转（rlvk 不导出 vk::False 常量）
		info.compareEnable = false;  // Bool32 隐转（rlvk 不导出 vk::False 常量）
		info.minLod = 0.0f;
		info.maxLod = 20.0f;
		info.unnormalizedCoordinates = false;  // Bool32 隐转（rlvk 不导出 vk::False 常量）
		info.borderColor = vk::BorderColor::eIntTransparentBlack;
		sharedSamplerSlot = samplerHeap.getReservedSlots();
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
};

}  // namespace RGL
