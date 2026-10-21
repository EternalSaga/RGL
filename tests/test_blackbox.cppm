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
 * ## 变异红线声明（**三批合计 28 条，全部抓到，28/28**）
 *
 * 上上批（task 侧 + 黑匣子编码）**11/11**，2026-10-21 实测：原文读数 =
 * `ab/blackbox/mutate_results.txt`，harness = `ab/blackbox/mutate_blackbox.py`。
 * 上批（mesh 取址链 + latch-once）**10/10**，同日晚些：原文读数 =
 * `ab/meshcheck/mutate_results.txt`，harness = `ab/meshcheck/mutate_mesh.py`。
 * 本批（债-143 CPU 断言 ⑪⑫ + 钳制护栏）**7/7**，2026-10-22 实测：原文读数 =
 * `ab/debt143/mutate_results_merged.txt`，harness = `ab/debt143/mutate_debt143.py`。
 * 本文件共 **13** 条 TEST_CASE（编号 ①–⑫，含 ④b）。
 *
 * 方法学六条防线（skill `rgl-test-discipline`）全部落在 harness 里：`shutil.copyfile`
 * （**不带 mtime**）+ 还原后 `touch`；显式 `cmake --build build --target rgl_tests`
 * （tests/ 是 EXCLUDE_FROM_ALL，`just build` 不编它）并核对**构建自身**返回码
 * （构建红 ≠ 判据红，单列 BUILD-FAIL 计数并立刻停）；**每条还原后重跑基线断言全绿**
 * （两批合计 21 次还原全部 ✅ 全绿）；禁 `git checkout`（工作树有本批未提交的实现体）；
 * 启动时自愈遗留 `.mbak` / `.mutbak`（后台任务被回收时源码正带变异）；谓词类变异只改
 * 运算符不删参数（`return false` 会撞 `-Werror=unused-parameter`，拿到的是编译失败而不是判据红）。
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
 * | M1 | P1 `clusterStart >= rangeCount` → `>`（相邻合法界漂移）| 🔯 ⑧ |
 * | M2 | P2 丢掉 `clusterStart` 项（只看派发序号，本地看似合理）| 🔯 ⑧ |
 * | M3 | P3 簇条目守卫少加一个步长（off-by-stride）| 🔯 ⑧ |
 * | M4 | P6 顶点池判据丢掉顶点步长（少判一整条顶点）| 🔯 ⑧ |
 * | M5 | P8 回绕阈值 32 位 → 40 位（回绕判据形同不存在）| 🔯 ⑧ |
 * | M6 | 去掉段级两判的「条目可读」门控（与 shader 控制流脱钩）| 🔯 ⑧ |
 * | M7 | `describeMeshChainViolations` 漏 `vertex_pool_oob` 的名字 | 🔯 ⑨ |
 * | L1 | 🔴 mesh 自证的 `clusterIdx` 改回直读 `groupID.x` | 🔯 ⑩ |
 * | L2 | 🔴 mesh 自证的 `half` 改回直读 `groupID.x` | 🔯 ⑩ |
 * | L3 | 🔴 task 谓词 1 改回直读 `drawID`（判据与取数可能不同行）| 🔯 ⑩ |
 * | R1 | 债-143 判据 `>` → `>=`（needed == rangeCount 的合法界被误判）| 🔯 ⑪ |
 * | R2 | needed 丢掉 maxIntra 项（只看起点，多要几簇看不见）| 🔯 ⑪ |
 * | R3 | 🔴 needed 改 32 位算术（回绕把"越界"判成"没越界"= 假阴）| 🔯 ⑪ |
 * | R4 | describe 漏 `cluster_past_range` 的名字（CPU↔GPU 同名桥断）| 🔯 ⑫ |
 * | R5 | 最大簇内序 `(mgc−1)>>1` → `mgc>>1`（偶数组多算一簇）| 🔯 ⑪ |
 * | R6 | mgc=0 早退由 0 改 1（不派发的行也报违例 = 反向用例响）| 🔯 ⑪ |
 * | R7 | 🔴 钳制 `>=` → `>`（超发第一组漏网 = 修复本体被弱化；不重编即红）| 🔯 ⑩ |
 *
 * 🔴 读数里值得注意的四点：① **每条变异都只红一条用例**（红用例数恒 =1），说明各条用例
 * 之间没有互相兜底——去掉哪条判据就只有那条用例响，这既是好事（归因干净）也意味着
 * **删用例不会立刻让别的用例变红**，后来人精简测试时没有网兜着。
 * ② S1/S3/M1 这三条"相邻合法界漂移"能红，完全靠 ⑥⑧ 里每条谓词都配了
 * `界-1 合法 / 界 违例` 的成对断言——只测违例侧的话这三条会全绿（对称判据半边规则，实测有效）。
 * ③ 🔴 **两类变异的构建要求相反**，harness 因此给每条变异单独配 `build` 开关而不是全局一刀切：
 * C++ 变异（M/D/S 类）**必须重编**否则量的是旧二进制；latch 变异（L 类）**必须不重编**——
 * ⑩ 是**运行时读 `.slang` 源**的结构护栏，若这里也去 build，就把"改源即生效"这条性质
 * 掩盖掉了（后来人会以为要重编，进而去掉运行时读源的设计）。L1–L3 实测**不重编即红**，
 * 这同时是 ⑩ 自身有效性的证据。
 * ④ M6 是这批最该有的一条：段级两判在镜像里是**按"条目可读"门控**的（与 shader 控制流同形
 * ——条目越界时那些值根本不存在，不能拿 0 去判）。去掉门控后镜像会在 shader 不判的地方判，
 * 于是反向用例红。这条变异证明"门控"不是可有可无的写法偏好，而是判据的一部分。
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

