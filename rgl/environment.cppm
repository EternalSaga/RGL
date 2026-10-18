module;
#include <glm/glm.hpp>  // irradianceSH（std::array<glm::vec4, 9>）
export module environment;

import rlstd;
import rlvk;
import vkerror;
import vk_device;
import vk_resources;  // 架构债务续账步骤 3：env/IBL 场景级内容资产从内核拆出——服务向内核引 heap，单向
import sampler_library;  // ensureEnvPrefilterReady 复用共享 LINEAR sampler（架构债务续账步骤 4a 产物）
import env_prefilter;  // Phase 8 ④b：GGX 预滤波环境 cubemap（heap + shader object，mip_gen 变体）
import asset_table;  // TextureAsset（env 单例入库）

namespace RGL {

/**
 * 环境/IBL 服务（2026-09-19 架构债务续账步骤 3：原 Resources 的 envAsset 单例 /
 * envIrradianceSH / sky sampler / EnvPrefilter 实例 + ensureEnvPrefilterReady 整体搬出
 * ——env 是**场景级内容资产**，sky sampler 的 maxLod 烘焙是 env 上传期知识，内核不该
 * 认识）。生命周期：main 栈对象，构造于 SamplerLibrary 之后、env 上传（B 阶段）与
 * 管线烘焙（binding 4/5）之前。槽号/写入时序与原 Resources 实现逐字节一致。
 */
export class Environment {
   public:
	/**
	 * @param kernel 资源内核（resource/sampler heap + allocSlot 领槽）
	 * @param device 设备层（sampler/描述符写入、EnvPrefilter 管线创建）
	 * @param samplers 共享 sampler 库（prefilter init 复用其 LINEAR sampler）
	 */
	Environment(RGL::Resources& kernel, RGL::DeviceContext& device, SamplerLibrary& samplers)
		: kernel(kernel), device(device), samplers(samplers), envPrefilter(device) {}

	/**
	 * Phase 5.5 环境 cubemap（场景级单例资产，texture_pool::uploadCubeMap 产出入库）：
	 * 与材质纹理集（AssetTable::textures）分离——env 绑定场景而非材质，不占纹理 id 空间
	 *（白纹理 id 0 / 首槽契约不受影响）。
	 * @param asset env 纹理资产（移动入；其 heapSlot / mipLevels 供 addModelEntry /
	 *   addLightEntry 烘焙 binding 4/5）
	 * @return 入库的 env 资产引用（optional 内部存储地址稳定——Environment = main 栈
	 *   对象永不 move，描述符指针链自引用在 emplace 之后设置即稳定）
	 */
	[[nodiscard]] TextureAsset& emplace(TextureAsset asset) {
		envAsset.emplace(std::move(asset));
		return *envAsset;
	}
	/** @return env cubemap 资产指针（nullptr = 未上传环境）；addModelEntry /
	 * addLightEntry 据此烘焙 binding 4/5（env 槽全局单例，不进组合去重 key）。 */
	[[nodiscard]] const TextureAsset* get() const {
		return envAsset ? &*envAsset : nullptr;
	}
	/** Phase 8 批次 ④a：环境漫反射辐照度 radiance SH9（9 × vec4，.xyz=系数、.w 留白；CPU
	 * env 加载期投影，renderListSystem 每帧拷进 SceneFrameData.irradianceSH → scene data 尾区
	 * → scene.slang irradiance(N) 做辐照度卷积消费）。 */
	void setIrradianceSH(const std::array<glm::vec4, 9>& sh) { envIrradianceSH = sh; }
	/** @return 环境漫反射辐照度 radiance SH9（未设 = 全 0 = 无环境漫反射）。 */
	[[nodiscard]] const std::array<glm::vec4, 9>& getIrradianceSH() const { return envIrradianceSH; }

	/**
	 * Phase 5.5 sky/env sampler：linear mipmap（mipmapMode=LINEAR + maxLod=mipLevels−1），
	 * 槽号经台账申报（归属者 "sampler.skyEnv"）。
	 *
	 * ⚠ 本 sampler 的 **maxLod = mipLevels−1 是 GGX 预滤波 mip 链唯一的入口**。2026-10-04
	 * 之前它与 AO 的 point sampler 各自算出同一个 `reservedSlots + 1`，被后写的 maxLod=0
	 * 覆写 ⟹ 所有 roughness 都在采样 mip0，`env.mipRange` 扫 0..20 画面逐字节不变。
	 * 护栏 = `Resources::claimSamplerSlot`（不同归属者结构上不可能同槽）。
	 * 幂等（同 owner 重复领返回同一槽）。不动共享 sampler——模型纹理路径零影响。
	 * maxLod 烘焙进描述符 → 调用点须已知环境贴图 mip 级数（env 上传期）。
	 * @param mipLevels 环境贴图 mip 级数（maxLod = mipLevels−1）
	 * @return sky sampler 在 sampler heap 的槽号（台账发放）
	 */
	[[nodiscard]] uint32_t ensureSkySampler(uint32_t mipLevels) {
		if (skySamplerReady)
			return skySamplerSlot;
		const auto& samplerHeap = kernel.getSamplerHeap();
		vk::SamplerCreateInfo info{};
		info.magFilter = vk::Filter::eLinear;
		info.minFilter = vk::Filter::eLinear;
		info.mipmapMode = vk::SamplerMipmapMode::eLinear;
		info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		info.anisotropyEnable = false;  // Bool32 隐转（rlvk 不导出 vk::False 常量）
		info.compareEnable = false;  // Bool32 隐转（rlvk 不导出 vk::False 常量）
		info.minLod = 0.0f;
		info.maxLod = static_cast<float>(mipLevels - 1);
		info.unnormalizedCoordinates = false;  // Bool32 隐转（rlvk 不导出 vk::False 常量）
		info.borderColor = vk::BorderColor::eIntTransparentBlack;
		auto slot = kernel.claimSamplerSlot("sampler.skyEnv");
		if (!slot)
			throw std::runtime_error(slot.error().describe());
		skySamplerSlot = *slot;
		auto slotAddr = samplerHeap.getSlotCpuAddress(skySamplerSlot);
		if (!slotAddr)
			throw std::runtime_error(slotAddr.error().describe());
		vk::HostAddressRangeEXT dst{*slotAddr, samplerHeap.getDescriptorSize()};
		skySamplerCreateInfo = info;
		device.getDevice().writeSamplerDescriptorsEXT(skySamplerCreateInfo, dst);
		skySamplerReady = true;
		return skySamplerSlot;
	}

