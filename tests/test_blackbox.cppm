module;
#include <doctest/doctest.h>
/**
 * test_blackbox —— 债-140 **A1 黑匣子**与 **A2 自证谓词**的纯函数面（rlbase，零设备依赖）。
 *
 * 被测对象 = `rgl/blackbox.cppm` + `blackbox_impl.cppm` 里的纯函数：
 *   `blackboxDigest` / `blackboxRowWord` / `blackboxDigestSegment` /
 *   `encodeBlackboxFrameNdjson` / `encodeSelfCheckPush` + `decodeSelfCheckPush` /
 *   `shadowRowViolations` / `describeShadowRowViolations`
 * 判据 = 「需不需要一个 VkDevice 才能跑」= 不需要 ⟹ 纯 CPU 套件（不进 `TEST_SUITE("gpu")`）。
 *
 * ## 为什么值得测（实机读数替代不了的那部分）
 * A2 的判据住在 **task 着色器**里，而着色器里的判据**没法单测**（要 GPU、要真崩）。
 * 它一旦写错（边界 `>=` 写成 `>`、word 下标数错、把 gcY 判成 gcX），实机读数就是
 * "零违例" —— 与"确实没违例"**完全同形**，我们会把一条实现 bug 读成"越界/零字段
 * 在自证覆盖面外"的排除结论。CPU 镜像（⑥）让同一条判据进 `just test` 并可被变异。
 *
 * A1 侧同理：黑匣子的全部价值在"案发前最后 N 帧"，而编码器一旦把 `count` 与 `rows`
 * 之一写错或漏写，复盘就会把"COUNT 从未超过已写行数"读成一条排除结论（④ 钉两个数
 * 必须各自在场且是原值）。
 *
 * ## 🔴 本文件最重要的一条 = 用例 ⑥
 * 五条谓词每条都要"最小违例 + 相邻合法界"两半。只测违例侧，`>=` 改成 `>` 不会红；
 * 只测 gcY 不测 gcZ，把 `||` 写成 `&&` 也不会红（skill：对称判据的每一半边各要一条）。
 *
 * ## 变异红线声明（**2026-10-21 实测，11 条全部抓到，11/11**；
 * 原文读数 = `/home/robin/program/ab/blackbox/mutate_results.txt`，
 * harness = `/home/robin/program/ab/blackbox/mutate_blackbox.py`。本文件共 **8** 条 TEST_CASE，编号 ①–⑦ + ④b）
 * 方法学六条防线（skill `rgl-test-discipline`）全部落在 harness 里：`shutil.copyfile`
 * （**不带 mtime**）+ 还原后 `touch`；显式 `cmake --build build --target rgl_tests`
 * （tests/ 是 EXCLUDE_FROM_ALL，`just build` 不编它）并核对**构建自身**返回码
 * （构建红 ≠ 判据红，单列 BUILD-FAIL 计数并立刻停）；**每条还原后重跑基线断言全绿**
 * （实测 11 次还原全部 ✅ 全绿）；禁 `git checkout`（工作树有本批未提交的实现体）；
 * 启动时自愈遗留 `.mutbak`（后台任务被回收时源码正带变异）；谓词类变异只改运算符不删参数
 * （`return false` 会撞 `-Werror=unused-parameter`，拿到的是编译失败而不是判据红）。
 *
 * | # | 变异 | 实测红在 |
 * |---|---|---|
 * | B1 | `blackboxDigest` 的 `*= kFnvPrime` → `+= kFnvPrime` | 🔯 ① |
 * | B2 | `blackboxDigest` 空 span 早退返回 0 | 🔯 ① |
 * | B3 | `blackboxRowWord` 越界守卫 `size() < 4*(i+1)` → `empty()` | 🔯 ② |
 * | B4 | `blackboxDigestSegment` 末行改取第 0 行 | 🔯 ③ |
 * | B5 | ndjson 的 `count` 实参 `s.drawCount` → `s.rowCount` | 🔯 ④ |
 * | B6 | `encodeSelfCheckPush` 超限由返回 0 改成静默截断 | 🔯 ⑤ |
 * | S1 | 谓词 1 `>=` → `>` | 🔯 ⑥ |
 * | S2 | 谓词 2 `!= 1u` → `== 0u`（本地正确的另一半）| 🔯 ⑥ |
 * | S3 | 谓词 4 `>=` → `>` | 🔯 ⑥ |
 * | S4 | 谓词 5 `||` → `&&` | 🔯 ⑥ |
 * | D1 | `describeShadowRowViolations` 漏 `object_index_oob` 的名字 | 🔯 ⑦ |
 *
 * 🔴 读数里值得注意的两点：① **每条变异都只红一条用例**（红用例数恒 =1），说明八条用例
 * 之间没有互相兜底——去掉哪条判据就只有那条用例响，这既是好事（归因干净）也意味着
 * **删用例不会立刻让别的用例变红**，后来人精简测试时没有网兜着。
 * ② S1/S3 这两条"相邻合法界漂移"能红，完全靠 ⑥ 里每条谓词都配了 `界-1 合法 / 界 违例`
 * 的成对断言——只测违例侧的话这两条会全绿（skill 的对称判据半边规则，本批实测有效）。
 */

