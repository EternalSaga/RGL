module;
/**
 * test_izb_sizes —— IZB head/nodes 的**尺寸/写上界公式**与注册表对账（批次 B2-4③′ 迁入）
 *
 * ## 被测契约
 *
 *  1. `izbHeadBytes` = G²×4B、`izbNodesBytes` = W·H×16B —— 期望值出处 = `izb_shadow.cppm`
 *     接口注释的布局定义（head = G² uint；nodes = W·H × 16B 四元组），**不抄实现返回值**。
 *  2. 写上界对账（判据 5）：注册 = 公式值时对账必须过（阳性锚点）；注册**偏小**时对账必须红，
 *     且消息点名两侧字节数与差值（真程序里这条在 `syncIzbGraphAttachments` throw，
 *     本文件测的是它的判据本体 `checkBufferWriteBound` + 公式配对）。
 *  3. extent 漂移 = 同名替换（B2-4③′ 契约④）：重注册后 `bufferAttachmentBytes` 必须
 *     答**新**字节数——重编译"吃到新尺寸"的机器可观测面。
 *
 * ## 变异红线（哪个变异让我红；答不出 = 摆设）
 *
 *   1. 公式抄错（head 用 G×4 / nodes 用 W·H×8）→ 用例 1 的闭式期望红。
 *   2. `izbHeadWriteBound` 与 `izbHeadBytes` 脱钩（比如写死 0）→ 用例 2 的配对对账红。
 *   3. 注册偏小时对账被放行（`>=` 改 `<=`）→ 用例 3 的红。
 *   4. 注册表同名替换失效（append 而非覆盖）→ 用例 4 的"新字节数"红。
 *   5. 附件名常量被改（"izb_head" → 别的）→ 用例 5 逐字节钉死它红
 *      （图侧声明、注册键、日志三处共用同一个常量，改名必须是有意识的动作）。
 */
#include <doctest/doctest.h>
#include <string>

export module test_izb_sizes;

import rlstd;
import rlvk;
import vkerror;
import render_graph;
import izb_shadow;

namespace RGL {
namespace {

std::string errOfBound(const std::expected<void, Error>& r) {
	return r.has_value() ? std::string{} : r.error().message;
}

}  // namespace

TEST_CASE("IZB 尺寸公式：闭式期望（G²×4 / W·H×16），出处 = 接口注释的布局定义") {
	// head = G² 个 uint32：G=64 → 4096×4 = 16384B（IZB 默认 grid，手算独立于实现）。
	CHECK(izbHeadBytes(64u) == 16384u);
	CHECK(izbHeadBytes(1u) == 4u);
	// nodes = 每像素一条 16B 链节点（u, v, lightDepth, next）：1280×720 → 921600×16。
	CHECK(izbNodesBytes(vk::Extent2D{1280u, 720u}) == 921600u * 16u);
	CHECK(izbNodesBytes(vk::Extent2D{1u, 1u}) == 16u);
}

TEST_CASE("写上界与尺寸**配对**对账：注册 = 公式值必须过（判据 5 的阳性锚点）") {
	registerBufferAttachment(kIzbHeadAttachment, [] { return izbHeadBytes(64u); });
	registerBufferAttachment(kIzbNodesAttachment,
		[] { return izbNodesBytes(vk::Extent2D{1280u, 720u}); });
	// 相等必须过（dispatch 恰好写满整块 = IZB 的常态：create 逐像素写 nodes 全表）。
	CHECK(errOfBound(checkBufferWriteBound(
		kIzbHeadAttachment, izbHeadWriteBound(64u)))
		.empty());
	CHECK(errOfBound(checkBufferWriteBound(
		kIzbNodesAttachment, izbNodesWriteBound(vk::Extent2D{1280u, 720u})))
		.empty());
}

TEST_CASE("注册偏小（容量算漏）→ 对账红，消息点名两侧与差值（判据 5 的反向半边）") {
	// 模拟"尺寸公式算漏一半"的变异现场：注册 = 公式值 ÷ 2。
	registerBufferAttachment("izb_mutation_head", [] { return izbHeadBytes(64u) / 2; });
	const auto msg = errOfBound(
		checkBufferWriteBound("izb_mutation_head", izbHeadWriteBound(64u)));
	REQUIRE_FALSE(msg.empty());
	CHECK(msg.find("izb_mutation_head") != std::string::npos);
	CHECK(msg.find("16384") != std::string::npos);  // 写上界
	CHECK(msg.find("8192") != std::string::npos);   // 注册值
	CHECK(msg.find("8192") != std::string::npos);   // 差值 = 8192（与注册值同数，都在场）
}

TEST_CASE("extent 漂移 = 同名替换：重注册后注册字节必须答新值（重编译吃到新尺寸）") {
	registerBufferAttachment("izb_drift_nodes",
		[] { return izbNodesBytes(vk::Extent2D{64u, 64u}); });
	const auto before = bufferAttachmentBytes("izb_drift_nodes");
	REQUIRE(before.has_value());
	CHECK(*before == 64u * 64u * 16u);
	// resize 后模块重注册（Renderer::syncIzbGraphAttachments 的语义）：字节数必须跟着走。
	registerBufferAttachment("izb_drift_nodes",
		[] { return izbNodesBytes(vk::Extent2D{32u, 32u}); });
	const auto after = bufferAttachmentBytes("izb_drift_nodes");
	REQUIRE(after.has_value());
	CHECK(*after == 32u * 32u * 16u);
}

TEST_CASE("附件名常量逐字节钉死：图侧声明 / 注册键 / 日志共用它") {
	CHECK(kIzbHeadAttachment == "izb_head");
	CHECK(kIzbNodesAttachment == "izb_nodes");
}

}  // namespace RGL