/**
 * mesh 链的**合法基准态**（⑧ 的每一条都在它之上只动一个字段）。
 * 数值量级取自实机黑匣子：obj 1590 / cstart 11 / 簇步长 44 / 顶点步长 48。
 */
MeshChainCheckInput meshClean() {
	MeshChainCheckInput in;
	in.objectIndex = 1590u;
	in.objectCapacity = 4096u;
	in.rangeCount = 12u;
	in.clusterStart = 11u;
	in.intraCluster = 0u;
	in.clusterIdx = 100u;
	in.clusterStride = 44u;
	in.clustersBytes = 100000u;
	in.triOffset = 5000u;
	in.triCount = 20u;
	in.trisBytes = 20000u;
	in.remapOffset = 300u;
	in.vertCount = 15u;
	in.remapBytes = 8000u;
	in.vertexPoolBytes = 100000u;
	in.vertexStride = 48u;
	in.vertexValid = true;
	in.localIdx = 3u;
	in.vb = 480u;
	return in;
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

/**
 * ⑧ **mesh 取址链七判的 CPU 镜像**（本批主判据之一）。
 *
 * 与 ⑥ 同一条纪律：每条都要"最小违例 + 相邻合法界"两半，只测违例侧则 `>=` 写成 `>` 不会红。
 * 🔴 另配一条**反向门控**用例：簇条目不可读时，段级两判必须**不评**——否则镜像拿一堆 0 去判
 * 越界，与 shader 的控制流不同形（shader 那时根本没读条目，triOffset 等值不存在）。
 */
TEST_CASE("⑧ meshChainViolations 七判各自最小违例 + 相邻合法界 + 条目不可读时段判不评") {
	// 基准态必须零位（数值取的是实机黑匣子里的真实量级：obj 1590 / cstart 11 / 步长 44 / 48）
	CHECK(meshChainViolations(meshClean()) == kMeshViolNone);

	// P0 objectIndex：上界是**开区间**
	{
		auto in = meshClean();
		in.objectIndex = 4095u;
		CHECK(meshChainViolations(in) == kMeshViolNone);
		in.objectIndex = 4096u;
		CHECK(meshChainViolations(in) == kMeshViolObjectIndexOob);
		in.objectIndex = 0xFFFFFFFFu;
		CHECK(meshChainViolations(in) == kMeshViolObjectIndexOob);
	}
	// P1 clusterStart 真上界（= range.count）：上一批覆盖面清单第 1 条，本批补上的那条
	{
		auto in = meshClean();
		in.clusterStart = 11u;  // rangeCount-1 → 合法
		in.intraCluster = 0u;
		CHECK(meshChainViolations(in) == kMeshViolNone);
		in.clusterStart = 12u;  // == rangeCount → 违例
		CHECK(meshChainViolations(in) == kMeshViolClusterStartOob);
	}
	// P2 派发序号推出区间：与 P1 **互斥**（归因要分清是行表值坏还是 groupCount 算错）
	{
		auto in = meshClean();
		in.clusterStart = 11u;
		in.intraCluster = 0u;
		CHECK(meshChainViolations(in) == kMeshViolNone);
		in.intraCluster = 1u;  // 11 + 1 == rangeCount → 只报 P2，不报 P1
		const uint32_t b = meshChainViolations(in);
		CHECK((b & kMeshViolClusterPastRange) != 0u);
		CHECK((b & kMeshViolClusterStartOob) == 0u);
	}
	// P3 簇条目越簇表尾（引擎已有守卫，本判据是给它接声音）+ P8 32 位回绕
	{
		auto in = meshClean();
		// cBase + stride == clustersBytes 是合法的（条目正好占满最后 44B）
		in.clusterIdx = 2272u;  // 2272*44 = 99968；+44 = 100012 > 100000 → 违例
		CHECK((meshChainViolations(in) & kMeshViolClusterEntryOob) != 0u);
		in.clusterIdx = 2271u;  // 2271*44 = 99924；+44 = 99968 ≤ 100000 → 合法
		CHECK(meshChainViolations(in) == kMeshViolNone);
		// P8：clusterIdx × 44 超过 32 位能表示的范围（引擎原码会回绕、守卫判不出来）
		in.clusterIdx = 97612893u;  // ×44 = 4294967292 ≤ 0xFFFFFFFF → 不回绕
		CHECK((meshChainViolations(in) & kMeshViolClusterBaseWrapped) == 0u);
		in.clusterIdx = 97612894u;  // ×44 = 4294967336 > 0xFFFFFFFF → 回绕
		CHECK((meshChainViolations(in) & kMeshViolClusterBaseWrapped) != 0u);
	}
	// P4 Tris 段越界（原码**无守卫**）
	{
		auto in = meshClean();
		in.triOffset = 20000u - 3u * in.triCount;  // 正好占满 → 合法
		CHECK(meshChainViolations(in) == kMeshViolNone);
		in.triOffset = 20000u - 3u * in.triCount + 1u;  // 多一字节 → 违例
		CHECK(meshChainViolations(in) == kMeshViolTriSectionOob);
	}
	// P5 Remap 段越界（原码**无守卫**）
	{
		auto in = meshClean();
		in.remapOffset = 8000u / 4u - in.vertCount;  // (2000-15)*4 + 15*4 = 8000 → 合法
		CHECK(meshChainViolations(in) == kMeshViolNone);
		in.remapOffset = 8000u / 4u - in.vertCount + 1u;
		CHECK(meshChainViolations(in) == kMeshViolRemapSectionOob);
	}
	// P6 顶点池越界 = fault@0 前一次读；P7 局部索引越本簇
	{
		auto in = meshClean();
		in.vb = 100000u - 48u;  // 正好最后一个顶点 → 合法
		CHECK(meshChainViolations(in) == kMeshViolNone);
		in.vb = 100000u - 48u + 1u;
		CHECK(meshChainViolations(in) == kMeshViolVertexPoolOob);
		in = meshClean();
		in.localIdx = 14u;  // vertCount-1 → 合法
		CHECK(meshChainViolations(in) == kMeshViolNone);
		in.localIdx = 15u;  // == vertCount → 违例
		CHECK(meshChainViolations(in) == kMeshViolLocalIdxPastCluster);
	}
	// 🔴 反向门控：条目不可读时，段级两判**必须不评**（shader 那时没读条目，值不存在）
	{
		auto in = meshClean();
		in.clusterIdx = 97612894u;  // 条目必然不可读
		in.triOffset = 0xFFFFFF00u;  // 故意放会回绕的垃圾值
		in.triCount = 0xFFFFFFFu;
		in.remapOffset = 0xFFFFFFF0u;
		in.vertCount = 0xFFFFFFFu;
		const uint32_t b = meshChainViolations(in);
		CHECK((b & kMeshViolClusterEntryOob) != 0u);
		CHECK((b & kMeshViolTriSectionOob) == 0u);     // 没评 = 不报，与 shader 同形
		CHECK((b & kMeshViolRemapSectionOob) == 0u);   // 同上
	}
	// 组合：真实故障形态"条目在表内但属于别的对象"→ P4/P5 一起中，P1/P3 都不中
	{
		auto in = meshClean();
		in.triOffset = 900000u;  // 远超 trisBytes
		in.remapOffset = 900000u;
		const uint32_t b = meshChainViolations(in);
		CHECK((b & kMeshViolTriSectionOob) != 0u);
		CHECK((b & kMeshViolRemapSectionOob) != 0u);
		CHECK((b & (kMeshViolClusterStartOob | kMeshViolClusterEntryOob)) == 0u);
	}
}

/** ⑨ mesh 链位段 → 文本：八条都要有名字，未知位不许读成 none。 */
TEST_CASE("⑨ describeMeshChainViolations 八条各有名字且可归因") {
	CHECK(describeMeshChainViolations(kMeshViolNone) == "none");
	CHECK(describeMeshChainViolations(kMeshViolClusterStartOob) == "cluster_start_oob");
	CHECK(describeMeshChainViolations(kMeshViolVertexPoolOob) == "vertex_pool_oob");
	CHECK(describeMeshChainViolations(kMeshViolClusterBaseWrapped) == "cluster_base_wrapped");
	const std::string both =
		describeMeshChainViolations(kMeshViolTriSectionOob | kMeshViolRemapSectionOob);
	CHECK(both == "tri_section_oob|remap_section_oob");
	CHECK(describeMeshChainViolations(1u << 30u) != "none");
	for (uint32_t bit : {1u << 0, 1u << 1, 1u << 2, 1u << 3, 1u << 4, 1u << 5, 1u << 6, 1u << 7,
			1u << 8})
		CHECK(describeMeshChainViolations(bit) != "unknown_bits");
}

// ───────────── ⑩ latch-once 结构护栏（读 .slang 源本体）─────────────

/** 读源文件并剥掉行注释与块注释后返回。护栏数的是**代码里的**读，注释里提几次都不算。 */
std::string readSlangStripped(const char* file) {
	const std::string path = std::string(RGL_SHADER_SRC_DIR) + "/" + file;
	std::ifstream in(path, std::ios::binary | std::ios::ate);
	if (!in.good())
		return {};  // 读不到源 = 护栏空转，下面的 REQUIRE_FALSE(mesh.empty()) 会当场红
	const size_t size = static_cast<size_t>(in.tellg());
	in.seekg(0, std::ios::beg);
	std::string all(size, '\0');
	in.read(all.data(), static_cast<long>(size));
	std::string out;
	out.reserve(all.size());
	for (size_t i = 0; i < all.size();) {
		if (all.compare(i, 2, "//") == 0) {
			const auto nl = all.find('\n', i);
			if (nl == std::string::npos)
				break;
			i = nl;
			continue;
		}
		if (all.compare(i, 2, "/*") == 0) {
			const auto end = all.find("*/", i + 2);
			if (end == std::string::npos)
				break;
			i = end + 2;
			continue;
		}
		out += all[i++];
	}
	return out;
}

size_t countToken(const std::string& hay, std::string_view needle) {
	size_t n = 0, pos = 0;
	while ((pos = hay.find(needle, pos)) != std::string::npos) {
		++n;
		pos += needle.size();
	}
	return n;
}

/**
 * ⑩ **latch-once 结构护栏**：自证变体里"被验的值"必须是**读一次进局部、之后判据与使用共用
 * 那份拷贝**。这条性质住在 shader 源码里，编译产物与运行时都判不了 ⟹ 只能对源文本下判据
 * （先例 = `check_shader_deps.py` 的 R5 数裸字节偏移）。
 *
 * 🔴 判据为什么长这样：自证支路里 `groupID.x` / `drawID` / `mdiTaskPush.rowBase` 这些原始输入
 *    **只允许出现在 latch 那一次**；一旦有人把某个使用点改回直读 intrinsic，计数就涨，本用例
 *    当场红（变异 L1 实测）。注释里的提及不算（已先剥注释）。
 * 🔴 期望值是**结构**而不是巧合，且逐处归属写在断言旁边：mesh 侧 `groupID.x` 全文 4 处
 *    = 自证 latch 点 1 + MDI 原版 1 + 非 MDI 出厂支 1 + 原版 half 1。若哪天原版也并回 latch
 *    （那是改进），这里应当**跟着改数**，不是把断言删掉。
 */
TEST_CASE("⑩ latch-once：自证支路只读一次 intrinsic，使用点全走拷贝") {
	const std::string mesh = readSlangStripped("shadow_meshlet.mesh.slang");
	const std::string task = readSlangStripped("shadow_meshlet.task.slang");
	REQUIRE_FALSE(mesh.empty());
	REQUIRE_FALSE(task.empty());

	// ── mesh：`groupID.x` 全文 5 处，归属**逐处钉死**（少一处或多一处都说明结构变了）──
	//   L?  const uint latchedSlot = groupID.x;                       ← 自证 latch 点（唯一）
	//   L?  ... mdiPayload.clusterStart + (groupID.x >> 1u);          ← MDI 原版 #else
	//   L?  ... shadowMeshletDrawArgs.clusterStart + (groupID.x >> 1u); ← 非 MDI 出厂支
	//   L?  const uint half = groupID.x & 1u;                          ← 原版 half
	//   L?  if (groupID.x >= mdiPayload.meshGroupCount)                ← 债-143 原版钳制
	CHECK(countToken(mesh, "groupID.x") == 5u);
	CHECK(countToken(mesh, "mdiPayload.objectIndex") == 3u);   // latch 点 + MDI 原版两处解引用
	CHECK(countToken(mesh, "mdiPayload.clusterStart") == 2u);  // latch 点 + MDI 原版 range 读
	// 🔴 债-143 钳制的结构护栏：payload 上界的**同构声明两侧各一处**、task 两支各写一次、
	//    mesh 两支各钳一次（自证支走 latch 拷贝、原版走 payload 直读——原版没有 latch 纪律，
	//    它与自证支的差异**只剩身份来源**这条既有口径不许被破坏）。
	CHECK(countToken(mesh, "mdiPayload.meshGroupCount") == 2u);    // 自证 latch 点 + 原版钳制
	CHECK(countToken(mesh, "latchedMeshGroupCount") == 2u);        // latch 点 + 自证钳制
	CHECK(countToken(mesh, "uint meshGroupCount;") == 1u);
	CHECK(countToken(task, "uint meshGroupCount;") == 1u);
	CHECK(countToken(task, "p.meshGroupCount") == 2u);             // 自证支 + 原版各写一次
	// 🔴 钳制的**精确形态**各钉一条（`>=` 改成 `>` = 超发的第一组漏网，这里当场红）：
	CHECK(countToken(mesh, "if (latchedSlot >= latchedMeshGroupCount) {") == 1u);
	CHECK(countToken(mesh, "if (groupID.x >= mdiPayload.meshGroupCount) {") == 1u);
	// 🔴 真正锋利的三条：**自证支路里不许出现任何对 groupID.x 的直接运算**。
	//    把使用点改回直读 intrinsic（变异 L1）会立刻把下面两条推高。
	CHECK(countToken(mesh, "groupID.x >> 1u") == 2u);  // 只许出现在两条非自证支
	CHECK(countToken(mesh, "groupID.x & 1u") == 1u);   // 只许是原版那条 half
	CHECK(countToken(mesh, "latchedSlot >> 1u") >= 2u);  // 自证的 clusterIdx + P2
	CHECK(countToken(mesh, "latchedSlot & 1u") == 1u);   // 自证的 half
	CHECK(countToken(mesh, "latchedSlot") >= 6u);  // latch + clusterIdx + P2 + half + 债-143 钳制
	CHECK(countToken(mesh, "latchedObjectIndex") >= 4u);

	// ── task：`drawID` 3 处 = 形参声明 + latch + 原版 #else；`mdiTaskPush.rowBase` 2 处同理 ──
	CHECK(countToken(task, "drawID") == 3u);
	CHECK(countToken(task, "mdiTaskPush.rowBase") == 2u);
	CHECK(countToken(task, "latchedDrawId") >= 3u);
	CHECK(countToken(task, "latchedRowBase") >= 3u);
	CHECK(countToken(task, "latchedObjectIndex") >= 4u);
	// 🔴 反向：判据里**不许**再出现"重算 row"的写法 —— 上一批的 TOCTOU 就来自这一串
	//    （`row` 曾把 `mdiTaskPush.rowBase + drawID` 又算一遍，而不是复用算 rowBytes 那份）。
	//    现在它只允许存在于原版 #else 那一处。
	CHECK(countToken(task, "mdiTaskPush.rowBase + drawID") == 1u);
}

// ───────────── ⑪⑫ 债-143 CPU 断言（行要的簇数 ≤ 对象簇区间）─────────────

/**
 * 债-143 审计的**合法基准行**（⑪ 的每一条都在它之上只动一个字段）。
 * 数值取的是实机命中现场：obj=681 / range_base=22277 / range_count=1 / cstart=0，
 * 但 mgc 取**合法值 2**（1 簇 × 2 槽）——GPU 抓到的形态是 mgc≥4，那正是违例侧。
 */
ShadowRowRangeInput rowClean() {
	return ShadowRowRangeInput {
		.objectIndex = 681u, .objectCapacity = 4096u, .clusterStart = 0u,
		.meshGroupCount = 2u, .rangeCount = 1u,
	};
}

/**
 * ⑪ **债-143 CPU 断言**（mesh 自证 P2 的 CPU 对偶，常开审计的判据本体）。
 *
 * 纪律同 ⑥⑧：每条判据"最小违例 + 相邻合法界"两半（只测违例侧则 `>` 写成 `>=` 不红）；
 * 反向用例（mgc=0 不许响）钉"没触发"与"坏掉了"可区分；64 位那条钉回绕假阴——
 * `needed` 的期望值全部**手算闭式**（= clusterStart + ((mgc−1)>>1) + 1），不抄被测码。
 */
TEST_CASE("⑪ shadowRowRangeViolations 最小违例 + 相邻合法界 + needed 闭式 + 64 位不回绕") {
	// 基准态必须零位（单簇对象 + 2 组 = 恰好填满区间）
	CHECK(shadowRowRangeViolations(rowClean()) == kRowRangeViolNone);

	// needed 闭式（期望值 = 手算，出处是接口注释里的数学契约）：
	//   mgc=0 → 0（不派发）；mgc=1/2 → cstart+1；mgc=3/4 → cstart+2；奇数不整除丢尾
	CHECK(shadowRowRangeNeeded(0u, 0u) == 0u);
	CHECK(shadowRowRangeNeeded(0u, 1u) == 1u);
	CHECK(shadowRowRangeNeeded(0u, 2u) == 1u);
	CHECK(shadowRowRangeNeeded(0u, 3u) == 2u);
	CHECK(shadowRowRangeNeeded(0u, 4u) == 2u);
	CHECK(shadowRowRangeNeeded(35u, 2u) == 36u);  // obj=774 的合法形态（区间 36 恰好填满）
	CHECK(shadowRowRangeNeeded(35u, 4u) == 37u);  // 同现场多要 1 簇 = GPU 命中的形态

	// past_range 边界对（区间 1 簇）：mgc=2 合法 / mgc=4 违例（要 2 簇）
	{
		auto in = rowClean();
		in.meshGroupCount = 4u;
		CHECK(shadowRowRangeViolations(in) == kRowRangeViolPastRange);
		in.meshGroupCount = 3u;  // 奇数组（表与行分家的病态形态）也按实际派发量判
		CHECK(shadowRowRangeViolations(in) == kRowRangeViolPastRange);
		in.meshGroupCount = 2u;
		CHECK(shadowRowRangeViolations(in) == kRowRangeViolNone);
	}
	// 大区间尾例（obj=774 现场：cstart=35 / rc=36）：界上合法、越一簇违例
	{
		auto in = rowClean();
		in.clusterStart = 35u;
		in.rangeCount = 36u;
		in.meshGroupCount = 2u;
		CHECK(shadowRowRangeViolations(in) == kRowRangeViolNone);  // needed=36 == rc → 合法
		in.meshGroupCount = 4u;
		CHECK(shadowRowRangeViolations(in) == kRowRangeViolPastRange);  // needed=37 > 36
	}
	// 反向用例：mgc=0 = 不派发任何组 = **恒不违例**（哪怕区间是 0）
	{
		auto in = rowClean();
		in.meshGroupCount = 0u;
		in.rangeCount = 0u;
		CHECK(shadowRowRangeViolations(in) == kRowRangeViolNone);
	}
	// object_oob：上界是**开区间**（capacity−1 合法，capacity 违例）
	{
		auto in = rowClean();
		in.objectIndex = 4095u;
		CHECK(shadowRowRangeViolations(in) == kRowRangeViolNone);
		in.objectIndex = 4096u;
		in.rangeCount = 0u;  // 区间条目不存在，调用方给 0（接线口径 → auditShadowMdiRows）
		CHECK(shadowRowRangeViolations(in)
		    == (kRowRangeViolObjectOob | kRowRangeViolPastRange));  // 要簇的行两条同响
		in.meshGroupCount = 0u;  // 不派发的行只响 object_oob 一条
		CHECK(shadowRowRangeViolations(in) == kRowRangeViolObjectOob);
	}
	// 🔴 64 位不回绕（假阴防线）：cstart=UINT32_MAX + 1 簇 = needed 2^32，任何 32 位
	// rangeCount 都装不下 ⟹ 必响。32 位算术在这里回绕成 0 = "越界"读成"没越界"。
	{
		auto in = rowClean();
		in.clusterStart = 0xFFFFFFFFu;
		in.meshGroupCount = 2u;
		in.rangeCount = 0xFFFFFFFFu;
		CHECK(shadowRowRangeNeeded(0xFFFFFFFFu, 2u) == 4294967296ull);
		CHECK(shadowRowRangeViolations(in) == kRowRangeViolPastRange);
	}
}

/**
 * ⑫ 债-143 位段 → 文本：两条各有名字，且 `cluster_past_range` **必须与 GPU P2 同名**
 * （同源对账 = 两侧 grep 同一个词；改名 = 拆桥，这条断言当场红）。
 */
TEST_CASE("⑫ describeShadowRowRangeViolations 两条各有名字且与 GPU P2 同名") {
	CHECK(describeShadowRowRangeViolations(kRowRangeViolNone) == "none");
	CHECK(describeShadowRowRangeViolations(kRowRangeViolObjectOob) == "object_index_oob");
	CHECK(describeShadowRowRangeViolations(kRowRangeViolPastRange) == "cluster_past_range");
	CHECK(describeShadowRowRangeViolations(kRowRangeViolObjectOob | kRowRangeViolPastRange)
	    == "object_index_oob|cluster_past_range");
	CHECK(describeShadowRowRangeViolations(1u << 30u) != "none");
	for (uint32_t bit : {1u << 0, 1u << 1})
		CHECK(describeShadowRowRangeViolations(bit) != "unknown_bits");
	// 与 GPU 侧的桥：P2 的短串必须与本断言的完全同名（跨 describe 函数对账，出处独立）
	CHECK(describeShadowRowRangeViolations(kRowRangeViolPastRange)
	    == describeMeshChainViolations(kMeshViolClusterPastRange));
}

}  // namespace RGL
