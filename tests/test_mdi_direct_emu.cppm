module;
#include <doctest/doctest.h>
#include <cstdint>
#include <string>
#include <filesystem>  // 临时 lua 落盘（GMF 例外清单成员，先例 test_mdi_rows）
#include <fstream>

/**
 * test_mdi_direct_emu —— 债-140 §27 **直派模拟**（gfx1151 workaround，上游 Mesa#16670）的
 * 纯逻辑层：三态键裁决 + 行表→网格算式 + 组号→行号映射 + 空组早退判据。
 *
 * 被测对象 = `rgl/runtime_config.cppm`（resolveMdiShadowDirectEmu + 键解析）+
 * `rgl/mdi_rows.cppm`（mdiDirectEmu* 四枚）。**不是** `TEST_SUITE("gpu")`：一个设备都不碰
 * （判据 = 「需不需要一个 VkDevice 才能跑」）。
 *
 * ## 为什么值得测（截图与 gates 都看不出来的那类）
 * workaround 的语义等价在 §26.2/§28 用**截图逐位**证过一次，但那份判据只在真机真帧时跑；
 * 这四枚函数是「直派网格如何消费行表」的 CPU 侧唯一算式产地（task 侧 MDI_DIRECT 分支是
 * 第二份拼写），它们错了的形态是：行号错位 = 画错对象的影子（画面变了但没崩）、
 * 早退判据反了 = 整段影子消失或全亮、三态裁决反了 = workaround 在故障机上不生效 /
 * 在健康机上白换派发形态。这些都不需要 GPU 就能钉死。
 *
 * ## 🔴 shader 第二拼写的对账边界（引用本文件时不许读宽）
 * `shadow_meshlet.task.slang` 的 MDI_DIRECT 分支（`row = rowBase + SV_GroupID.x` +
 * `gcX == 0` 早退）与本文件的算式是**同一契约的两份拼写**（C++↔shader 无共享头的既有限制，
 * 同 blackbox CPU 镜像格局）。本文件钉 CPU 侧；两份的**数值一致**由 §28 的「键开输出与 MDI
 * 版逐位相同」截图判据整体兜底（错位/反判据都会让 400 帧截图红）。字面量级对账
 * （meshlet_spec_checks 同型）未接 = 已知边界，不谎称「双钉」。
 *
 * ## 变异红线声明（本批实测 2026-10-23；方法学四条防线 → test_mdi_rows 文件头）
 *   M1 `resolveMdiShadowDirectEmu` Auto 支 `return affectedDevice` → `return affectedDevice
 *      || true`（保参形态——裸 `return true` 撞 -Werror=unused-parameter，拿不到判据红；
 *      skill「谓词类变异要保留参数使用」本批再实测）
 *      ⟹ 红「三态裁决全表」的 Auto×非受影响格（出厂行为被换 = 最贵的一种错）。
 *   M2 同函数 On 支 `return true` → `return affectedDevice`
 *      ⟹ 红同表的 On×非受影响格（强制脚失效 = 排查工具不可信）。
 *   M3 `mdiDirectEmuGridFits` `gridX <= max` → `gridX < max`
 *      ⟹ 红「容量边界」的相等格（上界被谈掉一格）。
 *   M4 `mdiDirectEmuSkipRow` `== 0u` → `<= 1u`
 *      ⟹ 红「早退判据」的 gcX=1 格（可见行被吞 = 影子缺一块而无声）。
 *   M5 `mdiDirectEmuRow` `rowBase + groupX` → `rowBase + (groupX >> 1)`
 *      ⟹ 红「组号→行号」的闭式与单调两条（错位画邻居）。
 *   M6 键解析 `"true"` 支删掉（落到未知值）
 *      ⟹ 红「配置键」的 true 命中格（且未知值格关键词变——两条红）。
 * 每条还原后重跑全量基线全绿（防线④；本批实测坑：copyfile+touch 后**同秒**的增量构建会因
 * mtime 粒度读到陈旧 BMI——还原后构建要**跑两次或核对重编行**，M4' 那轮假红就是这么来的）。
 * 读数（2026-10-23 实测）：M1 M2 M3 M4' M5 M6 全红 ✓，逐条还原后基线全绿 ✓。
 */

export module test_mdi_direct_emu;

import rlstd;
import mdi_rows;         // mdiDirectEmuGridX / GridFits / Row / SkipRow
import runtime_config;   // MdiShadowDirectEmu / resolveMdiShadowDirectEmu / loadRuntimeConfig
import test_support;     // Lcg（写死种子，禁真随机）

