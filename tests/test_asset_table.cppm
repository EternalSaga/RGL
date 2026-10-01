module;
#include <doctest/doctest.h>

// AssetTable::expectedGeometrySlot 槽号算术契约（批 12.3d · B5）——被测 =
// rgl/asset_table.cppm:189-191（rlgfx 成员）：
//   return reservedSlots + HeapSlotGeometryBase + geometrySlotCount;
//
// ## 无设备构造的核实（任务红线：需设备就停）
// AssetTable 默认构造只创建 meshes/textures/materials 三个空容器——TextureAsset
// （含 VmaImage / vk::raii::ImageView，真设备对象）只在 emplaceTexture 被调用时才
// 实例化，本测试不调它。import 面（rlvk/VmaRaii）是**编译期**依赖 ≠ 运行期设备依赖
// （同 test_render_graph 头注释的批 12.2d 结论）。⇒ 可无设备测，未改生产代码形态。
//
// ## 契约来源（断言照源码注释写，不照实现回显）
//   · asset_table.cppm:182-188：期望槽号 = reserved + HeapSlotGeometryBase + 已占数；
//     上传方 allocSlot(Geometry) 后比对，违例 = 几何段中间插了别的分配。
//   · asset_table.cppm:174-176：计数单位 = **槽**（非模型、非 primitive 数）；
//     uploadModelMeshes 每块 +1、uploadBuiltinCube +2 → 计数由上传方显式维护，
//     addMesh/addMaterial 注册资产本身**不**推进期望槽。
//   · reservedSlots + HeapSlotGeometryBase = 几何段起点（用户槽第一段），纹理段首槽
//     （白纹理）= reserved + 几何段容量——由 vk_resources 侧断言，本表只保证几何
//     计数与纹理记录（textureSize）互不耦合（期望槽号是二元函数，见 ④）。
//
// ## 变异红线（哪个变异让我红）
//   · 公式丢 reservedSlots（return base + count）→ ①/②/④ 红
//   · 计数差一（+ count ± 1）→ ② 红
//   · addMesh/addMaterial 顺手 ++geometrySlotCount → ③ 红
//   · HeapSlotGeometryBase 生成值漂移 → ①' 红（逼 vk_resources 段布局同步）
export module test_asset_table;

import rlstd;
import asset_table;
import model_types_generated;  // HeapSlotGeometryBase（asset_table 签名用它但不 re-export）

namespace RGL {

TEST_CASE("AssetTable::expectedGeometrySlot：期望槽号 = reserved + 几何段基址 + 已占数（纯算术）") {
	AssetTable table;

	// ① 空表：下一个几何槽 = 用户槽起点（reservedSlots）+ 几何段基址（count = 0）
	CHECK(table.getGeometrySlotCount() == 0u);
	CHECK(table.expectedGeometrySlot(24u) == 24u + HeapSlotGeometryBase);
	// ①' 钉基址常量实值（= 0，生成物 model_types_generated）：几何段紧贴驱动保留槽，
	//    生成器改值必须逼 vk_resources 三段分配器（几何/纹理/runtime）同步重排。
	CHECK(HeapSlotGeometryBase == 0u);

	// ② 占 k 个几何槽（上传方 allocSlot 成功后 incrementGeometrySlotCount）→ 期望逐槽 +1
	for (uint32_t k = 1; k <= 5; ++k) {
		table.incrementGeometrySlotCount();
		CHECK(table.getGeometrySlotCount() == k);
		CHECK(table.expectedGeometrySlot(24u) == 24u + HeapSlotGeometryBase + k);
	}

	// ③ 计数单位 = 槽，不是资产条数：注册 mesh / material **不**推进期望槽号
	//   （只有显式 increment 推进——"每块 +1"由上传方维持，本表不自动数）。
	const uint32_t before = table.expectedGeometrySlot(24u);
	table.addMesh(nullptr, 9u, 12u, 0u);  // shared_ptr 空占位即可：本契约不触碰 buffer 对象
	table.addMaterial(MaterialAsset{});
	table.addMaterial(MaterialAsset{});   // find-or-add 值去重（asset_table.cppm:211-231）→ 仍 1 条
	CHECK(table.expectedGeometrySlot(24u) == before);
	CHECK(table.meshSize() == 1u);
	CHECK(table.materialSize() == 1u);

	// ④ 期望槽号只有两个输入（reservedSlots、count）：换 reservedSlots 是纯平移，
	//    与表内容零耦合。"纹理段首槽 = reserved + 几何段容量"的段间不侵扰推导，
	//    前提是几何计数不被纹理/材质记录数污染——任何这类耦合（例如公式里多出
	//    textureSize()）都会破坏这里的线性平移关系。
	CHECK(table.expectedGeometrySlot(100u) - table.expectedGeometrySlot(24u) == 100u - 24u);
	//    白纹理（纹理段首槽）本体契约在 Resources 侧（emplaceTexture 需 VkDevice，
	//    本文件刻意不构造 TextureAsset）；表侧可观测的下界 = 空表无纹理记录。
	CHECK(table.textureSize() == 0u);
}

}  // namespace RGL
