module;
#include <doctest/doctest.h>
#include <cstdint>
#include <cstring>
#include <initializer_list>  // modules 坑：range-for 的花括号列表要真 STL 头（只 include doctest 不够）

/**
 * test_gpu_poison —— 债-140 H3 毒化实验的**纯函数面**（rlbase，零设备依赖）。
 *
 * 被测对象 = `rgl/diagnostics.cppm` + `diagnostics_impl.cppm` 里新增的四件：
 *   `gpuPoisonWordAt` / `fillGpuPoison` / `parsePoisonScope` / `poisonActiveFor`
 * 判据 = 「需不需要一个 VkDevice 才能跑」= 不需要 ⟹ 纯 CPU 套件（不进 `TEST_SUITE("gpu")`）。
 *
 * ## 为什么值得测（实机读数替代不了的那部分）
 * 毒化实验的全部效力都压在一条前提上：**毒值必须真的铺进去、且必须非零**。
 * 这条前提在实机上失效时的表现是「fault 地址纹丝不动还是 0x0」——而那**正是实验的负读数**。
 * 也就是说：填充逻辑坏掉 ⟹ 实验必然"得到负读数" ⟹ 我们会把实现 bug 读成一条排除结论。
 * 这是最贵的一种假负：它不报错、不崩溃，只是让一次判决手段白跑（还烧掉一轮 GPU 复现）。
 * 所以这里钉的不是"代码写得好不好"，而是**这条实验有没有判别力**。
 *
 * ## 🔴 本文件最重要的一条 = 用例 6
 * `poisonActiveFor` 门关时三个目标全 false —— 这是「门关 = 逐位不变」（出厂锚回归的前提）
 * 唯一的机器判据。截图逐位只能证明"这一次跑没变"，证不了"门没开时那条 if 根本不进"；
 * 而一旦有人把判据写成只看 scope 不看门，出厂锚会在**门也关着**时照样全绿（因为默认
 * scope 是 "all"，只看 scope 就恒真）——只有用例 6 的"门关 + scope=heap"这个组合能抓。
 *
 * ## 变异红线声明（**2026-10-21 实测**；harness = `/home/robin/program/ab/poison/mutate_poison.py`，
 * 原文读数 = 同目录 `mutate_results.txt`。本文件共 **6** 条 TEST_CASE，编号 ①–⑥ 对应用例顺序）
 * 方法学四条防线（skill `rgl-test-discipline`）：备份/还原用 `shutil.copyfile`（**不带 mtime**）
 * + 还原后 `touch`；显式 `cmake --build build --target rgl_tests`（tests/ 是 EXCLUDE_FROM_ALL，
 * `just build` 不编它）并核对**构建自身**返回码（构建红 ≠ 判据红）；每条还原后重跑基线断言全绿；
 * 🔴 禁 `git checkout`/`git stash`（工作树有本批未提交的实现体，会被一起卷走）。
 * harness 另加两条**本批现场烧出来**的防线：
 *   ⑴ **逐用例分进程 + 判退出码**。`--abort-after` 图快的代价是"中止当前用例后，同一次调用里
 *      后面的用例根本不跑"⟹ "没跑"被读成"没红"（实测把 M1 的 ①③ 误报成只有 ①，差点把红线
 *      声明写错）；而几十万条失败明细又会把 doctest 的汇总行挤出缓冲 ⟹ 退出码才对体积免疫。
 *   ⑵ **启动时自愈遗留 `.bak`**。后台任务被环境回收时源码正带着变异（实测连踩两次），
 *      下一轮会把"变异态"量成"基线红"，症状是"测试坏了"，真实现场残留在上一次。
 *
 *   M1 `gpuPoisonWordAt` 去掉 `/4u` → 红 **①②③**（比预想多中一条：② 的逐字期望 `base + w`
 *      是**独立出处**、不跟着变异一起歪 ⟹ 这正是"期望值不抄被测码"买到的东西）
 *   M2 删掉回绕守卫（`return word;`）→ 红 **①**
 *   M3 基准字改成 `0u`（"填 0 也算填"）→ 红 **①②③**（全库最坏的一种毒：与未写内存同形）
 *   M4 尾部内层循环不查界（`i < 4`）→ 红 **③**（越界写，canary 字节响）
 *   M5 尾部整个不填（只按整字）→ 红 **③**（留未毒洞 = 判据盲区）
 *   M6 删掉 `bytes == 0` 早退 → 🔴 **实测不红，且这条是故意设计的自我证伪**：结论是那个早退
 *      **构造上冗余**（`for (off=0; off<0)` 本就不进）⟹ 它是可读性护栏、不是判据，**不配红线
 *      声明**。留档而不是删掉，既拦后来人问"这条怎么没用例"，也拦有人误以为它被验过。
 *   M7 未知名被静默接受（把 `"heaps"` 当 heap 认）→ 红 **④**
 *   M8 删掉空 token 检查 → 红 **④**。⚠ 读数细节：M8 下 `"heap,"` 仍会因"未知名"报错，
 *      红的是**消息可归因**那半条 ⟹ 两种拼错必须能从消息里分开，否则修法靠猜。
 *   M9 🔴 `poisonActiveFor` 只看 scope 不看门 → 红 **⑥**（本文件主判据；**出厂锚抓不到它**：
 *      默认 scope = "all"，那种变异在门关态恒真 ⟹ 只有"门关 + scope=heap"这个组合能抓）
 *   M10 `poisonActiveFor` 的 switch 把 rowTable 接到 heap → 红 **⑥**
 */
