module;
#include <glm/glm.hpp>
export module evssm_shadow;

import rlstd;
import rlvk;
import vk_device;
import vk_resources;
import VmaRaii;
import offgraph_sync;  // 批次 B1b：图外同步发射账（barrier 由边表物化，本模块不写 mask）

namespace RGL {

/** EVSSM 矩链级数（含 mip0；= 探针口径 11.6c。消费端 maxMipLevel = 本值−1，
 *  scene data shadowEvssm.w 由 render_list 从本常量导出——禁双源写死 7）。 */
export inline constexpr uint32_t kEvssmMipLevels = 8;

/**
 * EVSSM 软阴影的矩图数据层（批 11.6d；docs/evssm_plan.md §4.1 C 方案）。
 *
 * 图外 compute（IzbShadow 同族编舞，render_graph 无 compute kind）：
 * 阴影图集 D32（PCF 同槽只读）→ RGBA16F 矩图 mip0（Filament 矩组
 * z=exp(c·(2d−1))）→ 7 级 per-tile box 降采样链（免 sampler2DArray 的对齐论证
 * = 11.6b）。shadow pass / RT 缓存 / graph **零改动**。
 *
 * 生命周期 = Renderer 在 shadow.lua 的 mode="evssm" 时构造；
 * **门关 = 本对象根本不被构造**（逐位旁路红线，同 izb_shadow）。
 *
 * 布局策略：矩图全程 GENERAL（探针 /tmp/opencode/evssm_probe 已证 RADV 可行），
 * 帧内零 layout 转移；跨阶段依赖用**全局 stage/access barrier**（无需 image 句柄
 * ——阴影图归属 RT 缓存，本模块只有其 heap 槽）。
 *
 * 描述符铁纪律（2026-10-01 用户拍板）：全部 Resources descriptor heap 用户槽
 * （Runtime 段），老式 DescriptorPool 禁入（docs/DESCRIPTOR_HEAP_SPEC.md）。
 */
export class EvssmShadow {
   public:
	/**
	 * @param vkDevice   设备层
	 * @param resources  descriptor heap（9 槽领取 + 描述符写入）
	 * @param mapSize    阴影图集整幅边长（2048，= kShadowMapSize）
	 * @param cols       级联分块列数（shadowAtlasColumns）
	 * @param mipLevels  矩链级数（8 = 探针口径；含 mip0）
	 * @param exponent   EVSM 指数 c（默认 = kEvssmExponent 唯一权威源；消费端经
	 *        scene data 常量块同源，勿传其他值）
	 * @param shadowMapSlot 阴影图集采样 heap 槽（ensureShadowMapSlot 结果，只读复用）
	 * @param momentsSampleSlot 矩图 sampled 槽（Resources::ensureEvssmSampleSlot 发号，
	 *        binding 15 已 bake 此槽号——本构造以真 view **覆写**同槽描述符，同 IzbShadow
	 *        mask 槽模式；重建换图重写同槽）
	 * @param sync 图外同步发射账（批次 B1b：barrier 由边表物化，本模块不写 mask 字面量）。
	 *        构造期用它申报 `evssm_moments_chain` 的应发次数 = `mipLevels − 1`——**必须在构造期**，
	 *        录制期申报等于自己出题自己答，那条判据就自证了。
	 */
	EvssmShadow(RGL::DeviceContext& vkDevice, RGL::Resources& resources,
		uint32_t mapSize, uint32_t cols, uint32_t mipLevels, float exponent,
		uint32_t shadowMapSlot, uint32_t momentsSampleSlot, OffGraphSync& sync);

	/**
	 * 图外 sandwich（录在 light pass 迭代顶部、izb sandwich 同款位置）：
	 * 边 `evssm_depth_to_compute`（graphics→compute，覆盖阴影图 depth-write→本 pass read）
	 * → moments dispatch → 逐级 downsample（级间边 `evssm_moments_chain`，compute→compute）
	 * → 收尾边 `evssm_moments_to_light`（compute→fragment，light pass 采样前）。
	 * 🔴 三条边的 stage/access **不在本模块写**——由 `offgraph_sync` 的 usage 词表物化，
	 * 本模块只负责"在哪发"（批次 B1b → `docs/compute_调研.md` §7.2）。
	 * @param cmd pass 循环所在命令缓冲（heap 已绑定）
	 */
	void recordFrame(vk::raii::CommandBuffer& cmd);

	/** 矩图采样槽（全 mip 链 view，GENERAL；= d-2 消费变体 binding 15 烘焙用）。 */
	[[nodiscard]] uint32_t getMomentsSlot() const { return sampleSlot; }
	/** 矩链级数（消费端 maxMipLevel 参数）。 */
	[[nodiscard]] uint32_t getMipLevels() const { return mipLevels; }

   private:
	struct PushImpl;  // 两个 push 布局在 impl unit（避免接口拖宏）
	/// 图外同步发射账（批次 B1b；Renderer 持有，生命周期覆盖本模块）
	OffGraphSync& sync;
	DeviceContext* deviceCtx = nullptr;
	Resources* resources = nullptr;
	uint32_t mapSize = 0;
	uint32_t cols = 0;
	uint32_t mipLevels = 0;
	float exponent = kEvssmExponent;  // vk_resources 唯一权威源（scene data 常量块同源下发）
	uint32_t shadowMapSlot = 0;
	uint32_t sampleSlot = 0;                 // 全 mip sampled（binding 15）
	std::vector<uint32_t> mipSlots;          // 每 mip storage 槽（size = mipLevels）
	// view create info 常驻（描述符 heap 语义 = pView 指 create info，随槽存活）
	std::vector<vk::ImageViewCreateInfo> mipViewCreates;
	std::optional<vk::ImageViewCreateInfo> sampleViewCreate;
	RGL::VmaImage momentsImage;
	std::optional<vk::raii::ShaderEXT> momentsShader;
	std::vector<vk::raii::ShaderEXT> downShaders;  // index = level−1（level 1..mipLevels−1）
};

}  // namespace RGL