#include <cstdint>
#include <initializer_list>  // modules 坑：range-for 的花括号列表要真 STL 头（只 include doctest 不够）
export module test_blackbox;

import rlstd;
import blackbox;

namespace RGL {
namespace {

/** 一行 24B 行表的六个 word（`kMdiShadowRowStride` 的口径，测试侧独立拼字节）。 */
std::vector<std::byte> makeRow(uint32_t gcX, uint32_t gcY, uint32_t gcZ, uint32_t objectIndex,
	uint32_t clusterStart, uint32_t meshGroupCount) {
	const uint32_t words[6] = {gcX, gcY, gcZ, objectIndex, clusterStart, meshGroupCount};
	std::vector<std::byte> out(24u);
	for (uint32_t w = 0u; w < 6u; ++w)
		for (uint32_t b = 0u; b < 4u; ++b)
			out[4u * w + b] = static_cast<std::byte>((words[w] >> (8u * b)) & 0xFFu);
	return out;
}

/** 一条**合法**的自证输入（谓词全不成立的基准态；⑥ 的每一条都在它之上只动一个字段）。 */
ShadowRowCheckInput cleanInput() {
	return ShadowRowCheckInput {
		.drawId = 5u, .rowsWritten = 10u, .groupCountX = 1u, .groupCountY = 1u, .groupCountZ = 1u,
		.objectIndex = 7u, .meshGroupCount = 3u, .objectCapacity = 4096u,
	};
}

/** 数子串出现次数（④ 用它对账键是否存在，避免把整行 JSON 塞进断言）。 */
size_t countNeedle(std::string_view hay, std::string_view needle) {
	size_t n = 0u, pos = 0u;
	while ((pos = hay.find(needle, pos)) != std::string_view::npos) {
		++n;
		pos += needle.size();
	}
	return n;
}

}  // namespace

/**
 * ① `blackboxDigest` = FNV-1a 64。
 *
 * 期望值出处 = **FNV 公开测试向量**（`http://www.isthe.com/chongo/tech/comp/fnv/` 的
 * FNV-1a 64-bit 一栏），不是抄被测码。三条各钉一个不同的失效形态：
 *  - 已知向量 → 算法本身对（阳性锚点：若函数从未生效，这里必红）；
 *  - 空 span ≠ 四个零字节的摘要 → "这段没写"与"这段写的全是零"必须可区分；
 *  - `ab` ≠ `ba` → 只 XOR 不乘法的退化哈希（顺序无关）会在这里红。
 */
TEST_CASE("① blackboxDigest = FNV-1a 64（公开向量 + 空/零可区分 + 顺序敏感）") {
	const std::string_view kEmpty;
	REQUIRE(blackboxDigest(std::as_bytes(std::span<const char>{kEmpty})) == 0xcbf29ce484222325ull);
	const std::string_view kA = "a";
	REQUIRE(blackboxDigest(std::as_bytes(std::span<const char>{kA})) == 0xaf63dc4c8601ec8cull);
	const std::string_view kFoo = "foobar";
	REQUIRE(blackboxDigest(std::as_bytes(std::span<const char>{kFoo})) == 0x85944171f73967e8ull);

	// 空 vs 全零：两者都必须与对方不同，且全零那条不得等于 0（0 = "没算"的形态）。
	const std::array<std::byte, 4> zeros {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
	const uint64_t dZero = blackboxDigest(std::span<const std::byte>{zeros});
	CHECK(dZero != blackboxDigest(std::span<const std::byte>{}));
	CHECK(dZero != 0u);

	// 顺序敏感（退化哈希的照妖镜）
	const std::string_view kAb = "ab", kBa = "ba";
	CHECK(blackboxDigest(std::as_bytes(std::span<const char>{kAb}))
	    != blackboxDigest(std::as_bytes(std::span<const char>{kBa})));
}

/**
 * ② `blackboxRowWord` 的小端与越界行为。
 *
 * 期望字节序出处 = shader 侧 `ByteAddressBuffer.Load` 的**小端**语义（契约），不是抄实现。
 * 反向用例（不足 4 字节 → 0）是必需的：没有它，"越界返回 0"与"越界读到邻居内存"
 * 在正数用例上长得一样。
 */
TEST_CASE("② blackboxRowWord 逐字节小端；不足一字返回 0 不越界读") {
	const auto row = makeRow(0x11223344u, 1u, 1u, 0xAABBCCDDu, 9u, 2u);
	const std::span<const std::byte> s{row};
	CHECK(blackboxRowWord(s, 0u) == 0x11223344u);
	CHECK(blackboxRowWord(s, 3u) == 0xAABBCCDDu);
	CHECK(blackboxRowWord(s, 5u) == 2u);
	// 显式小端锚：首字节必须是最低位字节
	CHECK(static_cast<unsigned char>(row[0]) == 0x44u);  // 显式小端锚：首字节必须是最低位字节

	// 反向：短 span 一律 0，不许读越界
	const std::array<std::byte, 5> tiny {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
		std::byte{5}};
	const std::span<const std::byte> ts{tiny};
	CHECK(blackboxRowWord(ts, 0u) == 0x04030201u);  // 前 4 字节够 → 正常读
	CHECK(blackboxRowWord(ts, 1u) == 0u);           // 只有 1 字节余 → 0
	CHECK(blackboxRowWord(std::span<const std::byte>{}, 0u) == 0u);
}

/**
 * ③ `blackboxDigestSegment` 的首/末行摘取与"无行"判定。
 *
 * 🔴 本用例的主判据是 `hasRows`：调用方把"段内零行"读成"首末行都是全零"就是假绿
 * （全零行恰好是本案的嫌疑形态之一）。所以空段/短段/零步长三条都必须 `hasRows == false`，
 * 而正常段必须 `hasRows == true` 且首末行**取自不同的行**（用两个不同的行内容钉住）。
 */
TEST_CASE("③ blackboxDigestSegment 取首末行；无行时 hasRows=false（不许读成全零行）") {
	const uint32_t stride = 24u;
	std::vector<std::byte> two;
	auto h = makeRow(1u, 1u, 1u, 11u, 2u, 5u);
	auto t = makeRow(1u, 1u, 1u, 22u, 8u, 9u);
	two.insert(two.end(), h.begin(), h.end());
	two.insert(two.end(), t.begin(), t.end());

	const auto d2 = blackboxDigestSegment(std::span<const std::byte>{two}, stride);
	REQUIRE(d2.hasRows);
	CHECK(d2.head.words[3] == 11u);
	CHECK(d2.tail.words[3] == 22u);
	CHECK(d2.tail.words[5] == 9u);
	CHECK(d2.digest != 0u);

	// 单行段：首行 == 末行（同一行，不是"末行取空"）
	const std::span<const std::byte> oneSpan{h.data(), h.size()};
	const auto d1 = blackboxDigestSegment(oneSpan, stride);
	REQUIRE(d1.hasRows);
	CHECK(d1.head.words[3] == 11u);
	CHECK(d1.tail.words[3] == 11u);

	// 三条"无行"反向：空 / 不足一行 / 零步长
	CHECK_FALSE(blackboxDigestSegment(std::span<const std::byte>{}, stride).hasRows);
	const std::array<std::byte, 23> shortRow {};
	CHECK_FALSE(blackboxDigestSegment(std::span<const std::byte>{shortRow}, stride).hasRows);
	CHECK_FALSE(blackboxDigestSegment(std::span<const std::byte>{two}, 0u).hasRows);
}

/**
 * ④ ndjson 编码：形态 + 关键事实必须在场。
 *
 * 钉的不是"好不好看"，而是**复盘能不能靠它回答本案的问题**：`count`（上传的间接计数）
 * 与 `rows`（本帧写入行数）必须同时在场且可对账 —— 本案主嫌疑就是 `count > rows`。
 * 因此这里显式造一条 `count > rows` 的记录并断言两个数都按原值出现（不是被某个
 * "反正相等"的字段替掉）。
 */
TEST_CASE("④ encodeBlackboxFrameNdjson 是单行合法形态，且 COUNT 与 rows 各自在场") {
	BlackboxFrameRecord rec;
	rec.frame = 1234u;
	rec.timestampNs = 99887766u;
	rec.segmentCount = 1u;
	rec.segments[0].cascadeIndex = 2u;
	rec.segments[0].frameSlot = 1u;
	rec.segments[0].firstRow = 1530u;
	rec.segments[0].rowCount = 1591u;
	rec.segments[0].taskCount = 1200u;
	rec.segments[0].argsOffset = 36720u;
	rec.segments[0].rowBase = 1530u;
	rec.segments[0].drawCount = 1592u;  // 🔴 故意 COUNT > rows（本案主嫌疑形态）
	rec.segments[0].drawCountSource = 1591u;
	rec.segments[0].sliceDigest = 0x0123456789ABCDEFull;
	rec.segments[0].headRowIndex = 1530u;
	rec.segments[0].tailRowIndex = 3120u;
	rec.segments[0].head = blackboxDigestSegment(
		std::span<const std::byte>{}, 24u).head;
	rec.heapSlotCount = 2u;
	rec.heapSlots[0] = 41u;
	rec.heapSlots[1] = 34u;
	rec.dgcActive = 1u;
	rec.selfCheckOn = 1u;

	const std::string line = encodeBlackboxFrameNdjson(rec);
	// 形态：单行、首尾花括号、花括号配平
	CHECK(line.size() > 2u);
	CHECK(line.back() == '\n');
	CHECK(line.find('\n') == line.size() - 1u);  // 只有一个换行，且在末尾
	CHECK(countNeedle(line, "{") == countNeedle(line, "}"));
	CHECK(line.front() == '{');

	// 关键事实必须在场，且是**原值**
	CHECK(line.find("\"frame\":1234") != std::string::npos);
	CHECK(line.find("\"rows\":1591") != std::string::npos);
	CHECK(line.find("\"count\":1592") != std::string::npos);
	CHECK(line.find("\"count_src\":1591") != std::string::npos);
	CHECK(line.find("0x0123456789abcdef") != std::string::npos);
	CHECK(line.find("\"heap_slots\":[41,34]") != std::string::npos);
	CHECK(line.find("\"dgc\":1") != std::string::npos);
	CHECK(line.find("\"selfcheck\":1") != std::string::npos);
	// 段对象必须恰一个（段数对账，多写/漏写都会在这里红）
	CHECK(countNeedle(line, "\"s\":") == 0u);
	CHECK(countNeedle(line, "\"s0\":") == 1u);
	CHECK(line.find("\"s1\":") == std::string::npos);
}

/** ④b 零段记录：不许凭空产出段对象（对空集合的断言要加前提守卫的反向配套）。 */
TEST_CASE("④b 零段记录只有一层花括号，且没有 s0") {
	BlackboxFrameRecord rec;
	rec.frame = 7u;
	const std::string line = encodeBlackboxFrameNdjson(rec);
	CHECK(countNeedle(line, "{") == 1u);
	CHECK(countNeedle(line, "}") == 1u);
	CHECK(line.find("\"s0\":") == std::string::npos);
	CHECK(line.find("\"heap_slots\"") == std::string::npos);  // 槽数 0 → 不写这个键
}

/**
 * ⑤ 自证 push 的打包/拆包往返，以及**超限不截断**。
 *
 * 🔴 超限返回 0 是本函数的判据核心：截断（`& 0xFFFF`）会让界**变小**，于是
 * "行号 ≥ 变小的界"更容易成立 → 那是假**阳**；但反过来若把界截断成 0，
 * `row >= 0` 恒真 = 每行都报 = 刷屏；而若实现写成"截断后照用"，某些行会**漏报**。
 * 三种失效里"漏报"是最贵的，所以这里钉：任一界 > 65535 → 返回 0（= 明确不可用），
 * 且往返必须无损。
 */
TEST_CASE("⑤ encodeSelfCheckPush 往返无损；任一界超 16 位则返回 0 不截断") {
	for (uint32_t rows : {0u, 1u, 1591u, 4773u, 65535u}) {
		for (uint32_t cap : {1u, 4096u, 65535u}) {
			const uint32_t w = encodeSelfCheckPush(rows, cap);
			// 本循环里 cap 恒 ≥ 1 ⟹ 合法打包结果必非 0（0 是"超限不可用"的哨兵值）。
			// 这条同时钉住"合法输入被误判成超限"。
			CHECK(w != 0u);
			const auto back = decodeSelfCheckPush(w);
			CHECK(back.rowsWritten == rows);
			CHECK(back.objectCapacity == cap);
		}
	}
	// 超限：必须返回 0（哨兵），**不许**静默截断
	CHECK(encodeSelfCheckPush(65536u, 4096u) == 0u);
	CHECK(encodeSelfCheckPush(1591u, 65536u) == 0u);
	CHECK(encodeSelfCheckPush(0u, 0u) == 0u);  // 两个界都 0 是合法打包（结果恰为 0）
	// 拆包的位段归属：低 16 位是 rowsWritten，高 16 位是 objectCapacity
	const auto d = decodeSelfCheckPush(0x00020003u);
	CHECK(d.rowsWritten == 3u);
	CHECK(d.objectCapacity == 2u);
}

/**
 * ⑥ **本文件主判据**：自证谓词 CPU 镜像的五条，每条一个最小违例 + 一个相邻合法界。
 *
 * 🔴 为什么这份镜像必须存在：shader 里的判据**没法单测**（要 GPU、要真崩），而它一旦写错
 * （边界用 `>` 而不是 `>=`、word 下标数错、把 Y/Z 判成 X），读数就是"零违例"——
 * 与"确实没违例"**完全同形**。CPU 镜像让同一条判据进 `just test`，变异它必须红。
 *
 * 每条都配"相邻合法界"：只测违例侧，`>=` 写成 `>` 不会红（skill 的"对称判据每半边各一条"）。
 */
TEST_CASE("⑥ shadowRowViolations 五条谓词各自最小违例 + 相邻合法界（主判据）") {
	// 基准态必须零位 —— 否则后面每条都在测"基准就错"
	CHECK(shadowRowViolations(cleanInput()) == kShadowViolNone);

	// 谓词 1：间接计数与已上传行数分家（段内相对口径）。边界 = rowsWritten 本身
	{
		auto in = cleanInput();
		in.drawId = 9u;  // rowsWritten-1 → 合法
		CHECK(shadowRowViolations(in) == kShadowViolNone);
		in.drawId = 10u;  // == rowsWritten → 违例
		CHECK(shadowRowViolations(in) == kShadowViolRowBeyondWritten);
		in.drawId = 11u;
		CHECK(shadowRowViolations(in) == kShadowViolRowBeyondWritten);
		in.rowsWritten = 0u;  // 一段都没写 → 任何 drawID 都越界（不许因"界为 0"跳过判据）
		in.drawId = 0u;
		CHECK((shadowRowViolations(in) & kShadowViolRowBeyondWritten) != 0u);
	}
	// 谓词 2：gcX 必须恰为 1（0 与 2 都算违例 —— 0 是"空 task"，2 是 ×2 被乘回）
	{
		auto in = cleanInput();
		in.groupCountX = 0u;
		CHECK(shadowRowViolations(in) == kShadowViolGcxNotOne);
		in.groupCountX = 2u;
		CHECK(shadowRowViolations(in) == kShadowViolGcxNotOne);
		in.groupCountX = 1u;
		CHECK(shadowRowViolations(in) == kShadowViolNone);
	}
	// 谓词 3：meshGroupCount 为 0 = 会发空 DispatchMesh；1 是合法下界
	{
		auto in = cleanInput();
		in.meshGroupCount = 0u;
		CHECK(shadowRowViolations(in) == kShadowViolMeshGroupCountZero);
		in.meshGroupCount = 1u;
		CHECK(shadowRowViolations(in) == kShadowViolNone);
	}
	// 谓词 4：objectIndex 上界是**开区间**（capacity-1 合法，capacity 违例）
	{
		auto in = cleanInput();
		in.objectIndex = 4095u;
		CHECK(shadowRowViolations(in) == kShadowViolNone);
		in.objectIndex = 4096u;
		CHECK(shadowRowViolations(in) == kShadowViolObjectIndexOob);
		in.objectIndex = 0xFFFFFFFFu;  // 陈旧行常见形态
		CHECK(shadowRowViolations(in) == kShadowViolObjectIndexOob);
	}
	// 谓词 5：gcY / gcZ **各自**都要判（只判 Y 的弱化变异必须被 Z 那条抓到）
	{
		auto in = cleanInput();
		in.groupCountY = 0u;
		CHECK(shadowRowViolations(in) == kShadowViolRowMisaligned);
		in = cleanInput();
		in.groupCountZ = 0u;
		CHECK(shadowRowViolations(in) == kShadowViolRowMisaligned);
		in = cleanInput();
		in.groupCountY = 3u;
		in.groupCountZ = 4u;
		CHECK(shadowRowViolations(in) == kShadowViolRowMisaligned);
	}
	// 组合：一次同时踩 1 与 4（"计数分家 + 行内容是垃圾"的复合形态）—— 两个位都要在
	{
		auto in = cleanInput();
		in.drawId = 4000u;      // ≥ rowsWritten(10) → 谓词 1
		in.objectIndex = 9000u;  // ≥ objectCapacity(4096) → 谓词 4
		const uint32_t bits = shadowRowViolations(in);
		CHECK((bits & kShadowViolRowBeyondWritten) != 0u);
		CHECK((bits & kShadowViolObjectIndexOob) != 0u);
		CHECK((bits & kShadowViolGcxNotOne) == 0u);
	}
}

/**
 * ⑦ 违例位段 → 文本：none / 单条 / 组合 / 未知位四种出口两两不同。
 *
 * "未知位"那条不是洁癖：位段与描述表分家时（加了枚举忘了加名字），
 * 日志会显示 `none` 从而把一次真违例读成"没违例"。
 */
TEST_CASE("⑦ describeShadowRowViolations 四种出口两两不同且可归因") {
	CHECK(describeShadowRowViolations(kShadowViolNone) == "none");
	CHECK(describeShadowRowViolations(kShadowViolRowBeyondWritten)
	    == "row_beyond_written");
	CHECK(describeShadowRowViolations(kShadowViolObjectIndexOob)
	    == "object_index_oob");
	const std::string both = describeShadowRowViolations(
		kShadowViolRowBeyondWritten | kShadowViolObjectIndexOob);
	CHECK(both == "row_beyond_written|object_index_oob");
	CHECK(both != "none");
	// 未知位：必须**不是** "none"
	CHECK(describeShadowRowViolations(1u << 30u) != "none");
	// 五条都要有名字（漏一条 = 该位单独出现时被读成 unknown，且与"未知位"同形）
	for (uint32_t bit : {1u << 0, 1u << 1, 1u << 2, 1u << 3, 1u << 4})
		CHECK(describeShadowRowViolations(bit) != "unknown_bits");
}

}  // namespace RGL