namespace RGL {
namespace {

/** 临时 lua 配置（先例 test_mdi_rows::TempLua，同路径同纪律）。 */
class TempLua {
  public:
	TempLua(std::string_view tag, std::string_view body)
	: path_(std::filesystem::temp_directory_path() / ("rgl_cfg_" + std::string(tag) + ".lua")) {
		std::ofstream out(path_);
		out << body;
	}
	TempLua(const TempLua&) = delete;
	TempLua& operator=(const TempLua&) = delete;
	~TempLua() {
		std::error_code ec;
		std::filesystem::remove(path_, ec);
	}
	std::string pathStr() const { return path_.string(); }

  private:
	std::filesystem::path path_;
};

using E = MdiShadowDirectEmu;

}  // namespace

TEST_CASE("🔴 三态裁决全表：auto×设备两脚 3×2 逐格闭式（出厂行为 = Auto×非受影响 = 关）") {
	// 期望值出处 = 键语义本身（configs/runtime.lua 注释 + 字段注释），不抄被测码。
	SUBCASE("auto：受影响设备开、非受影响关（workaround 只咬 gfx1151）") {
		CHECK(RGL::resolveMdiShadowDirectEmu(E::Auto, true) == true);
		CHECK(RGL::resolveMdiShadowDirectEmu(E::Auto, false) == false);  // M1 红点
	}
	SUBCASE("true：无视设备强制开（排查/对照脚）") {
		CHECK(RGL::resolveMdiShadowDirectEmu(E::On, true) == true);
		CHECK(RGL::resolveMdiShadowDirectEmu(E::On, false) == true);  // M2 红点
	}
	SUBCASE("false：无视设备强制关（含受影响设备 = 回到今天的 MDI 形态）") {
		CHECK(RGL::resolveMdiShadowDirectEmu(E::Off, true) == false);
		CHECK(RGL::resolveMdiShadowDirectEmu(E::Off, false) == false);
	}
	// 出厂默认 = Auto（成员初始化路径，不看 lua 文件）
	CHECK(RuntimeConfig {}.meshletMdiShadowDirect == E::Auto);
}

TEST_CASE("行表→保守上界网格：gridX = 段行数（含孔洞行），多值探针钉死非常量") {
	// 探针值两两不同且 ≠ 常见常量（M：`return 常数` 的变异在这里红）。
	CHECK(RGL::mdiDirectEmuGridX(1u) == 1u);
	CHECK(RGL::mdiDirectEmuGridX(5u) == 5u);
	CHECK(RGL::mdiDirectEmuGridX(1591u) == 1591u);  // 真产物量级（sliceRows 上界口径）
	CHECK(RGL::mdiDirectEmuGridX(0u) == 0u);        // 空段算术自洽（调用方守不空，见 emit 判据）
	// Lcg 定种子随机多值：gridX 恒等（闭式期望 = 输入本身）
	RGL::testing::Lcg lcg(0xD14262u);
	for (int i = 0; i < 64; ++i) {
		const uint32_t v = static_cast<uint32_t>(lcg.uniformInt(0, 1048575));
		CHECK(RGL::mdiDirectEmuGridX(v) == v);
	}
}

TEST_CASE("容量边界（VUID-TaskEXT-07322）：相等过、+1 不过、0 恒过（上界/下界各半边）") {
	CHECK(RGL::mdiDirectEmuGridFits(65535u, 65535u) == true);  // 相等边界（M3 红点）
	CHECK(RGL::mdiDirectEmuGridFits(65536u, 65535u) == false);  // 越界一格
	CHECK(RGL::mdiDirectEmuGridFits(0u, 0u) == true);           // 下界半边
	CHECK(RGL::mdiDirectEmuGridFits(1u, 0u) == false);
	CHECK(RGL::mdiDirectEmuGridFits(1591u, 65535u) == true);  // 真产物恒过（回落分支是防御）
}

TEST_CASE("组号→行号：闭式值 + 逐组连续 + 与 mdiShadowRowLocation 同源（错位画邻居在此红）") {
	// 闭式期望（独立第二实现：行号 = 帧槽×sliceRows + 段起点 + 组号，§19.5 口径手算）
	CHECK(RGL::mdiDirectEmuRow(0u, 0u) == 0u);
	CHECK(RGL::mdiDirectEmuRow(100u, 5u) == 105u);
	CHECK(RGL::mdiDirectEmuRow(1591u * 3u, 1590u) == 1591u * 3u + 1590u);
	// 单调连续：相邻组号 → 相邻行号（M5 的 `>>1` 变异在此红——两组合并成一行）
	for (uint32_t g = 0; g + 1u < 40u; ++g)
		CHECK(RGL::mdiDirectEmuRow(7u, g + 1u) == RGL::mdiDirectEmuRow(7u, g) + 1u);
	// 🔴 同源对账：rowBase 取 `mdiShadowRowLocation` 的输出（MDI 与直派共用同一个数，
	// 分家 = task 读的行与 GPU 发的命令不是同一行）。期望值 = 闭式 (slot×slice + first) + g。
	const uint32_t sliceRows = 1591u, frameSlot = 1u, firstRow = 1591u;
	const auto loc = RGL::mdiShadowRowLocation(sliceRows, frameSlot, firstRow);
	for (uint32_t g = 0; g < 8u; ++g)
		CHECK(RGL::mdiDirectEmuRow(loc.rowBase, g) == sliceRows * frameSlot + firstRow + g);
	// argsOffset 与 rowBase 的同源（同一 pair 的另一半，行字节 = 行号 × stride）
	CHECK(loc.argsOffset == static_cast<uint64_t>(loc.rowBase) * RGL::kMdiShadowRowStride);
}

TEST_CASE("空组早退判据：只认 gcX==0（gcX=1/2/UINT32_MAX 都不许吞——可见行是命根子）") {
	CHECK(RGL::mdiDirectEmuSkipRow(0u) == true);   // 阳性锚点：MDI 下这行本就不启动 task
	CHECK(RGL::mdiDirectEmuSkipRow(1u) == false);  // M4 红点（`<=1` 吞掉全部可见行）
	CHECK(RGL::mdiDirectEmuSkipRow(2u) == false);
	CHECK(RGL::mdiDirectEmuSkipRow(UINT32_MAX) == false);
	// 与行表构建器的口径对账：构建器写的 gcX 恒 ∈ {0,1}（不变量 3b），早退判据必须
	// 「1 全放行、0 全吞」——用 Lcg 造混合可见性序列数吞放数（闭式：吞 = Σ[gcX==0]）。
	RGL::testing::Lcg lcg(0x5EED27u);
	uint32_t rows[64], zero = 0;
	for (auto& r : rows) {
		r = static_cast<uint32_t>(lcg.uniformInt(0, 1));
		zero += r == 0u ? 1u : 0u;
	}
	uint32_t skipped = 0;
	for (auto r : rows)
		skipped += RGL::mdiDirectEmuSkipRow(r) ? 1u : 0u;
	CHECK(skipped == zero);
	CHECK(zero > 0u);        // 前提守卫：全零/全一的激励让「吞放数」断言失去判别力
	CHECK(zero < 64u);
}

TEST_CASE("runtime_config：mdi_shadow_direct 默认 auto、三值命中、未知值 fail-fast、非字符串=缺键") {
	{  // 缺块 / 缺键 = Auto
		TempLua f("rt_emu_default", "return { meshlet = { assets = true } }\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletMdiShadowDirect == E::Auto);
	}
	{  // 三值各自命中（探针值 ≠ 默认：auto 那条靠显式写出 + 与 bool 键不同的读径，
	   // true/false 两条是 M6 的红点）
		struct Case { const char* lit; E want; };
		const Case cases[] = {{"auto", E::Auto}, {"true", E::On}, {"false", E::Off}};
		for (const auto& c : cases) {
			TempLua f("rt_emu_key",
				std::string("return { meshlet = { mdi_shadow_direct = \"") + c.lit + "\" } }\n");
			auto r = loadRuntimeConfig(f.pathStr());
			REQUIRE(r.has_value());
			CHECK(r->meshletMdiShadowDirect == c.want);
		}
	}
	{  // 未知字符串值 = 启动失败且点名键（笔误把 workaround 读丢比崩更糟）
		TempLua f("rt_emu_bogus", R"lua(return { meshlet = { mdi_shadow_direct = "on" } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().message.find("mdi_shadow_direct") != std::string::npos);
	}
	{  // 非字符串（布尔/数字）= 缺键保默认（applyMeshletShadowLodFallbackKey 同族边界：
	   // sol::optional<string> 读成 nullopt，⚠ 已写进键函数注释的覆盖面，不谎称 fail-fast）
		TempLua f("rt_emu_bool", R"lua(return { meshlet = { mdi_shadow_direct = true } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletMdiShadowDirect == E::Auto);
	}
	{  // 🔴 无扁平键双读（两个真相源反模式）
		TempLua f("rt_emu_flat", R"lua(return { meshlet_mdi_shadow_direct = "true" })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletMdiShadowDirect == E::Auto);
	}
}

}  // namespace RGL
