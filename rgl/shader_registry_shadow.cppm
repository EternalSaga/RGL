module;
#include <fmt/format.h>  // 条目构造失败消息（照 :impl 的 GMF fmt 先例）
/**
 * shader_registry:shadow —— ★18-B1 阴影段簇路径条目 + ★M-1b 真 MDI 变体条目（契约见
 * 接口 shader_registry.cppm 对应声明；实现自 :impl 拆出 = 文件代码行红线 500，
 * 先例 = :fullscreen。两条 add 刻意保持两份代码（不抽公共体，理由 → 函数头注释）。
 */
module shader_registry:shadow;

import shader_registry;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vk_device;
import vk_resources;
import shader;
import asset_table;

namespace RGL {

	// 簇化轴 ★18-B1：阴影段簇路径条目（契约见接口注释）。全场景 1 条目 = 簇 buffer 是全局
	// 单例（§10.2）⟹ addShadowEntry 的 vertexSlot 去重键在这里没有对应物；幂等靠条目名
	// （同 addSkyEntry / addLightEntry 的 `pipelineIndex("...")` 写法），装配层逐 prim 调它。
	uint32_t ShaderRegistry::addShadowMeshletEntry(std::array<uint32_t, 4> clusterSlots) {
		if (auto idx = pipelineIndex("shadow_meshlet"))
			return *idx;
		const RGL::PipelineSpec* spec = findSpec("shadow_meshlet");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: shadow_meshlet");
		RGL::PipelineBindings b{};
		b.descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		// binding 0/1 = 合法占位槽号（本管线 mesh/frag 两 stage 都不声明它们：mesh 不 import model、
		// frag 是 depth-only 空壳）⟹ 批 M 过滤不烘（批 M 前是「烘了空转无害」，现在根本不烘），
		// 方向与 addSkyEntry/addLightEntry 的 0/1 占位同（「声明未映射」才是 VVL 错误，
		// 过滤只敢滤没声明的）。取 0/1 两个真实存在的低段槽号。
		b.slotVertex = 0;
		b.slotIndex = 1;
		sceneData.ensure();  // 幂等创建 scene data buffer + 各子范围槽（批 N+1 起无返回槽号）
		fillSceneRegionSlots(b);  // scene data 子范围载体（binding 表以该函数为准，别在此抄）
		//（恒带上，SPV 未声明的那条由批 M 过滤自动裁掉——C++ 侧不登记谁用）
		b.hasSceneData = true;
		// textured = false → 不传 binding 1 的 baseColor 形态；不传 env、不传 6/7/8/12/13/15/16
		// ——两个 shader 文件均未声明它们（批 M 后即使传了也不会烘，但「不传」让槽号来源
		// 不参与账目，同 addShadowEntry 的取舍；🔴 若将来 shader 声明了其中之一，这里必须
		// 补传——「声明了未 mapping」仍是 VVL 错误）。
		// 簇 buffer 固定 binding（§10.2，与 addMeshletEntry 的 kClusterBindings 前 4 项同源）：
		// 17=顶点池 18=簇表 19=Tris 20=Remap。🔴 **不含 21**（逐簇可见标志）——本管线不声明
		// 该 binding，条目侧也不烘（两侧同向，理由 → 接口注释）。
		static constexpr std::array<uint32_t, 4> kClusterBindings{17u, 18u, 19u, 20u};
		for (size_t i = 0; i < kClusterBindings.size(); ++i)
			b.extra.push_back({.binding = kClusterBindings[i], .sampler = false,
				.heapSlot = clusterSlots[i]});
		auto entry = PipelineEntry{.name = "shadow_meshlet",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				"shadow_meshlet"),
			// ShadowMeshletDrawArgs = objectIndex + cascadeIndex + clusterStart（12B，scalar
			// layout；frame_draw_list.cppm 的 static_assert 与本 shader 的 struct 同源）
			.argsSize = 12,
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: shadow_meshlet (cluster slots {}/{}/{}/{}，binding "
				"17-20；无 binding 21) → 下标 {}",
				clusterSlots[0], clusterSlots[1], clusterSlots[2], clusterSlots[3], index);
		return index;
	}

	void ShaderRegistry::bindShadowMeshletToAsset(uint32_t assetId, uint32_t entryIdx) {
		assets.bindShadowMeshletPipeline(assetId, entryIdx);
	}