	/** @return GGX 预滤波生成器（Phase 8 ④b；启动期一次性基础设施，构造于本对象构造期）。
	 * cubemap 16F 上传路径经它把 env mip 链（level ≥1）录进该 cube 的上传 CB：GGX
	 * importance-sample 预滤波（descriptor heap + shader object，须先经 ensurePrefilterReady
	 * 建齐 level shader/槽，见 env_prefilter 模块头注释）。 */
	[[nodiscard]] RGL::EnvPrefilter& getPrefilter() { return envPrefilter; }

	/**
	 * 备齐 GGX 预滤波的 heap 基础设施（env 16F cube 上传前调用，幂等增量）。职责：首次注入
	 * heap 引用 + 复用 SamplerLibrary::ensureShared 的 LINEAR sampler（init）；若已建 level
	 * shader 数 < levels，逐个 allocSlot 领取 src/dst 描述符槽 + EnvPrefilter 建对应 level 的
	 * shader object（每 level 一 shader object + 一对独立固定槽）。env 单例启动期只上传一次，
	 * 实际只补一轮。
	 * **段契约**：同 ensureMipGenReady——src/dst 走运行时段，不再推进几何段或纹理段游标。
	 * @param levels env 总 mip 级数（= cube mipLevels；只增不减）
	 * @return void；Error = allocSlot 越界 或 shader object 创建失败
	 */
	[[nodiscard]] std::expected<void, Error> ensurePrefilterReady(uint32_t levels) {
		if (levels <= 1)
			return {};  // 单级图无需预滤波
		if (!envPrefilterBound) {
			const uint32_t samplerSlot = samplers.ensureShared();  // 幂等：LINEAR/CLAMP，共用一份
			envPrefilter.init(kernel.getDescriptorHeap(), kernel.getSamplerHeap(), samplerSlot);
			envPrefilterBound = true;
		}
		while (envPrefilter.levelCount() < levels) {
			auto srcSlot = kernel.allocSlot(RGL::SlotSegment::Runtime, "envpf.src");
			if (!srcSlot)
				return std::unexpected(std::move(srcSlot).error());
			auto dstSlot = kernel.allocSlot(RGL::SlotSegment::Runtime, "envpf.dst");
			if (!dstSlot)
				return std::unexpected(std::move(dstSlot).error());
			if (auto added = envPrefilter.addLevel(*srcSlot, *dstSlot); !added)
				return std::unexpected(std::move(added).error());
		}
		return {};
	}

   private:
	RGL::Resources& kernel;  // 资源内核引用（heap / allocSlot）
	RGL::DeviceContext& device;  // 设备层引用（sampler 描述符写入）
	SamplerLibrary& samplers;  // 共享 sampler 库引用（prefilter init 共用）
	RGL::EnvPrefilter envPrefilter;  // Phase 8 ④b：GGX 预滤波 env cubemap 生成器（heap + shader object，启动期一次性）
	bool envPrefilterBound = false;  // env-prefilter heap 引用已注入标记（ensurePrefilterReady 首建守卫；幂等）
	// Phase 5.5：sky/env sampler（台账申报槽，ensureSkySampler 幂等写入）+ 环境 cubemap 单例
	vk::SamplerCreateInfo skySamplerCreateInfo{};  // create info 稳定存储（同 SamplerLibrary 规则）
	uint32_t skySamplerSlot = 0;  // sky sampler 槽号（Resources::claimSamplerSlot 发放）
	bool skySamplerReady = false;  // sky sampler 就绪标记（ensureSkySampler 幂等守卫）
	std::optional<TextureAsset> envAsset;  // env cubemap 单例（nullopt = 未上传环境；场景级资产，见 emplace 注释）
	std::array<glm::vec4, 9> envIrradianceSH{};  // Phase 8 ④a：环境漫反射 radiance SH9（env 加载期算，见 setIrradianceSH）
};

}  // namespace RGL