export module test_gpu_poison;

import rlstd;
import diagnostics;
import test_support;  // Lcg（写死种子，禁真随机）

namespace RGL {
namespace {

/** 毒值基准字的**期望**小端字节序（出处 = 契约"填 base+idx 并按小端落盘"，不是抄被测码）。 */
constexpr std::byte kPoisonBytesLE[4]{std::byte{0xEF}, std::byte{0xBE}, std::byte{0xAD},
	std::byte{0xDE}};

/**
 * 回绕点的四字组下标 = `0x1'0000'0000 − 0xDEADBEEF` = **0x2152'4111**（字节偏移
 * 0x8549'0444 ≈ 2.08 GiB）。🔴 这个数**算出来抄进来，别手推**——本文件第一版手推成
 * 0x2154'1111，实测 `base + 它 = 0x1D000` 根本不是 0，于是"守卫生效"那条断言直接红，
 * 而红的原因是锚点自己错不是代码错（skill「断言要照源码写、期望值要有出处」的现场版）。
 */
constexpr uint64_t kWrapWordIndex = 0x100000000ull - kGpuPoisonBaseWord;

/**
 * 单例守卫：`poisonActiveFor` 读进程级单例，用例改完必须还原，否则**同进程后续用例**
 * （test_scene_config / test_gate_lint 都读它）看到的是被改过的门。
 * REQUIRE 失败会抛栈展开 ⟹ 析构里还原，比手写尾行可靠。
 */
struct SingletonGuard {
	DiagnosticsConfig saved;
	SingletonGuard() : saved(diagnostics()) {}
	~SingletonGuard() { setDiagnostics(saved); }
};

/** 读第 i 个字节（测试自己的辅助，独立判据 = 直接下标）。 */
std::byte at(const std::vector<std::byte>& buf, size_t i) { return buf[i]; }

/** 按小端读 buf 中 off 起的 4 字节（off+4 必须在界内）。 */
uint32_t readWord(const std::vector<std::byte>& buf, size_t off) {
	return static_cast<uint32_t>(at(buf, off)) | (static_cast<uint32_t>(at(buf, off + 1)) << 8) |
		(static_cast<uint32_t>(at(buf, off + 2)) << 16) |
		(static_cast<uint32_t>(at(buf, off + 3)) << 24);
}

/**
 * 第 i 个字节**应该是**什么（期望值的独立出处 = 契约"字 = 基准字 + i/4，按小端落盘"，
 * 不调用被测的 gpuPoisonWordAt ⟹ 它写歪了这里不会跟着歪）。
 */
std::byte poisonByteAt(size_t i) {
	const uint32_t word = kGpuPoisonBaseWord + static_cast<uint32_t>(i / 4);
	return static_cast<std::byte>((word >> (8u * (i % 4))) & 0xFFu);
}

}  // namespace

TEST_CASE("① gpuPoisonWordAt = 基准字 + 四字组下标，且恒非零（含回绕点）") {
	// 阳性锚点：逐偏移的期望值 = 手算常量（契约 = base + off/4）。
	// 🔴 只钉 off=0 是不够的：一个 `return kGpuPoisonBaseWord;`（常量毒值）在 off=0 上
	// 完全正确，而它丢掉了本实验的**归因能力**（地址反解不出是哪个四字组）。
	CHECK(gpuPoisonWordAt(0) == 0xDEADBEEFu);
	CHECK(gpuPoisonWordAt(4) == 0xDEADBEEFu + 1u);
	CHECK(gpuPoisonWordAt(24) == 0xDEADBEEFu + 6u);       // 行表一行 = 24B = 6 个四字组
	CHECK(gpuPoisonWordAt(32) == 0xDEADBEEFu + 8u);       // 描述符堆步长 32B 的下一槽
	CHECK(gpuPoisonWordAt(1000) == 0xDEADBEEFu + 250u);
	// 非 4 的倍数向下取整到四字组（调用点只会传 4 的倍数，但函数是全定义域纯函数，
	// 语义必须写死，否则"偏移算术在别处被复用"时行为未知）。
	CHECK(gpuPoisonWordAt(5) == gpuPoisonWordAt(4));
	CHECK(gpuPoisonWordAt(7) == gpuPoisonWordAt(4));

	// 🔴 恒非零契约（本实验的命门：0 与"从未写过的内存"同形 = 判据静默失效）。
	// 回绕点是唯一能让 `base + idx` 归零的输入，守卫就为它存在。
	CHECK(gpuPoisonWordAt(kWrapWordIndex * 4) != 0u);
	CHECK(gpuPoisonWordAt(kWrapWordIndex * 4) == 0xDEADBEEFu);  // 改口回基准字
	CHECK(gpuPoisonWordAt(kWrapWordIndex * 4 + 4) != 0u);       // 回绕点之后也非零

	// 可归因性：真实分配尺寸内**逐四字组互不相同**，否则"地址反解偏移"会多解。
	// 判据用「严格 +1 递增 + 除 off=0 外不等于基准字」代替排序去重：两者等价地证明单射，
	// 但前者 O(n) 无临时容器（本套件是毫秒级纯 CPU 套件，不该为一条性质排 64MB）。
	// 上界 16KB = 4096 个四字组。**为什么不是"扫到最大对象那么大"**：这条性质是逐字一致的
	// （w(off) 只依赖 off/4），扫 4096 个与扫 26 万个的判据强度相同，而后者在变异态下会打印
	// 几十万条失败明细、把 doctest 的汇总行都挤掉（实测踩过：harness 读不到汇总 → 误报）。
	// 16KB 已覆盖"一行 24B / 一槽 64B"的真实步长几百倍，够暴露任何取整或步长错。
	constexpr uint64_t kSpan = 16ull << 10;
	for (uint64_t off = 4; off < kSpan; off += 4) {
		INFO("偏移 ", off);
		CHECK(gpuPoisonWordAt(off) == gpuPoisonWordAt(off - 4) + 1u);  // 严格递增 ⟹ 单射
		CHECK(gpuPoisonWordAt(off) != gpuPoisonWordAt(0));             // 不与 off=0 碰撞
		CHECK(gpuPoisonWordAt(off) != 0u);
	}
	// 反解公式本身（读数时要拿它把 fault 地址换回偏移，写错方向就归因错）：
	CHECK((gpuPoisonWordAt(24 * 999) - kGpuPoisonBaseWord) * 4 == 24 * 999);
	CHECK((gpuPoisonWordAt(32 * 77) - kGpuPoisonBaseWord) * 4 == 32 * 77);
}

TEST_CASE("② fillGpuPoison 逐字节等于显式小端的毒值字（锚点先自证）") {
	constexpr size_t kBytes = 4 * 6;  // 6 个四字组 = 行表 4 行不到，够覆盖递增
	std::vector<std::byte> buf(kBytes, std::byte{0x00});
	// 🔴 锚点自证（skill 的教训：铺完立刻回读逐字节 REQUIRE，否则"锚点失效"与
	// "代码是对的"长得一模一样）——铺 0 之后必须真是 0，才轮到被测动作。
	for (size_t i = 0; i < kBytes; ++i)
		REQUIRE(at(buf, i) == std::byte{0x00});

	fillGpuPoison(buf.data(), kBytes);

	// 前 4 字节 = 基准字的小端形态（逐字节钉，不用 memcpy 比较：字节序本身就是要钉的东西）
	for (size_t i = 0; i < 4; ++i)
		CHECK(at(buf, i) == kPoisonBytesLE[i]);
	// 逐四字组递增，且每个字都等于 gpuPoisonWordAt(该偏移)
	for (size_t w = 0; w < kBytes / 4; ++w) {
		const uint32_t expect = kGpuPoisonBaseWord + static_cast<uint32_t>(w);
		const uint32_t got = static_cast<uint32_t>(at(buf, w * 4)) |
			(static_cast<uint32_t>(at(buf, w * 4 + 1)) << 8) |
			(static_cast<uint32_t>(at(buf, w * 4 + 2)) << 16) |
			(static_cast<uint32_t>(at(buf, w * 4 + 3)) << 24);
		INFO("四字组 ", w);
		CHECK(got == expect);
	}
	// 🔴 "毒面不留洞"的**正确判据是字级不是字节级**：`base + idx` 会产出 0xDEADBF00 这种
	// 含 0x00 字节的字（idx=0x11 起就有），所以"逐字节非零"是个假不变量（本文件第一版
	// 拿它当锚，Lcg 那条用例立刻红了）。字级判据同时更强也更贴事实：GPU 的 Load 至少是
	// 4 字节对齐 ⟹ 只要每个四字组等于期望值，就不存在"读到未毒区"的形态。
	for (size_t w = 0; w < kBytes / 4; ++w) {
		INFO("四字组 ", w);
		CHECK(readWord(buf, w * 4) == gpuPoisonWordAt(w * 4));
	}
	// 首字必须等于基准字（肉眼在 hexdump 里认毒的锚）
	CHECK(readWord(buf, 0) == kGpuPoisonBaseWord);

	// 反向用例：0 长度不该碰任何字节（早退分支的"不该触发"用例）
	std::vector<std::byte> keep(8, std::byte{0xAA});
	fillGpuPoison(keep.data(), 0);
	for (size_t i = 0; i < keep.size(); ++i) {
		INFO("0 长度越界改了字节 ", i);
		CHECK(at(keep, i) == std::byte{0xAA});
	}
	// nullptr 不崩（调用点的 getMappedData() 理论上可能为空，判据在别处出声，这里只保证不炸）
	fillGpuPoison(nullptr, 16);
	fillGpuPoison(nullptr, 0);
}

TEST_CASE("③ 非 4 倍数的尾部：不留 0 洞、不越界写一字节") {
	// 12 字节缓冲，只毒前 6 字节（= 1.5 个四字组）。两个失效形态都要响：
	//   · 尾部整个不填 → 字节 4、5 留 0 = 与未写内存同形（判据盲区）
	//   · 尾部按整字填 → 越界写 2 字节（canary 响）
	std::vector<std::byte> buf(12, std::byte{0x00});
	for (size_t i = 0; i < buf.size(); ++i)
		REQUIRE(at(buf, i) == std::byte{0x00});  // 锚点自证

	fillGpuPoison(buf.data(), 6);

	// 第一个四字组完整
	for (size_t i = 0; i < 4; ++i)
		CHECK(at(buf, i) == kPoisonBytesLE[i]);
	// 尾部 2 字节 = 第二个四字组的低 2 字节（小端），且**非零**
	const uint32_t w1 = kGpuPoisonBaseWord + 1u;
	CHECK(at(buf, 4) == static_cast<std::byte>(w1 & 0xFFu));
	CHECK(at(buf, 5) == static_cast<std::byte>((w1 >> 8) & 0xFFu));
	CHECK(at(buf, 4) != std::byte{0x00});
	CHECK(at(buf, 5) != std::byte{0x00});
	// 🔴 越界守卫：字节 6 起必须一个没动
	for (size_t i = 6; i < buf.size(); ++i) {
		INFO("越界写了字节 ", i);
		CHECK(at(buf, i) == std::byte{0x00});
	}

	// 1 字节的极端形态（真实分配不会这么小，但函数是全定义域纯函数）
	std::vector<std::byte> one(4, std::byte{0x00});
	fillGpuPoison(one.data(), 1);
	CHECK(at(one, 0) == std::byte{0xEF});
	CHECK(at(one, 1) == std::byte{0x00});

	// Lcg 写死种子：随机长度填充后，区内无 0、区外全 0
	testing::Lcg rng(0x5EED140Bu);
	for (int t = 0; t < 64; ++t) {
		const size_t n = static_cast<size_t>(rng.uniformInt(1, 200));
		std::vector<std::byte> b(n + 8, std::byte{0x00});
		fillGpuPoison(b.data(), n);
		for (size_t i = 0; i < n; ++i)  // 区内不留洞：逐字节等于期望毒值（含尾部不足一字）
			REQUIRE(at(b, i) == poisonByteAt(i));
		for (size_t i = n; i < b.size(); ++i)
			REQUIRE(at(b, i) == std::byte{0x00});  // 区外不越界
	}
}

TEST_CASE("④ parsePoisonScope：接受集、空格容忍、fail-fast 可归因") {
	// 三个单名 + all
	CHECK(parsePoisonScope("heap")->heap);
	CHECK_FALSE(parsePoisonScope("heap")->rowTable);
	CHECK_FALSE(parsePoisonScope("heap")->cullChain);
	CHECK(parsePoisonScope("rowtable")->rowTable);
	CHECK_FALSE(parsePoisonScope("rowtable")->heap);
	CHECK(parsePoisonScope("cull")->cullChain);
	CHECK_FALSE(parsePoisonScope("cull")->heap);

	// all ≡ 三个单名的并集（"all" 不是第四个目标，是三个的缩写）
	const auto all = parsePoisonScope("all");
	REQUIRE(all.has_value());
	CHECK(all->heap);
	CHECK(all->rowTable);
	CHECK(all->cullChain);

	// 逗号子集 + 空格容忍
	const auto sub = parsePoisonScope("heap, cull");
	REQUIRE(sub.has_value());
	CHECK(sub->heap);
	CHECK_FALSE(sub->rowTable);
	CHECK(sub->cullChain);
	CHECK(parsePoisonScope(" heap , rowtable ")->rowTable);

	// 🔴 fail-fast 反向用例集：每一条都必须**返回 unexpected**，而不是静默给一个可用 scope。
	// 失效形态：未知名被吞掉 ⟹ 少毒一块，而"没毒到"与"毒到了没炸"在 fault 读数上同形。
	for (std::string_view bad : {std::string_view{""}, std::string_view{"heaps"},
			 std::string_view{"row_table"}, std::string_view{"HEAP"}, std::string_view{"heap,"},
			 std::string_view{",heap"}, std::string_view{"heap,,cull"}, std::string_view{"heap;rowtable"},
			 std::string_view{"heap cull"}, std::string_view{"cullchain"}}) {
		INFO("非法 spec = \"", bad, "\"");
		auto r = parsePoisonScope(bad);
		CHECK_FALSE(r.has_value());
		// 可归因：错误消息必须带上 offending 内容（只说"解析失败"等于让人重跑一遍猜）
		if (!r)
			CHECK(r.error().message.find(bad.empty() ? "empty" : std::string(bad)) !=
				std::string::npos);
	}
}

TEST_CASE("⑤ 毒化目标枚举与 scope 字段一一对应（新增目标漏接必须红）") {
	// 这条钉的是"scope 的三个布尔与 PoisonTarget 的三个枚举值是同一件事"。
	// 三个名字各写一遍（heap/rowtable/cull）而枚举有第四个值时，poisonActiveFor 的
	// 穷尽 switch 会编译失败（无 default），但**字符串侧漏加一个分支不会**——它会走到
	// "未知名 = 报错"，于是新目标永远毒不了而没人回来加这里。本用例把映射钉成断言。
	const auto s = parsePoisonScope("heap,rowtable,cull");
	REQUIRE(s.has_value());
	CHECK(s->heap);
	CHECK(s->rowTable);
	CHECK(s->cullChain);
	// 三个名字必须互不相同地生效（一个把两个名字映射到同一布尔的复制粘贴错）
	CHECK_FALSE(parsePoisonScope("heap")->rowTable);
	CHECK_FALSE(parsePoisonScope("heap")->cullChain);
	CHECK_FALSE(parsePoisonScope("rowtable")->cullChain);
	CHECK_FALSE(parsePoisonScope("rowtable")->heap);
	CHECK_FALSE(parsePoisonScope("cull")->heap);
	CHECK_FALSE(parsePoisonScope("cull")->rowTable);
}

TEST_CASE("⑥ 🔴 poisonActiveFor = 门 ∧ scope 的合取（门关 = 三个目标全 false）") {
	SingletonGuard guard;  // 改单例必须还原（同进程别的用例也读它）

	// ── 门关：scope 写什么都必须全 false。这是「门关 = 逐位不变」的机器判据。
	// 🔴 单靠"默认配置下出厂锚逐位"抓不到"只看 scope 不看门"的变异：默认 scope = "all"，
	// 那种变异在门关态会恒真 ⟹ 三处分配点全部铺毒 ⟹ 出厂锚红。但**反过来**（默认 scope
	// 被谁改成 ""）就全绿了。所以这里显式喂 "all" 和 "heap" 两种 scope 各钉一遍。
	for (std::string_view scope : {std::string_view{"all"}, std::string_view{"heap"},
			 std::string_view{"rowtable"}, std::string_view{"cull"}}) {
		INFO("门关 + scope = \"", scope, "\"");
		setDiagnostics(DiagnosticsConfig{.poisonGpuMem = false, .poisonScope = std::string(scope)});
		CHECK_FALSE(poisonActiveFor(PoisonTarget::heap));
		CHECK_FALSE(poisonActiveFor(PoisonTarget::rowTable));
		CHECK_FALSE(poisonActiveFor(PoisonTarget::cullChain));
	}

	// ── 门开 + all：三个都毒
	setDiagnostics(DiagnosticsConfig{.poisonGpuMem = true, .poisonScope = "all"});
	CHECK(poisonActiveFor(PoisonTarget::heap));
	CHECK(poisonActiveFor(PoisonTarget::rowTable));
	CHECK(poisonActiveFor(PoisonTarget::cullChain));

	// ── 门开 + 单名：只有那一个毒（分 buffer 归因的全部依赖面）
	setDiagnostics(DiagnosticsConfig{.poisonGpuMem = true, .poisonScope = "heap"});
	CHECK(poisonActiveFor(PoisonTarget::heap));
	CHECK_FALSE(poisonActiveFor(PoisonTarget::rowTable));
	CHECK_FALSE(poisonActiveFor(PoisonTarget::cullChain));

	setDiagnostics(DiagnosticsConfig{.poisonGpuMem = true, .poisonScope = "rowtable"});
	CHECK_FALSE(poisonActiveFor(PoisonTarget::heap));
	CHECK(poisonActiveFor(PoisonTarget::rowTable));
	CHECK_FALSE(poisonActiveFor(PoisonTarget::cullChain));

	setDiagnostics(DiagnosticsConfig{.poisonGpuMem = true, .poisonScope = "cull"});
	CHECK_FALSE(poisonActiveFor(PoisonTarget::heap));
	CHECK_FALSE(poisonActiveFor(PoisonTarget::rowTable));
	CHECK(poisonActiveFor(PoisonTarget::cullChain));

	// ── 门开 + 非法 scope：按"不毒"处理（宁可实验无效，不要静默毒错对象）。
	// 正常路径下启动期已 fail-fast，走到这里说明单例被绕过赋值 ⟹ 出声 + 全 false。
	setDiagnostics(DiagnosticsConfig{.poisonGpuMem = true, .poisonScope = "bogus"});
	CHECK_FALSE(poisonActiveFor(PoisonTarget::heap));
	CHECK_FALSE(poisonActiveFor(PoisonTarget::rowTable));
	CHECK_FALSE(poisonActiveFor(PoisonTarget::cullChain));
}

}  // namespace RGL