	// ★M-1b（docs/mdi_plan.md §19.2 D2 / §19.5）：真 MDI 阴影条目（契约见接口注释）。
	// 与 addShadowMeshletEntry **刻意保持两份代码**（不抽公共体）：两者的差异只有一枚
	// binding 与一份 mesh SPV，但「条目级 SPV 变体位」是会被后续批继续分叉的地方
	// （债-105 的「凡条目级的位都要问宿主被谁共用」），两份并排比一份带 bool 参数更难漏烘。
	// 只应差别两处的行已用注释钉住（mesh SPV 走 spec、extra 多一枚 22）。
	uint32_t ShaderRegistry::addShadowMeshletMdiEntry(std::array<uint32_t, 4> clusterSlots,
		uint32_t argsSlot) {
		if (auto idx = pipelineIndex("shadow_meshlet_mdi"))
			return *idx;
		const RGL::PipelineSpec* spec = findSpec("shadow_meshlet_mdi");
		if (!spec)
			throw std::runtime_error("pipeline spec not found: shadow_meshlet_mdi");
		RGL::PipelineBindings b{};
		b.descriptorSize = res.getDescriptorHeap().getDescriptorSize();
		// binding 0/1 占位 = 旧条目逐字同（两 stage 不声明 ⟹ 批 M 过滤不烘；方向判据 →
		// addShadowMeshletEntry 注释：「声明了未 mapping」才是 VVL 错误）。
		b.slotVertex = 0;
		b.slotIndex = 1;
		sceneData.ensure();  // 幂等创建 scene data buffer + 各子范围槽（批 N+1 起无返回槽号）
		fillSceneRegionSlots(b);  // scene data 子范围载体（binding 表以该函数为准，别在此抄）
		//（恒带上，SPV 未声明的那条由批 M 过滤自动裁掉——C++ 侧不登记谁用）
		b.hasSceneData = true;
		// 簇 buffer 固定 binding（§10.2，与旧条目 kClusterBindings 前 4 项同源；🔴 不含 21）
		static constexpr std::array<uint32_t, 4> kClusterBindings{17u, 18u, 19u, 20u};
		for (size_t i = 0; i < kClusterBindings.size(); ++i)
			b.extra.push_back({.binding = kClusterBindings[i], .sampler = false,
				.heapSlot = clusterSlots[i]});
		// 🔴 binding 34 = 行表（MDI 变体 SPV 声明了它，两侧同向，见接口注释；定案号 22 被
	// scene.slang 的 sceneHeader UBO 占用——本文件 import scene，重复声明 = mapping 冲突 =
	// vkCreateShadersEXT 报错，实测首跑现形 ⟹ 落全树第一个空闲号）。描述符内容 =
		// MeshletShadowArgsService 写入的 storage buffer 描述符（整张 ring×2 容量，轮转落在
		// argsOffset/rowBase 这两个**数**上，描述符与条目启动期烘一次不动）。
		b.extra.push_back({.binding = 34, .sampler = false, .heapSlot = argsSlot});
		auto entry = PipelineEntry{.name = "shadow_meshlet_mdi",
			.pipeline = RGL::RenderPipeline(device.getDevice(), loadShaderEffect(*spec), b,
				"shadow_meshlet_mdi"),
			// ShadowMeshletDrawArgs = rowBase + cascadeIndex + reserved（12B，scalar layout；
			// word0 语义切换见 shadow_meshlet.mesh.slang ★M-1b 注释。🔴 尺寸与旧条目同源，
			// static_assert 在 frame_draw_list.cppm——argsSize 不动 = 执行器通用 push 路径不动）
			.argsSize = 12,
			.spec = *spec, .bindings = b};
		const uint32_t index = static_cast<uint32_t>(entries.size());
		indexByName[entry.name] = index;
		entries.push_back(std::move(entry));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("pipeline entry: shadow_meshlet_mdi (cluster slots {}/{}/{}/{}，binding "
				"17-20 + 22=行表槽 {}；无 21) → 下标 {}",
				clusterSlots[0], clusterSlots[1], clusterSlots[2], clusterSlots[3], argsSlot, index);
		return index;
	}

	void ShaderRegistry::bindShadowMeshletMdiToAsset(uint32_t assetId, uint32_t entryIdx) {
		assets.bindShadowMeshletMdiPipeline(assetId, entryIdx);
	}
}  // namespace RGL
