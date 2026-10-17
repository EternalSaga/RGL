module;
#include <doctest/doctest.h>
#include <vulkan/vulkan.h>  // VkDrawMeshTasksIndirectCommandEXT（C 名；rlvk 只导出 vk:: 系，先例 test_mdi_drawindex:73）
#include <cstring>          // std::memcpy（行首 12B → Vk 结构）
#include <initializer_list>  // modules 坑：range-for 的花括号列表要真 STL 头（只 include doctest 不够）
#include <filesystem>        // 临时 lua 落盘（GMF 例外清单成员）
#include <fstream>

/**
 * test_mdi_rows —— ★M-0 + ★M-1a（docs/mdi_plan.md §19.3 / §19.5）：真 MDI 阴影段的行表构建器
 * 与它的尺寸/偏移算术（纯 CPU 套件）。
 *
 * 被测对象 = `rgl/mdi_rows.cppm` + `rgl/mdi_rows_impl.cppm`（rlbase）。**不是** `TEST_SUITE("gpu")`：
 * 构建器只读一段候选 span，一个设备都不碰（判据 = 「需不需要一个 VkDevice 才能跑」）。
 *
 * ## 为什么值得测（截图看不出来的那类）
 * ★M-1 的主判据是「MDI 形态 vs 逐条 draw 形态终图**逐位**相同」，而那条判据的前提全在本模块：
 *   · **行序 = 候选序**：一旦有人「顺手压紧」不可见行，depth-only 图集的 min 语义就不再与
 *     逐条 draw 等价 ⟹ A3 红，而红的原因在两个文件之外（最难查的一种红）；
 *   · **前 12B 恰是 `VkDrawMeshTasksIndirectCommandEXT`**：GPU 按裸字节读，字段错位 = 把
 *     objectIndex 当 groupCountX 用 = 发几十万倍 task 或一个都不发；
 *   · **stride 合法性（VUID-07088）**：不合法是 validation error / 未定义行为，不是"画面差一点"。
 * 这三条在门关态的截图里**一条都看不见**（本批零消费者 ⟹ 图必然不变），只能在这里钉。
 *
 * ## 不变量 1/3/4 的反向用例走 `validateMdiShadowRows`，不走构建器（可达性写清楚）
 * 构建器自己产出的行表**按构造**满足 1/3/4（同一趟循环写行与锚）⟹ 那三条在构建器入口上是
 * **哨兵**（防的是"以后有人改坏构建器"，变异与判据短路让它们各自变红，逐条读数见下方红线表）。
 * 要让它们有**可达的**反向用例，激励必须来自外部：行表 + 候选清单两个入参都是调用方给的
 * （★M-4 的 GPU 行表对账就是这个形态），于是导出的 `validateMdiShadowRows` 是它们真正的入口。
 * 不变量 2 相反——它是 **输入**契约，只有构建器判。
 *
 * ## 🔴 ★M-3T-c 改判据顺序后的两条**新哨兵**（validate 路径打不到，别把它们读成"没测"）
 * `validateMdiShadowRows` 的顺序是 行数 → 逐行对账（不变量 4）→ 聚合锚。★M-3T-c 起
 * `rowFor` 的 gcX 恒 ∈ {0,1}，而不变量 4 逐元素比 gcX ⟹ 凡通过逐行对账的表，
 * 「可见候选发出空行」与「Σ gcX ≠ rowCount − emptyRows（3b）」**结构上不可达**：
 *   · 外部表把某行 gcX 改成 2 ⟹ 先被不变量 4 点名（错误串逐字打印 got/want 的 gcX，可归因）；
 *   · 3b 的专属信号在**构建器路径**：`rowFor` 是构建器与对账器共用的唯一映射公式，
 *     「有人把 ×2 乘回 gcX」这类变异下逐行对账两边一起变、抓不到，只有 3b 红（红线表 ⑳ 实测）。
 * 「空行数 vs 不可见候选数」那条分支在**外部表**路径上同理成哨兵；但它**不是纯摆设**——
 * `rowFor` 级变异（红线表 ④：gcX 恒 1）让两侧一起变、逐行对账与 3b 全部静默，聚合锚里
 * 唯一出声的就是它（④ 的 10 条读数里有它一份）。
 *
 * ## 🔴 数值纪律：`kMdiShadowRowStride`(=24) 与本文件任何用例的行数/偏移都不相等
 * spike §6.3 的 M4 教训（`test_mdi_drawindex.cppm` 文件头记着全文）：两个数相等 ⟹ 参数序写反
 * 也测不出东西。本文件的行数 = 5 / 4 / 3 / 1、候选数 = 6 / 5、clusterStart = 3/11/13/17/1001/7、
 * 字节偏移 = 0/4/8/12/16/20 ——**没有一个是 24**（12 只作为 VUID 下界与 `sizeof(Vk…)` 出现，
 * 不作为行数/偏移）。
 * 「stride 与这些数都不相等」本身也钉成断言（`strideDistinctFromCaseNumbers`），凑数即红。
 *
 * ## 变异红线声明（★M-3T-c 后**全部重新实测** 2026-10-13；编号 = harness 的 M01…M24 / N1…N15，
 * 旧「十三条」数字全部作废——判据面随语义改判据重写过，旧读数对新判据面没有意义。
 * 🔴 **2026-10-14 追加 ㉔-㉗ 四条并重测 ⑥**（§19.6 挂号收账，见下方「2026-10-14 追加」小节））
 * 方法学四条防线：备份/还原用 `shutil.copyfile`（**不带 mtime**，抬 mtime 到未来时刻会让 clang
 * 跳过重编、读到半写 BMI 崩出假构建红）+ 显式 `cmake --build build --target rgl_tests`（tests/ 是
 * EXCLUDE_FROM_ALL，`just build` 不编它）并核对**构建自身**的返回码（构建红 ≠ 判据红，那种读数
 * 作废）、每条还原后重跑全量基线断言全绿。🔴 **禁 `git checkout`/`git stash`**：工作树里有主
 * agent 未提交的改动，任何 git 还原命令都会把它卷走。
 * 下面「红 N 条」= 该变异下失败的 TEST_CASE 条数（本文件共 **18** 条 TEST_CASE；「+发射 K」=
 * 同时打到 `test_mdi_shadow_emit.cppm`；「+设备 K」= 同时打到 `test_meshlet_shadow_args.cppm`
 * 的设备自证用例——同源 pair 是 CPU/GPU 两侧的共用产地）。
 *
 *   ① 压紧不可见行（`out.rows.push_back(row)` 外套 `if (row.groupCountX != 0u)`）→ 红 **10** 条
 *      +发射 5（正向锚点 + 行序 + 不可见行身份 + Lcg + 前 12B 对账 + inv2 边界 + 三条反向表 +
 *      聚合锚诚实）
 *   ② 不可见行整行清零（`rowFor` 里不可见即 `return row`，身份字段一起归零）→ 红 **7** 条 +发射 3。
 *      🔴 **不含任何反向用例**：整行清零仍留空行数 ⟹ 四个聚合锚一位不变，只有逐字段判据
 *      （不变量 4）抓得到
 *   ③ 身份字段错位（`row.objectIndex = c.clusterStart`）→ 红 **5** 条 +发射 2（正向锚点 + 行序 +
 *      不可见行身份 + Lcg + 前 12B 对账）
 *   ④ 🔴 **可见性判据弱化 = gcX 恒 1**（`rowFor` 的 `c.visible ? 1u : 0u` → `1u`，剔除失效）→ 红
 *      **10** 条 +发射 5。旧红线「`c.visible ? c.taskCount : 0u` → 恒 `c.taskCount`」的判据在新语义
 *      下**不存在了**——gcX 表达式里已没有可弱化的 mesh 组数因子，这条改钉「恒 1 绕过 0」。
 *      两侧共用 `rowFor` ⟹ 逐行对账静默、3b 等式仍成立（Σ gcX = 行数 − 空行数），聚合锚里
 *      **唯一出声的是「空行数 vs 不可见候选数」那条分支**（它不是纯哨兵的实测证据）
 *   ⑤ `rowCount` 锚判据短路（`&& false`）→ 红 **2** 条（全部坏形态 + 两两不同）
 *   ⑥ 🔴 行数判据**弱化**（`rows.size() != candidateCount` → `>`，只挡多行、放过少行）→ 红 **1**
 *      条（全部坏形态），且**全部来自形态 A′**（形态 A 漏行时姊妹判据 `rowCount != rows.size()` 出声
 *      且归因正确）。⟹ 只有 `badRowDroppedHonestAnchors`（删**最后一行** + 四个锚按剩下的行**重算** +
 *      补进来的候选可见）把这条弱化变成**归因错**——同 `test_meshlet_ranges` ⑫ 同型的「弱化不只测
 *      删除」缺口。
 *      🔴 **2026-10-14 重测：2 → 1**，不是检测力变弱，是**失效形态变了**：本批给 `checkRowOrder`
 *      加了显式前置（§19.6 挂号），少行的表如今在那里出声而不是**静默通过**，于是「两两不同」那条
 *      用例不再因 `REQUIRE(has_value)` 红（它只在错误串彼此相同或压根没错误串时红）。红 1 仍是红。
 *   ⑦ 空行数锚判据短路 → 红 **2** 条（同 ⑤）
 *   ⑧ 逐行对账判据弱化（整条比较 `|| true` 恒 continue）→ 红 **4** 条（全部坏形态 + 两两不同 +
 *      两条新哨兵用例）。⚠ 首版写成「去掉 `!empty[i] ||` 短路」是**构建红**（gcX 越界分支变不可达
 *      ⟹ -Werror unreachable），读数作废；改成 `(...) || true` 形态才拿到判据红。
 *      ⚠ **2026-10-14 未重测**（两次注入都是构建红：`... || true` 撞 `-Wlogical-op-parentheses`、
 *      `... || i == i` 撞 `-Wtautological-compare`），读数沿用 2026-10-13。它不可能被本批改动影响：
 *      前置哨兵只在不变量 1 被弱化时可达，而 ⑧ 不动不变量 1（同日抽测 ⑤⑦㉓ 三条读数逐字不变）
 *   ⑨ 构建后按 `objectIndex` 排序（"看着更像序"）→ 红 **10** 条 +发射 1（同 ①）
 *   ⑩ 🔴 **构建器 sumMesh 丢可见性门**（累加式 `row.groupCountX == 0u ? 0u : row.meshGroupCount`
 *      → 恒 `row.meshGroupCount`）→ 红 **10** 条 +发射 5（同 ①）。这是 ★M-3T-c 把「Σ 只加可见行」
 *      从 gcX 迁到 meshGroupCount 之后的**新形态**（旧红线 ⑩「Σ gcX 只加可见行」的等价物）
 *   ⑪ 配置解析键名打错（`"mdi_shadow"` → `"_mdi_shadow"`）→ 红 **1** 条（配置键用例的「显式命中」
 *      半边；探针必须取 **true** ≠ 默认 false 才抓得住——探针取默认同值时「键没读到」与「读到默认值」
 *      不可区分，该变异全绿）
 *   ⑫ 配置默认值翻成 true → 红 **1** 条（同一条配置用例，内含 4 处断言失败）
 *   ⑬ 不变量 2 判据短路（`visible && meshGroupCount == 0` → `&& false`）→ 红 **3** 条（inv2 只在
 *      构建器 + 全部坏形态 + 两两不同）
 *   ⑳ 🔴 **★M-3T-c 头号回归：×2 乘回 gcX**（`rowFor` 的 `c.visible ? 1u : 0u` → `? 2u : 0u`，
 *      一个 draw 两枚 task）→ 红 **11** 条 +发射 4。两侧共用 `rowFor` ⟹ 逐行对账静默，构建器
 *      自检的出声者**逐字是 3b**（错误串见「两条新哨兵」节）——**本表对 ★M-3T-c 最对症的一条**
 *   ㉑ **不变量 4 丢 meshGroupCount 比对面**（逐元素比较删掉 `meshGroupCount ==` 一项）→ 红 **3**
 *      条（全部坏形态 + 两两不同 + word[5] 专属用例）。这是 ★M-3T-c 把 word[5] 升为载荷后新增
 *      比对面的专属信号：没有「只换 word[5]」那条反向用例，此变异只剩聚合锚一条腿
 *   ㉒ **对账器 sumMesh 重算丢可见性门**（判据侧 `sumMesh += row.meshGroupCount` 全行求和）→ 红
 *      **10** 条 +发射 5（同 ⑩）。与 ⑩ 是同一判据的**两侧**：⑩ 谎报、㉒ 瞎判；㉒ 单独变异时
 *      「谎报成全行求和」的反向用例恰好被放行——构建侧与判据侧必须同纪律的实测理由
 *   ㉓ **不变量 3b 哨兵短路**（validate 的 `sumX != rows.size() − emptyRows` 判据 `&& false`）→
 *      红 **0** 条 = **存活**。这是**设计使然**，不是缺口：validate 先跑逐行对账，gcX≥2 的外部表
 *      永远走不到 3b（归因到不变量 4，见「两条新哨兵」节），3b 在外部 validate 路径上是纯哨兵。
 *      它的**专属信号是构建器自检路径** = ⑳（实测 11 条里含 3b 逐字出声）。组合变异 M24（3b 短路
 *      **且** ⑳ 的 ×2 乘回）实测仍红 **10** 条 +发射 5——per-row `gcX ≤ 1`（不变量 4 + 发射器
 *      闭式）独立兜住 task 数语义，哨兵短路不造成静默
 *
 * ### 2026-10-14 追加四条（§19.6 挂号收账：不变量 1 的 `<` 半边 + `checkRowOrder` 显式前置）
 * 这四条是**本批新写判据的实测**（harness = `/tmp/opencode/mutate_mdi_rows_extra{,2,3,4}.py`，
 * 四条防线齐；进场基线 sha256 `0af3451f29e8507c`，每条还原后逐字节复核 + 全量基线复绿）：
 *   ㉔ 🔴 **不变量 1 弱化到 `<`**（`!=` → `<`，只挡少行、**放过**多行）→ 红 **1** 条（全部坏形态）。
 *      这是 §19.6 点名「无人钉」的那半边：旧表 ①⑥⑨ 全是**少行**形态，`<` 漂移下它们照样红
 *      ⟹ 旧判据面对这条漂移**完全静默**。新增的 `badRowExtraHonestAnchors`（多一行 + 四个锚诚实）
 *      是唯一有牙的一条，needle 里带 `holds 6 rows but the candidate list holds 5` 两个数
 *      ⟹ 比较符被写反也红
 *   ㉕ **validate 顺序调换**（逐行对账先于不变量 1）→ 红 **1** 条。钉的是**前置本身**：多行的表
 *      在 `checkRowOrder` 的新前置处出声（错误串前缀 `mdi rows: precondition`，刻意不含
 *      `invariant` ⟹ 归因用例的 `mdi rows: invariant` 前缀判据与 needle 双双不中）
 *   ㉖ **前置哨兵短路**（`&& false`）→ 红 **0** 条 = **存活**，判定为**等价变异**（同 ㉓ 同族）：
 *      不变量 1 完好时该分支结构上不可达。它的价值不在"多一条判据"，见 ㉗
 *   ㉗ 🔴 **顺序调换 + 前置摘掉**（= 没有本批前置时 ㉕ 的真形态）→ 红 **2** 条（两条表用例），
 *      但**错误串归因是错的**，这才是本批前置的立项证据。实测原文：
 *      `invariant 4: row 5 holds {gcX 1, objectIndex 3, clusterStart 7, meshGroupCount 11} but
 *      candidate 5 maps to {gcX 0, objectIndex 137, clusterStart 529, meshGroupCount 0}`
 *      —— `137 / 529` 是**越候选尾读来的堆上邻接字节**（用例数据里没有这两个数），错误串把
 *      「行数不匹配」报成「行序不对 + 数值不等」。堆布局一变（换分配器 / 换用例顺序）它也可能
 *      **恰好相等而彻底静默**。⟹ 前置的作用是把这一格从「归因指错对象、且可能全绿」换成
 *      「确定性点名前置没跑」= §15.3 那条「归因错误比不归因更贵」的机器化
 *
 * ### ★M-1a 尺寸/偏移算术追加（harness 编号 N1-N4 / N14-N15，2026-10-13 **全部重测**）
 * 每条**同时**打到发射器与设备用例（设备侧用例名见 `tests/test_meshlet_shadow_args.cppm` 的文件头）：
 *   ⑭ **N1** `mdiShadowTailBytes` 把 `stride × (drawCount − 1)` 写成 `stride × drawCount`
 *      → 红 **2** 条（`闭式边界` + `Lcg 属性组二`）+ 设备侧 2 条。这正是「`stride × drawCount`
 *      对任何合法输入都 ≤ 容量 ⟹ VUID 判据被读成恒真」的那类写法。
 *   ⑮ **N2** `argsOffset = row × stride + 4`（字节偏移被挪而 `rowBase` 不动 = 两侧分家）
 *      → 红 **4** 条（同源 pair + 容量 + 闭式边界 + Lcg 组二）+ 发射 2 + 设备侧 **5** 条全红。
 *   ⑯ **N3** `rowBase = frameSlot × sliceRows`（丢掉 `firstRow` 项 = 分家的另一半形态）
 *      → 红 **2** 条（🔴 **只有**同源 pair 与 Lcg 组二：聚合量用例不碰 firstRow，容量用例也不碰）
 *      + 发射 2 + 设备侧 4 条。这条是「用例必须钉同源这件事本身」的实测理由。
 *   ⑰ **N4** `drawCount == 0` 的尾写成 `argsOffset + stride`（不回绕但多读一整行）
 *      → 红 **1** 条（`闭式边界`）+ 设备侧 1 条。
 *   ⑱ **N14** `mdiShadowArgsCapacityBytes` 漏乘 `slices`（恒取一份 slice）→ 红 **3** 条 + 设备侧 5 条。
 *      ⚠ 首版写成 `* 1u` ⟹ 形参 `slices` 变 unused → **-Werror 构建红**，读数作废；改成
 *      `* (slices * 0u + 1u)` 才拿到判据红（skill「谓词类变异要保留参数使用」的第 N 次命中）。
 *   ⑲ **N15** `mdiShadowSliceBytes` 把步长换成命令体 12（`stride` 与 `sizeof(Vk…)` 混用）
 *      → 红 **3** 条 + 设备侧 5 条。
 *
 * ### 两条**自我纠错**（首版期望值出处写错，靠变异才发现，已就地改对并留注）
 *   · 「容量少 1 字节必须报」首版写成 `fits(capacity − 1, …)` ⟹ **永不红**：容量按 stride 算而每行
 *     只读 12B，`capacity − 1 ≥ tail` 恒成立。判据本体是**末行尾**，于是钉成两条：
 *     `fits(tail)` 放行（spec 是 less than or equal）+ `fits(tail − 1)` 必报。
 *   · Lcg 组二里 `if (wantTail == wantCapacity)` 包住的反向断言是**永不执行的死代码**
 *     （尾 ≡ 12 (mod 24) 而容量 ≡ 0 (mod 24)）⟹ 那半边从来没牙，已改成无条件两条。
 *
 * ## 已知覆盖缺口（写出来，别让「全绿」被读成「全验过」）
 *   · 真产物对账（1591 候选 × 3 级联的 Σ meshGroupCount 闭式锚；★M-3T-c 起 Σ gcX 退化为
 *     「可见候选数」的平凡量，跨形态对账量换成了它）不在这里做：本批零消费者，没有真候选序列
 *     可喂（装配层不产它）。★M-1 接上后由启动日志常驻出声（同 `meshlet_lod` 批 B 的裁决）。
 *   · 「GPU 逐行读自己的 groupCountX」是设备级事实，spike 已实测（`per_draw_groupcount`），
 *     本文件只钉字节布局；跨层那一条 = ★M-1 判据 A3/A4。
 */
export module test_mdi_rows;

import rlstd;
import mdi_rows;         // MdiShadowRow / MdiShadowCandidate / MdiShadowRowTable / build / validate
import runtime_config;   // 配置键 meshlet.mdi_shadow 的解析契约
import test_support;     // Lcg（写死种子，禁真随机）

namespace RGL {
namespace {

/**
 * 正向锚点的候选序列（5 条：3 可见 2 不可见）。
 *
 * 手算期望（出处 = §19.2 D1 的行布局 + ★M-3T-c 的 task 化语义，不抄被测码）：
 *
 * | i | objectSlot | meshGroupCount | clusterStart | visible | ⟹ gcX |
 * |---|---|---|---|---|---|
 * | 0 | 7    | 6  | 3    | true  | 1 |
 * | 1 | 21   | 0  | 11   | false | 0 |
 * | 2 | 19   | 9  | 17   | true  | 1 |
 * | 3 | 1000 | 2  | 1001 | false | 0 |
 * | 4 | 3    | 11 | 7    | true  | 1 |
 *
 * 聚合锚：Σ gcX = **3**（每可见行恰一枚 task，恒等于 rowCount − emptyRows = 5 − 2，不变量 3b）；
 * sumMeshGroupCount = 6+9+11 = **26**（🔴 只算可见行；全行求和是 28，两者不等才有判别力）；
 * 空行数 = **2**；行数 = **5**。
 * 四个设计点：① 候选 3 **不可见但 meshGroupCount = 2** ⟹ 钉「word[5] 与可见性无关照写」
 * （gcX = 0 而 word[5] 非 0 的那一格）；② 候选 4 可见且 meshGroupCount = 11（表内最大）而
 * gcX 仍是 1 ⟹ 钉「gcX 单位是 task，不再乘 ×2 / 不携带 mesh 组数」；③ 每行 objectSlot ≠
 * clusterStart（字段互换必红），而候选 3 的 1000 / 1001 只差 1（写错一位也红）；④ 候选 4 的
 * objectSlot = 3 与候选 0 的 clusterStart = 3 同值 ⟹ 「行内字段错位」与「跨行错位」是两种
 * 不同的红，互不掩盖。
 */
const std::vector<MdiShadowCandidate>& anchor() {
	static const std::vector<MdiShadowCandidate> cands {
		MdiShadowCandidate { 7u, 6u, 3u, true },
		MdiShadowCandidate { 21u, 0u, 11u, false },
		MdiShadowCandidate { 19u, 9u, 17u, true },
		MdiShadowCandidate { 1000u, 2u, 1001u, false },
		MdiShadowCandidate { 3u, 11u, 7u, true },
	};
	return cands;
}

/** 锚点里第 i 条候选**该**发出的行（期望值 = 上面那张表，逐字段独立写死）。 */
MdiShadowRow expectedRow(size_t i) {
	// 🔴 ★M-3T-c：gcX 的单位是 **task**（可见 ? 1 : 0），mesh 组数住在 word[5]
	static constexpr uint32_t gcX [] { 1u, 0u, 1u, 0u, 1u };
	static constexpr uint32_t obj [] { 7u, 21u, 19u, 1000u, 3u };
	static constexpr uint32_t start [] { 3u, 11u, 17u, 1001u, 7u };
	static constexpr uint32_t groups [] { 6u, 0u, 9u, 2u, 11u };  // 与可见性无关照写
	MdiShadowRow row;
	row.groupCountX = gcX[i];
	row.objectIndex = obj[i];
	row.clusterStart = start[i];
	row.meshGroupCount = groups[i];
	return row;  // Y/Z = 1 由默认成员初始化给（= 契约的一部分，正向用例逐字段钉）
}

/** 逐字段比较（被测结构体不带 operator==；比较器住测试侧，先例 test_meshlet_lod）。 */
bool rowEq(const MdiShadowRow& a, const MdiShadowRow& b) {
	return a.groupCountX == b.groupCountX && a.groupCountY == b.groupCountY
		&& a.groupCountZ == b.groupCountZ && a.objectIndex == b.objectIndex
		&& a.clusterStart == b.clusterStart && a.meshGroupCount == b.meshGroupCount;
}

/** 把行表逐行摊成「objectIndex 序列」（行序判据的读图：错位一格立刻看出来）。 */
std::vector<uint32_t> objectIndexSequence(const MdiShadowRowTable& table) {
	std::vector<uint32_t> out;
	out.reserve(table.rows.size());
	for (const MdiShadowRow& r : table.rows)
		out.push_back(r.objectIndex);
	return out;
}

/** 按置换取候选（行序判据的期望值出处）。 */
std::vector<MdiShadowCandidate> permuted(const std::vector<size_t>& order) {
	std::vector<MdiShadowCandidate> out;
	out.reserve(order.size());
	for (size_t i : order)
		out.push_back(anchor()[i]);
	return out;
}

/** 临时 lua 配置（构造写盘、析构删除；先例 test_scene_config 的 TempLua）。 */
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
	/** 传给 loader 的路径字符串（生命周期随本对象）。 */
	std::string pathStr() const { return path_.string(); }

  private:
	std::filesystem::path path_;
};

// ─── 反向用例表（同一张表喂「逐条归因」与「两两不同」两条用例，杜绝两处漂移）──────────
/** 一个坏形态 = 一个造错误串的入口（构建器 or 对账器）+ 期望错误串的唯一关键词。 */
struct BadCase {
	const char* name;                            ///< 坏形态名（INFO 定位）
	std::optional<std::string> (*run)();         ///< 造出错误串（nullopt = 没报错 = 用例红）
	const char* needle;                          ///< 期望错误串的唯一关键词
};

/** 锚点候选的构建结果（构建失败 = 测试自己错了，REQUIRE 直接红）。 */
MdiShadowRowTable builtAnchor() {
	auto built = buildMdiShadowRows(anchor());
	REQUIRE(built.has_value());
	return *built;
}

/** 不变量 2（**构建器路径**）：可见候选 meshGroupCount = 0 —— 上游契约破了，宁可报错也不发空行。 */
std::optional<std::string> badVisibleNoTasks() {
	std::vector<MdiShadowCandidate> cands = anchor();
	cands[2].meshGroupCount = 0u;  // 可见 + 零 mesh 组（🔴 ★M-3T-c 判的是 meshGroupCount，不是 gcX）
	const auto built = buildMdiShadowRows(cands);
	return built ? std::nullopt : std::optional<std::string> { built.error() };
}
/** 不变量 1（形态 A：漏行）：行数组少一行（构建器将来若「顺手压紧」就是这个形状）。 */
std::optional<std::string> badRowDropped() {
	MdiShadowRowTable table = builtAnchor();
	table.rows.pop_back();
	return validateMdiShadowRows(table, anchor());
}
/** 不变量 1（形态 B：压紧）：把两条不可见行删掉 —— 看着像优化，破的是「行序 = 候选序」。 */
std::optional<std::string> badRowCompacted() {
	MdiShadowRowTable table = builtAnchor();
	std::vector<MdiShadowRow> kept;
	for (const MdiShadowRow& r : table.rows)
		if (r.groupCountX != 0u)
			kept.push_back(r);
	table.rows.swap(kept);
	return validateMdiShadowRows(table, anchor());
}
/**
 * 不变量 1（形态 A′：**漏行而三个锚全部诚实**）——只有「行数 == 候选数」这一条能抓的形态。
 *
 * 🔴 这条是变异 M06（`rows.size() != candidateCount` 弱化成 `>`，只挡多行放过少行）逼出来的，
 * 而它**两次**差点写成假隔离，两个坑都记在这里：
 *   ① 锚不重算 ⟹ 漏行时出声的是不变量 3（`sumGroupCountX` 与剩下的行对不上），"弱化行数判据"
 *      这件事本身没有专属信号（形态 A `badRowDropped` 就是这个形状，它靠邻居救场）。
 *   ② 删**中间**那一行 ⟹ 后面的行整体前移，出声的是不变量 4（"行 i 不是候选 i"）——实测该形态
 *      下 M06 只红 1 条且点名的是错的不变量。⟹ 必须删**最后一行**，剩下的行才与候选 0..3 对齐。
 * 于是本用例：删最后一行（前提 `anchor()[4].visible`，钉成 REQUIRE）+ 四个锚按剩下的行**重算**
 * + 补进来的候选可见（`emptyRows == 不可见候选数` 仍成立）⟹ 判据面只剩「行数 == 候选数」一条，
 * 弱化即**静默**（skill `rgl-test-discipline`：「变异要测弱化，不只测删除」）。
 * 形态也真实：将来真做压紧的人一定连锚一起重算，不会留着一个谎报的 rowCount。
 * 🔴 ★M-3T-c 起「诚实重算」多了一枚 `sumMeshGroupCount`（漏了它 ⟹ 出声的是不变量 3 的 mesh 锚
 * 分支而非行数判据，本用例就白隔离了）。
 */
std::optional<std::string> badRowDroppedHonestAnchors() {
	REQUIRE(anchor()[4].visible);  // 前提：末位候选可见 ⟹ 删它那一行不改变「不可见候选数」
	std::vector<MdiShadowCandidate> cands = anchor();
	cands.push_back(MdiShadowCandidate { 4242u, 5u, 13u, true });  // 可见 ⟹ 不可见候选数不变
	MdiShadowRowTable table = builtAnchor();
	table.rows.pop_back();  // 🔴 删**最后一行**：剩下的 4 行仍与候选 0..3 逐位对齐
	// （删中间行会让后面的行整体错位 ⟹ 出声的是不变量 4「行 i 不是候选 i」，本用例就白写了）
	// 🔴 四个锚按剩下的行**重算**（诚实）——谎报是形态 C 的职责。
	table.rowCount = table.rows.size();
	table.sumGroupCountX = 0u;
	table.sumMeshGroupCount = 0u;
	table.emptyRows = 0u;
	for (const MdiShadowRow& r : table.rows) {
		table.sumGroupCountX += r.groupCountX;
		table.sumMeshGroupCount += r.groupCountX == 0u ? 0u : r.meshGroupCount;  // 只算可见行
		table.emptyRows += r.groupCountX == 0u ? 1u : 0u;
	}
	return validateMdiShadowRows(table, cands);
}
/**
 * 不变量 1（形态 A″：**多一行而四个锚全部诚实**）——与形态 A′ 镜像的另一半边（§19.6 挂号）。
 *
 * 🔴 为什么必须单独一条：`rows.size() != candidateCount` 弱化成本地正确的 `<`（只挡少行、
 * **放过**多行）时，形态 A/A′/B 全是「少行」⟹ 它们照样红，**这条漂移完全静默**。而多行比少行
 * 更危险：`checkRowOrder` 的循环按 `rows.size()` 跑，多出来的那一行会拿 `cands[5]` 做期望
 * = `std::span` 越界读（无边界检查、无信号）。
 * 形态也真实：段边界算重一格（把下一段的首行也算进本级联）、或给一个可见候选双发一行，
 * 都会连锚一起重算 —— 谎报的 rowCount 是形态 C 的职责，这里要的是**诚实**的多行。
 * 期望值出处 = 不变量 1 第一条判据的**方向**（"holds 6 rows but ... holds 5"），不抄被测码：
 * needle 里带上 6/5 两个数，比较符被写反也红。
 */
std::optional<std::string> badRowExtraHonestAnchors() {
	MdiShadowRowTable table = builtAnchor();
	const MdiShadowRow extra = table.rows.back();  // 先取副本（push_back 自引用是 UB 温床）
	table.rows.push_back(extra);                   // 多一行（形态 = 段边界重一格 / 双发）
	// 🔴 四个锚按 6 行**重算**（诚实）——不重算则出声的是不变量 3，行数判据没有专属信号。
	table.rowCount = table.rows.size();
	table.sumGroupCountX = 0u;
	table.sumMeshGroupCount = 0u;
	table.emptyRows = 0u;
	for (const MdiShadowRow& r : table.rows) {
		table.sumGroupCountX += r.groupCountX;
		table.sumMeshGroupCount += r.groupCountX == 0u ? 0u : r.meshGroupCount;  // 只算可见行
		table.emptyRows += r.groupCountX == 0u ? 1u : 0u;
	}
	return validateMdiShadowRows(table, anchor());  // 候选仍是 5 条
}
/** 不变量 1（形态 C：锚与数组分家）：`rowCount` 谎报。 */
std::optional<std::string> badRowCountAnchor() {

	MdiShadowRowTable table = builtAnchor();
	table.rowCount = 3u;  // 数组仍是 5 行
	return validateMdiShadowRows(table, anchor());
}
/** 不变量 3（Σ gcX 锚谎报）。 */
std::optional<std::string> badSumAnchor() {
	MdiShadowRowTable table = builtAnchor();
	table.sumGroupCountX += 1u;
	return validateMdiShadowRows(table, anchor());
}
/** 不变量 3（空行数锚谎报）。 */
std::optional<std::string> badEmptyAnchor() {
	MdiShadowRowTable table = builtAnchor();
	table.emptyRows = 1u;  // 实际 2 条空行
	return validateMdiShadowRows(table, anchor());
}
/**
 * 「可见候选发出空行」——🔴 ★M-3T-c 起这条由**不变量 4** 点名（旧语义下 gcX 可以是 0，
 * 该形态要走到不变量 3 的「空行数 vs 不可见候选数」分支才出声；新语义下可见候选的 want gcX
 * 恒 1，got gcX = 0 在逐行对账处当场现形，错误串逐字打印 got/want 的 gcX，归因照样可定位）。
 * 「空行数 vs 不可见候选数」那条分支由此变成**哨兵**（validate 路径结构上到不了，只有
 * rowFor 级变异才打到它——见文件头「两条新哨兵」与红线表 ④）。
 * 手搭行表 = 唯一激励（构建器路径下不变量 2 先拦），而 ★M-4 的 GPU 行表正是「行表与候选清单
 * 分别由两个实现产出」的形态 ⟹ 这条不是假想敌。
 */
std::optional<std::string> badVisibleEmittedEmptyRow() {
	const std::vector<MdiShadowCandidate> cands { MdiShadowCandidate { 5u, 0u, 3u, true } };
	MdiShadowRowTable table;
	table.rows.push_back(MdiShadowRow { 0u, 1u, 1u, 5u, 3u, 0u });  // 可见候选的「合法」行
	table.sumGroupCountX = 0u;   // 与行数组对得上
	table.emptyRows = 1u;        // 与行数组对得上
	table.rowCount = 1u;
	return validateMdiShadowRows(table, cands);
}
/**
 * 不变量 3b 的期望激励（**外部表**形态）：手工把某行 gcX 改成 2（「一个 draw 发多枚 task」的
 * 字节形态）。🔴 实测归因（2026-10-13）：validate 先跑逐行对账，而 `rowFor` 永不产出 gcX=2
 * ⟹ 出声的是不变量 4（错误串逐字打印 got 的 `gcX 2`，可归因）；3b 本体的专属信号在**构建器
 * 路径**（rowFor 是两侧共用的唯一公式，逐行对账两边一起变抓不到）——红线表 ⑳ 用变异实测。
 */
std::optional<std::string> badGcXMultiTask() {
	MdiShadowRowTable table = builtAnchor();
	table.rows[2].groupCountX = 2u;  // 一个 draw 发两枚 task（×2 被乘回 gcX 的形态）
	return validateMdiShadowRows(table, anchor());
}
/**
 * 不变量 3（`sumMeshGroupCount` 谎报成「全行求和」）——🔴 这正是新语义下最容易写错的谎法：
 * 锚 = 6+0+9+2+11 = 28（把不可见行的 word[5] 也计进来），而判据期望 = 可见行之和 26。
 * 两数不等 ⟹ 这条反向用例有牙（若锚本来就是全行求和，谎报值 == 诚实值，用例恒真）。
 */
std::optional<std::string> badSumMeshAnchorAllRows() {
	MdiShadowRowTable table = builtAnchor();
	table.sumMeshGroupCount = 28u;  // 全行求和（正确值 26 = 只算可见行）
	return validateMdiShadowRows(table, anchor());
}
/**
 * 不变量 4（**word[5] 进了逐元素比对面**的专属激励）：两行只有 meshGroupCount 不同
 * （gcX 都是 1、objectIndex / clusterStart 相同）⟹ 互换后三个聚合锚一位不变、身份字段
 * 全对得上——若 meshGroupCount 不在比对面，这个坏形态会**完全静默**。
 */
std::optional<std::string> badMeshGroupsSwapped() {
	const std::vector<MdiShadowCandidate> cands {
		MdiShadowCandidate { 7u, 6u, 3u, true },
		MdiShadowCandidate { 7u, 9u, 3u, true },  // 同槽同 start，只有 meshGroupCount 不同
	};
	auto built = buildMdiShadowRows(cands);
	REQUIRE(built.has_value());
	std::swap(built->rows[0], built->rows[1]);
	return validateMdiShadowRows(*built, cands);
}
/** 不变量 4（序）：两行互换 —— 三个聚合锚**一位不变**，只有逐行判据抓得到。 */
std::optional<std::string> badRowsSwapped() {
	MdiShadowRowTable table = builtAnchor();
	std::swap(table.rows[1], table.rows[2]);
	return validateMdiShadowRows(table, anchor());
}
/** 不变量 4（身份字段错位）：行序对、第 3 行的 objectIndex 被写成别的槽。 */
std::optional<std::string> badIdentityRewritten() {
	MdiShadowRowTable table = builtAnchor();
	table.rows[3].objectIndex = 4242u;
	return validateMdiShadowRows(table, anchor());
}

const std::vector<BadCase>& badCases() {
	static const std::vector<BadCase> table = {
		{ "可见候选 meshGroupCount = 0（构建器路径）", badVisibleNoTasks, "invariant 2" },
		{ "行数组少一行（漏行）", badRowDropped, "invariant 1" },
		{ "漏行且四个锚全部诚实（只有行数判据抓得到）", badRowDroppedHonestAnchors,
			"but the candidate list holds" },
		{ "多一行且四个锚全部诚实（不变量 1 的 `<` 半边）", badRowExtraHonestAnchors,
			"holds 6 rows but the candidate list holds 5" },
		{ "压紧不可见行（行序破了）", badRowCompacted, "invariant 1" },
		{ "rowCount 锚谎报", badRowCountAnchor, "rowCount says" },
		{ "sumGroupCountX 锚谎报", badSumAnchor, "sumGroupCountX says" },
		{ "sumMeshGroupCount 谎报成全行求和", badSumMeshAnchorAllRows, "sumMeshGroupCount says" },
		{ "emptyRows 锚谎报", badEmptyAnchor, "emptyRows says" },
		{ "可见候选发出空行（★M-3T-c 起由不变量 4 点名）", badVisibleEmittedEmptyRow,
			"invariant 4" },
		{ "两行互换（聚合量一位不变）", badRowsSwapped, "invariant 4" },
		{ "行内身份字段被改写", badIdentityRewritten, "invariant 4" },
		{ "一个 draw 发多枚 task（gcX 手改成 2）", badGcXMultiTask, "invariant 4" },
		{ "只有 meshGroupCount 不同的两行互换（word[5] 进比对面）", badMeshGroupsSwapped,
			"invariant 4" },
	};
	return table;
}

/** 跑一条坏形态并 REQUIRE 它真的报错（两两不同用例用，坏形态静默 = 用例红）。 */
std::string errorOf(const BadCase& bad) {
	auto err = bad.run();
	INFO("坏形态：", bad.name);
	REQUIRE(err.has_value());
	return *err;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 正向锚点（期望值 = 上面那张手算表）
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("正向锚点：五行候选的行数组逐字段 + 四个聚合锚等于手算常量") {
	const auto built = buildMdiShadowRows(anchor());
	REQUIRE(built.has_value());
	REQUIRE(built->rows.size() == 5u);  // 前提守卫：下面的逐行遍历不能是空遍历
	for (size_t i = 0; i < built->rows.size(); ++i) {
		INFO("行 ", i);
		CHECK(rowEq(built->rows[i], expectedRow(i)));
		// 逐字段再钉一遍（rowEq 是「全等」，这里钉的是「哪个字段是什么」——只靠 rowEq，
		// 一个把两个字段同时写歪的变异可能撞上另一个用例，失败信息也指不到字段名）
		CHECK(built->rows[i].groupCountY == 1u);   // 恒 1（一维派发）
		CHECK(built->rows[i].groupCountZ == 1u);   // 恒 1（一维派发）
		// 🔴 ★M-3T-c：word[5] 不再是保留位，= 候选带来的 meshGroupCount（不可见行也照写）
		CHECK(built->rows[i].meshGroupCount == anchor()[i].meshGroupCount);
		CHECK(built->rows[i].objectIndex == anchor()[i].objectSlot);
		CHECK(built->rows[i].clusterStart == anchor()[i].clusterStart);
	}
	CHECK(built->sumGroupCountX == 3u);   // 🔴 task 数 = 可见行数（1+1+1），不再是 mesh 组数 26
	CHECK(built->sumMeshGroupCount == 26u);  // 🔴 6+9+11（只算可见行；全行求和是 28）
	CHECK(built->emptyRows == 2u);        // 候选 1、3
	CHECK(built->rowCount == 5u);
	// 🔴 不变量 3b（★M-3T-c 新契约）：Σ gcX == rowCount − emptyRows（每可见行恰一枚 task）
	CHECK(built->sumGroupCountX == built->rowCount - built->emptyRows);
	// 自洽契约：锚 == 逐行独立重算（★M-1 的 mdi.count / mdi.rows 直接取这两个锚，分家 = 收益判据读错臂）
	uint64_t sum = 0, sumMesh = 0, empty = 0;
	for (const MdiShadowRow& r : built->rows) {
		sum += r.groupCountX;
		sumMesh += r.groupCountX == 0u ? 0u : r.meshGroupCount;  // 只累加可见行
		empty += r.groupCountX == 0u ? 1u : 0u;
	}
	CHECK(built->sumGroupCountX == sum);
	CHECK(built->sumMeshGroupCount == sumMesh);
	CHECK(built->emptyRows == empty);
	CHECK(built->rowCount == built->rows.size());
	// 构建器返回前自检过 ⟹ 对外再验一次必须自洽（同一份判据，两处调用点必须同结论）
	CHECK_FALSE(validateMdiShadowRows(*built, anchor()).has_value());
}

TEST_CASE("🔴 行序 = 候选序：换三种顺序 ⟹ 行数组逐元素跟着换（压紧/重排必须红）") {
	// 三条置换：恒等 / 整体倒序 / 打乱。期望 = 按同一置换重排的正向锚点行。
	// 「聚合量抓不到错位」是 test_meshlet_lod 的 base-shift 教训：Σ gcX 与空行数在三条置换下
	// **一位不变**（本用例末尾把它钉成断言，读的人不必自己推）。
	const std::vector<std::vector<size_t>> orders {
		{ 0u, 1u, 2u, 3u, 4u },
		{ 4u, 3u, 2u, 1u, 0u },
		{ 2u, 0u, 4u, 1u, 3u },
	};
	std::vector<std::vector<uint32_t>> seen;
	for (const std::vector<size_t>& order : orders) {
		const auto built = buildMdiShadowRows(permuted(order));
		REQUIRE(built.has_value());
		REQUIRE(built->rows.size() == order.size());
		for (size_t i = 0; i < order.size(); ++i) {
			INFO("置换 ", order[0], " 行 ", i, "（候选 ", order[i], "）");
			CHECK(rowEq(built->rows[i], expectedRow(order[i])));
		}
		// 聚合量与序无关（这三条**不是**行序判据，写在这里是为了让「只有逐行断言抓得到」可读）
		CHECK(built->sumGroupCountX == 3u);      // task 数（可见行数）
		CHECK(built->sumMeshGroupCount == 26u);  // 可见行 mesh 组数
		CHECK(built->emptyRows == 2u);
		CHECK(built->rowCount == 5u);
		seen.push_back(objectIndexSequence(*built));
	}
	// 前提守卫：三条置换确实给出了三种行序（若构建器内部排序/压紧，三条会并成一条 ⟹ 这里红）
	CHECK_FALSE(seen[0] == seen[1]);
	CHECK_FALSE(seen[0] == seen[2]);
	CHECK_FALSE(seen[1] == seen[2]);
}

TEST_CASE("不可见行：gcX = 0 而 objectIndex / clusterStart / meshGroupCount 照写（红了要能定位是哪一行）") {
	const auto built = buildMdiShadowRows(anchor());
	REQUIRE(built.has_value());
	REQUIRE(built->rows.size() == 5u);
	for (size_t i = 0; i < anchor().size(); ++i) {
		const MdiShadowCandidate& c = anchor()[i];
		if (c.visible)
			continue;
		INFO("不可见候选 ", i);
		CHECK(built->rows[i].groupCountX == 0u);
		// 🔴 身份字段照写（整行清零 = 红了无法定位是哪一行，且与 §19.2 D4 口径不符）
		CHECK(built->rows[i].objectIndex == c.objectSlot);
		CHECK(built->rows[i].clusterStart == c.clusterStart);
		CHECK(built->rows[i].groupCountY == 1u);
		CHECK(built->rows[i].groupCountZ == 1u);
		// 🔴 ★M-3T-c：word[5] 是**载荷**不是保留位，且**与可见性无关照写**（候选 3 不可见而
		// meshGroupCount = 2 —— 这一格钉的就是「不可见行 word[5] 非 0」；task 只在 gcX=1 才读它，
		// 但红了要能定位是哪一行）
		CHECK(built->rows[i].meshGroupCount == c.meshGroupCount);
	}
	// 候选 1 不可见且 meshGroupCount = 0、候选 3 不可见但 meshGroupCount = 2 ⟹ 两条 gcX 同读数
	// （visible 说话，不可见时**不看** meshGroupCount；调用方不必特意清零），而 word[5] 读数不同
	CHECK(built->rows[1].groupCountX == 0u);
	CHECK(built->rows[3].groupCountX == 0u);
	CHECK(built->rows[3].meshGroupCount == 2u);  // 🔴 不可见行的 word[5] 不是 0
	// 反向半边：可见行的 gcX **恒 1**（meshGroupCount 再大也不进 gcX——候选 4 的 11 是最强探针；
	// 🔴 旧断言「taskCount 原样进 gcX」在新语义下是**错**的，单位已经从 mesh 组换成 task）
	CHECK(built->rows[0].groupCountX == 1u);
	CHECK(built->rows[2].groupCountX == 1u);
	CHECK(built->rows[4].groupCountX == 1u);
	CHECK(built->rows[4].meshGroupCount == 11u);  // mesh 组数住在 word[5]，不在 gcX
}

TEST_CASE("🔴 前 12B 逐字段对账 VkDrawMeshTasksIndirectCommandEXT（rlbase 不能 import rlvk 的第二钉）") {
	// 双钉的另一半（static_assert 那半在 rgl/mdi_rows.cppm）：本文件链全部四层库，可以拿真
	// Vulkan 结构体来对。rlbase 里**不许**出现这个名字（分层护栏 rules_layers 扫 import 边）。
	static_assert(sizeof(VkDrawMeshTasksIndirectCommandEXT) == 12,
		"VkDrawMeshTasksIndirectCommandEXT is no longer 12B — §19.2 D1's row layout premise changed");
	static_assert(offsetof(VkDrawMeshTasksIndirectCommandEXT, groupCountX) == 0,
		"Vk command groupCountX moved off offset 0");
	static_assert(offsetof(VkDrawMeshTasksIndirectCommandEXT, groupCountY) == 4,
		"Vk command groupCountY moved off offset 4");
	static_assert(offsetof(VkDrawMeshTasksIndirectCommandEXT, groupCountZ) == 8,
		"Vk command groupCountZ moved off offset 8");
	static_assert(sizeof(MdiShadowRow) == 24, "row is not 24B — §19.2 D1");
	static_assert(offsetof(MdiShadowRow, objectIndex) == 12,
		"shader reads word[3] = byte offset 12 (objectIndex)");
	static_assert(offsetof(MdiShadowRow, clusterStart) == 16,
		"shader reads word[4] = byte offset 16 (clusterStart)");
	static_assert(offsetof(MdiShadowRow, meshGroupCount) == 20,
		"task reads word[5] = byte offset 20 (meshGroupCount, ★M-3T-c 起是真载荷)");

	const auto built = buildMdiShadowRows(anchor());
	REQUIRE(built.has_value());
	REQUIRE(built->rows.size() == 5u);
	for (size_t i = 0; i < built->rows.size(); ++i) {
		INFO("行 ", i);
		// 前 12B 拷进真结构体 ⟹ 逐字段必须等于 (可见 ? 1 : 0, 1, 1)（🔴 ★M-3T-c：gcX 单位 = task）
		VkDrawMeshTasksIndirectCommandEXT cmd {};
		std::memcpy(&cmd, &built->rows[i], sizeof(cmd));
		const uint32_t wantX = anchor()[i].visible ? 1u : 0u;
		CHECK(cmd.groupCountX == wantX);
		CHECK(cmd.groupCountY == 1u);
		CHECK(cmd.groupCountZ == 1u);
		// 后 12B：word[3]/word[4] 是 shader 直读的两个索引，word[5] = meshGroupCount（task 拿去当
		// DispatchMesh 的 X；真机上它已被设备读取，不再是「保留、恒 0」）
		const auto* words = reinterpret_cast<const uint32_t*>(&built->rows[i]);
		CHECK(words[3] == anchor()[i].objectSlot);   // 不是 meshGroupCount（字段互换的变异在这里红）
		CHECK(words[4] == anchor()[i].clusterStart);
		CHECK(words[5] == anchor()[i].meshGroupCount);  // 不可见行也照写（候选 3 = 2）
	}
	// 行表是**连续**的 24B 行（★M-1 直接把 rows.data() memcpy 进 ring buffer，按 stride 逐行跳）
	CHECK(sizeof(MdiShadowRow) == kMdiShadowRowStride);
	CHECK(&built->rows[4] - &built->rows[0] == 4);  // 相邻行无 padding（指针差按元素算）
}

// ─────────────────────────────────────────────────────────────────────────────
// ★M-3T-c 三条新契约（task 化 gcX / 可见行对账量 / word[5] 进比对面）
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("🔴 3b task 数：Σ gcX == rowCount − emptyRows（每可见行恰一枚 task）+ 反向 gcX=2 必报") {
	const auto built = buildMdiShadowRows(anchor());
	REQUIRE(built.has_value());
	// 正向逐行半边：gcX ∈ {0,1}（「一个 draw 一枚 task」的字节形态；×2 若被乘回 gcX 红在这里）
	for (size_t i = 0; i < built->rows.size(); ++i) {
		INFO("行 ", i);
		CHECK(built->rows[i].groupCountX <= 1u);
	}
	// 正向聚合半边：期望值出处 = 手算表（3 枚可见行 × 1 = 3 = 5 − 2），不取被测锚互证
	CHECK(built->sumGroupCountX == 3u);
	CHECK(built->sumGroupCountX == built->rowCount - built->emptyRows);
	// 反向：手工把行 2 的 gcX 改成 2（「一个 draw 发多枚 task」的字节形态）⟹ validate 必须红。
	// 🔴 归因实测（2026-10-13）：validate 先跑逐行对账而 rowFor 永不产出 gcX=2 ⟹ 出声的是
	// 不变量 4，错误串逐字打印 got 的 `gcX 2`（可归因）；3b 本体的专属信号在构建器路径
	// （rowFor 两侧共用、逐行对账抓不到自己的公式），见文件头红线表 ⑳ 的实测错误串。
	const std::optional<std::string> err = badGcXMultiTask();
	REQUIRE(err.has_value());
	CHECK(err->find("mdi rows: invariant") != std::string::npos);
	CHECK(err->find("gcX 2") != std::string::npos);  // 错误串点名了那枚 2
}

TEST_CASE("sumMeshGroupCount 只算可见行（跨形态对账量）+ 反向：谎报成全行求和必报") {
	const auto built = buildMdiShadowRows(anchor());
	REQUIRE(built.has_value());
	// 正向：期望 = 可见行（0、2、4）的 6+9+11 = 26；全行求和 = 28（候选 3 不可见而 word[5]=2）。
	// 🔴 两个数**不相等**才是判别力：实现若全行求和会得 28，第一条断言当场红。
	CHECK(built->sumMeshGroupCount == 26u);
	CHECK(built->sumMeshGroupCount != 28u);
	// 激励本身就在锚点表里：不可见行的 word[5] 照写（2）但不进对账量
	CHECK(built->rows[3].groupCountX == 0u);
	CHECK(built->rows[3].meshGroupCount == 2u);
	// 反向：把锚谎报成全行求和（28）⟹ 不变量 3 的 sumMeshGroupCount 分支点名
	const std::optional<std::string> err = badSumMeshAnchorAllRows();
	REQUIRE(err.has_value());
	CHECK(err->find("sumMeshGroupCount says") != std::string::npos);
}

TEST_CASE("🔴 meshGroupCount 进不变量 4 的逐元素比对面：只换 word[5] 的两行互换必须红") {
	// 两行可见候选：gcX 都是 1、objectIndex / clusterStart 相同，只有 meshGroupCount 不同（6/9）
	// ⟹ 互换后三个聚合锚一位不变、gcX 与身份字段全对得上——word[5] 若不在比对面，这个坏形态
	// 会**完全静默**（本批把 meshGroupCount 加进比对面这件事就没有任何测试守着）。
	const std::vector<MdiShadowCandidate> cands {
		MdiShadowCandidate { 7u, 6u, 3u, true },
		MdiShadowCandidate { 7u, 9u, 3u, true },
	};
	const auto built = buildMdiShadowRows(cands);
	REQUIRE(built.has_value());
	MdiShadowRowTable swapped = *built;
	std::swap(swapped.rows[0], swapped.rows[1]);
	// 前提守卫：锚随表原样带走，而互换确实动不了它们（两行 gcX 都是 1、求和与序无关）
	CHECK(swapped.sumGroupCountX == built->sumGroupCountX);
	CHECK(swapped.sumMeshGroupCount == built->sumMeshGroupCount);
	CHECK(swapped.emptyRows == built->emptyRows);
	const auto err = validateMdiShadowRows(swapped, cands);
	REQUIRE(err.has_value());
	INFO("错误串：", *err);
	CHECK(err->find("invariant 4") != std::string::npos);
	CHECK(err->find("meshGroupCount") != std::string::npos);  // 错误串要能点名 word[5]（可归因）
}

TEST_CASE("stride 合法性（VUID-07088：4 的倍数且 ≥ 12）+ 🔴 与用例里的行数/偏移都不相等") {
	CHECK(kMdiShadowRowStride % 4u == 0u);   // VUID-07088 前半
	CHECK(kMdiShadowRowStride >= 12u);       // VUID-07088 后半（≥ sizeof(Vk…)）
	CHECK(kMdiShadowRowStride == sizeof(MdiShadowRow));
	CHECK(kMdiShadowRowStride == 24u);       // §19.2 D1 的定案值
	CHECK(kMdiShadowRowStride > sizeof(VkDrawMeshTasksIndirectCommandEXT));  // 24 > 12：多出来的正是载荷
	// 🔴 spike §6.3 的 M4 教训钉成断言：stride 与本文件任何用例的行数/偏移都不相等 ⟹ 「行数 ↔ 步长」
	// 参数序写反在每条用例上都可检测（两个数相等时互换测不出东西）。凑数即红。
	constexpr uint32_t caseNumbers [] {
		5u, 3u, 1u,               // 用例里的行数 / 候选数
		3u, 11u, 17u, 1001u, 7u,  // 用例里的 clusterStart（偏移）
		0u, 4u, 8u, 12u, 16u, 20u,  // 用例里的字节偏移
		26u, 28u,                 // ★M-3T-c 的聚合锚（可见行和 / 全行和——两者都别撞上 24）
		// ★M-1a 尺寸算术用例的输入与读数（🔴 同一条纪律：pair 的两个成员与 stride 相等时，
		// 「偏移↔行号互换」「乘 sliceRows 还是乘 slices」这类写反都测不出东西）
		2u, 7u, 10u, 5u, 72u, 192u, 168u, 240u, 336u, 324u, 4773u, 8u, 1u, 0u
	};
	for (uint32_t n : caseNumbers) {
		INFO("用例数值 ", n);
		CHECK(kMdiShadowRowStride != n);
	}
}

TEST_CASE("边界：空候选序列 = 合法空表（不是错误），且对账自洽") {
	const auto built = buildMdiShadowRows(std::span<const MdiShadowCandidate> {});
	REQUIRE(built.has_value());
	CHECK(built->rows.empty());
	CHECK(built->sumGroupCountX == 0u);
	CHECK(built->sumMeshGroupCount == 0u);
	CHECK(built->emptyRows == 0u);
	CHECK(built->rowCount == 0u);
	CHECK_FALSE(validateMdiShadowRows(*built, {}).has_value());
	// 前提守卫（本用例自己也要能失败）：空表**不是**上面几条断言的假绿来源——正向锚点用例
	// 已钉非空形态，这里钉的是「空输入不报错」这条契约（★M-1 里 = 某级联一个候选都不落）。
}

// ─────────────────────────────────────────────────────────────────────────────
// 反向：四条不变量各配可达用例（关键词逐条钉死）+ 错误串两两不同
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("反向：全部坏形态各自报出自己的不变量（关键词逐条钉死）") {
	REQUIRE(badCases().size() == 14u);  // 前提守卫：表被删空 = 下面的遍历恒真是假绿
	for (const BadCase& bad : badCases()) {
		const std::string err = errorOf(bad);
		INFO("坏形态：", bad.name, " 错误串：", err);
		CHECK(err.find("mdi rows: invariant") != std::string::npos);  // 统一前缀 = 可归因
		CHECK(err.find(bad.needle) != std::string::npos);
	}
}

TEST_CASE("反向：十四条错误串两两不同（可归因判据本身也要有牙）") {
	std::vector<std::string> errors;
	errors.reserve(badCases().size());
	for (const BadCase& bad : badCases())
		errors.push_back(errorOf(bad));
	REQUIRE(errors.size() == 14u);
	for (size_t i = 0; i < errors.size(); ++i)
		for (size_t j = i + 1; j < errors.size(); ++j) {
			INFO("错误串 ", i, " 与 ", j, " 相同：", errors[i]);
			CHECK_FALSE(errors[i] == errors[j]);
		}
	// 五条不变量各至少出现一次（只钉「两两不同」会让「几条其实只实现了一条」照样绿；
	// 🔴 3b 不在其列——它在 validate 路径是哨兵（文件头「两条新哨兵」），可达激励全被不变量 4
	// 先接住，其本体由红线表 ⑳ 的构建器路径变异实测，不在此凑数）
	for (const char* prefix : { "invariant 1", "invariant 2", "invariant 3", "invariant 4" }) {
		INFO("缺少不变量：", prefix);
		CHECK(std::ranges::any_of(errors,
			[&](const std::string& e) { return e.find(prefix) != std::string::npos; }));
	}
}

TEST_CASE("反向：不变量 2 只在构建器判（对账器不重复判输入契约），且不可见候选的 meshGroupCount 不被看") {
	// 半边 A：可见 + 零 mesh 组 ⟹ 构建器拒绝（宁可报错也不发一条 DispatchMesh(0) 的空 task；
	// 🔴 ★M-3T-c 判的是 meshGroupCount——可见行 gcX 恒 1，坏上游在 gcX 上完全看不出来）
	std::vector<MdiShadowCandidate> cands = anchor();
	cands[4].meshGroupCount = 0u;
	const auto bad = buildMdiShadowRows(cands);
	REQUIRE_FALSE(bad.has_value());
	CHECK(bad.error().find("invariant 2") != std::string::npos);
	CHECK(bad.error().find("visible with meshGroupCount 0") != std::string::npos);
	// 半边 B：不可见 + 零 mesh 组 ⟹ 合法（**不许**把它也当错误：那是把「不可见」读成「上游坏了」）
	std::vector<MdiShadowCandidate> okInvisible = anchor();
	okInvisible[1].meshGroupCount = 0u;
	okInvisible[1].visible = false;
	const auto ok = buildMdiShadowRows(okInvisible);
	REQUIRE(ok.has_value());
	CHECK(ok->rows[1].groupCountX == 0u);
	// 半边 C：不可见 + **非零** mesh 组 ⟹ 同样合法（gcX 仍是 0、word[5] 照写 77；调用方不必清零）
	std::vector<MdiShadowCandidate> okNonZero = anchor();
	okNonZero[1].meshGroupCount = 77u;
	okNonZero[1].visible = false;
	const auto ok2 = buildMdiShadowRows(okNonZero);
	REQUIRE(ok2.has_value());
	CHECK(ok2->rows[1].groupCountX == 0u);
	CHECK(ok2->rows[1].meshGroupCount == 77u);   // 与可见性无关照写
	CHECK(ok2->sumGroupCountX == 3u);            // 不可见 ⟹ gcX 0，不进 Σ task
	CHECK(ok2->sumMeshGroupCount == 26u);        // 🔴 77 也不进对账量（不可见行不计）
}

// ─────────────────────────────────────────────────────────────────────────────
// 配置门 meshlet.mdi_shadow（★M-0 零消费者，但键必须**被真读到**）
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("runtime_config：meshlet.mdi_shadow 默认 false、探针 true 命中、非布尔即失败、无扁平双读") {
	// 🔴 探针值取 **true**（≠ 默认 false）：取 true 时「键被读到」与「键被静默忽略」结果相同
	// = 用例失去判别力却照样绿（§18.11 翻默认那条纪律，本仓 M5 实测坐实过）。
	{  // 成员初始化默认路径（不看 lua 文件）
		CHECK(RuntimeConfig {}.meshletMdiShadow == false);
	}
	{  // 缺块 / 缺键 = 保持默认 false
		TempLua f("rt_mdi_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletMdiShadow == false);
		TempLua g("rt_mdi_nokey", R"lua(return { meshlet = { assets = true } })lua");
		auto r2 = loadRuntimeConfig(g.pathStr());
		REQUIRE(r2.has_value());
		CHECK(r2->meshletMdiShadow == false);
	}
	{  // 块内键名命中（探针 true）
		TempLua f("rt_mdi_on", R"lua(return { meshlet = { mdi_shadow = true } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletMdiShadow == true);
	}
	{  // 显式 false 也读得到（与「缺键回落」同读数，但键名命中路径由上一条钉）
		TempLua f("rt_mdi_off", R"lua(return { meshlet = { mdi_shadow = false } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletMdiShadow == false);
	}
	{  // 非布尔 = **启动失败**点名键（meshlet 块五枚布尔同纪律：静默回落 = 截图判据整批假绿）
		for (const char* bad : {R"lua(return { meshlet = { mdi_shadow = "yes" } })lua",
		                        R"lua(return { meshlet = { mdi_shadow = 1 } })lua"}) {
			TempLua f("rt_mdi_bad", bad);
			CHECK_FALSE(loadRuntimeConfig(f.pathStr()).has_value());
		}
	}
	{  // 🔴 **没有**扁平键的兼容双读（两个真相源 = 本仓反复登记的反模式）
		TempLua f("rt_mdi_flat", R"lua(return { meshlet_mdi_shadow = true })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletMdiShadow == false);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Lcg 属性组（写死种子；期望值 = 闭式契约，不抄被测码）
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// ★M-1a：尺寸/偏移算术（§19.5 末两条 buffer 边界 VUID + 🔴 argsOffset 与 rowBase 同源）
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("🔴 同源 pair：argsOffset == rowBase × stride，且两者由同一个 row 推出") {
	// 数值全部互不相等（slices=2 / sliceRows=7 / frameSlot=1 / firstRow=3，spike §6.3 的 M4
	// 教训同族：两个数取成相等 ⟹ 偏移↔步长互换也测不出东西）。
	// 手算期望（出处 = §19.5 的公式，不抄被测码）：
	//   row      = frameSlot*sliceRows + firstRow = 1*7 + 3 = 10
	//   rowBase  = 10
	//   argsOff  = 10*24 = 240
	const auto loc = mdiShadowRowLocation(7u, 1u, 3u);
	CHECK(loc.rowBase == 10u);
	CHECK(loc.argsOffset == 240u);
	// 🔴 同源这件事本身（pair 的两半必须来自同一个 row —— 任一半单独乘/加都红）
	CHECK(loc.argsOffset == static_cast<uint64_t>(loc.rowBase) * kMdiShadowRowStride);
	// 两个数**不相等**才有牙（若哪天它们巧合相等，互换的变异就隐形）
	CHECK(loc.argsOffset != loc.rowBase);
	CHECK(loc.rowBase != 7u);   // 不是 sliceRows
	CHECK(loc.rowBase != 2u);   // 不是 slices
	CHECK(loc.rowBase != 3u);   // 不是 firstRow
	CHECK(loc.argsOffset != 7u);
	CHECK(loc.argsOffset != 2u);
	CHECK(loc.argsOffset != 3u);
	CHECK(loc.argsOffset != 24u);  // 也不是 stride（stride 只做乘数，不当值用）
	// 恒等半边：frameSlot = 0 时 rowBase == firstRow，但 offset 仍是它的 24 倍
	const auto zero = mdiShadowRowLocation(7u, 0u, 3u);
	CHECK(zero.rowBase == 3u);
	CHECK(zero.argsOffset == 72u);
	CHECK(zero.argsOffset != zero.rowBase);
	// 换 sliceRows 时两个数一起跟着走（🔴 不许出现「offset 按 sliceRows 算而 rowBase 不跟」，
	// 那正是 shader 读错行、vkCmd 读对地方的分家形态）
	const auto other = mdiShadowRowLocation(5u, 1u, 3u);
	CHECK(other.rowBase == 8u);
	CHECK(other.argsOffset == 192u);
	CHECK(other.argsOffset == static_cast<uint64_t>(other.rowBase) * kMdiShadowRowStride);
	CHECK(other.argsOffset != loc.argsOffset);
	CHECK(other.rowBase != loc.rowBase);
	// offset 必须是 4 的倍数（VUID-…-offset-02710）
	CHECK(loc.argsOffset % 4u == 0u);
	CHECK(zero.argsOffset % 4u == 0u);
	CHECK(other.argsOffset % 4u == 0u);
}

TEST_CASE("slice 与整张容量：capacity == slices × sliceBytes == rows × slices × stride") {
	// 手算期望（24B 行、互不相等的数）：rows=7 slices=2 ⟹ slice=168、capacity=336
	CHECK(mdiShadowSliceBytes(7u) == 168u);
	CHECK(mdiShadowArgsCapacityBytes(7u, 2u) == 336u);
	CHECK(mdiShadowArgsCapacityBytes(7u, 2u) == 2u * mdiShadowSliceBytes(7u));
	// 真产物口径（★M-1a 装配日志该打出的量：可切上界 1591 × 级联段 3 = sliceRows 4773，
	// ring×2 ⟹ capacity 229,104B）——独立闭式，不被测码读走
	CHECK(mdiShadowSliceBytes(4773u) == 4773ull * 24ull);
	CHECK(mdiShadowArgsCapacityBytes(4773u, 2u) == 4773ull * 24ull * 2ull);
	// 零行 = 零字节（装配层据此拒绝建载体，见 createMeshletShadowArgsRing 闸门①）
	CHECK(mdiShadowSliceBytes(0u) == 0u);
	CHECK(mdiShadowArgsCapacityBytes(0u, 2u) == 0u);
	// 🔴 容量 == 最后一个 slice 的 argsOffset 上界：末 slice 的段起点偏移 == (slices−1)×sliceBytes
	const auto last = mdiShadowRowLocation(7u, 1u, 0u);
	CHECK(last.argsOffset == mdiShadowSliceBytes(7u));  // 帧槽 1 的 0 行 = 第 2 个 slice 的起点
	CHECK(last.argsOffset + mdiShadowSliceBytes(7u) == mdiShadowArgsCapacityBytes(7u, 2u));
}

TEST_CASE("🔴 闭式边界：末行 offset+12 落在容量内；容量少 1 字节必须报") {
	// 两条 VUID 收成一条公式（spec 原文 = drawing.adoc:2596-2606）：
	//   07089（drawCount==1）：offset + 12 ≤ size
	//   07090（drawCount> 1）：stride×(drawCount−1) + offset + 12 ≤ size
	// drawCount==1 时 07090 退化成 07089（stride×0 = 0）⟹ 同一条 tail 判据同时是两个 VUID。
	CHECK(kMdiShadowCommandBytes == 12u);
	CHECK(kMdiShadowCommandBytes < kMdiShadowRowStride);  // 12 < 24：每行只读前一半（多读 = 容量算错）
	// 独立写的两份期望（不取被测 tail 的读数）
	const uint64_t off = mdiShadowRowLocation(7u, 1u, 0u).argsOffset;  // = 168
	CHECK(mdiShadowTailBytes(off, 1u) == off + 12u);                   // 07089 手算
	CHECK(mdiShadowTailBytes(off, 7u) == off + 24u * 6u + 12u);        // 07090 手算
	CHECK(mdiShadowTailBytes(off, 0u) == off);  // 零 draw 不读任何字节（不许无符号回绕）
	// 最坏那一格：末个帧槽、段起点 0、一次读完整个 slice
	const uint64_t worst = mdiShadowTailBytes(off, 7u);                // = 168+144+12 = 324
	CHECK(worst == 324u);
	const uint64_t capacity = mdiShadowArgsCapacityBytes(7u, 2u);      // = 336
	CHECK(mdiShadowRangeFits(capacity, off, 7u));
	// 🔴 真正的约束点是「末行尾字节」，不是整张容量：容量按 stride 算（每行 24B），而设备每行
	// 只读 12B ⟹ 最小够用容量 == worst（= capacity − stride + 12 = 324）。所以"少 1 字节必须报"
	// 要拿 **worst** 减 1 来测——拿 capacity 减 1 测到的是**留在容量里的那 12B 余量**，判据当然
	// 还是绿（首版就写错过一次，红在这里才发现期望值出处错了）。
	CHECK(mdiShadowRangeFits(worst, off, 7u));      // 恰等号成立（spec 写的是 less than or equal）
	CHECK_FALSE(mdiShadowRangeFits(worst - 1u, off, 7u));  // 少 1 字节 ⟹ 立刻不成立
	// 越界一行的形态也拦得住（slice 1 的 firstRow 6 + 7 行 ⟹ 尾越出整张容量）
	const uint64_t pastOffset = mdiShadowRowLocation(7u, 1u, 6u).argsOffset;  // = 13×24 = 312
	const uint64_t past = mdiShadowTailBytes(pastOffset, 7u);  // = 312 + 6×24 + 12 = 468
	CHECK(pastOffset == 312u);
	CHECK(past == 468u);
	CHECK(past > capacity);  // 越界 = VU 违例（不崩、不报错，只是读越界当命令）
	CHECK_FALSE(mdiShadowRangeFits(capacity, pastOffset, 7u));
	// 逐 (slice, firstRow, drawCount) 全扫一遍：合法组合一个都不许报
	for (uint32_t slice = 0; slice < 2u; ++slice)
		for (uint32_t first = 0; first < 7u; ++first)
			for (uint32_t count = 1u; first + count <= 7u; ++count) {
				const uint64_t o = mdiShadowRowLocation(7u, slice, first).argsOffset;
				INFO("slice ", slice, " first ", first, " count ", count);
				CHECK(mdiShadowRangeFits(capacity, o, count));
				// 尾随字节跨度 = tail − offset（= 载体 argsBytes 的口径，不许是 count×stride）
				CHECK(mdiShadowTailBytes(o, count) - o == 24ull * (count - 1u) + 12ull);
			}
}

TEST_CASE("Lcg 属性组（种子 20261013）：随机候选序列的闭式契约 + 打乱后行序跟着走") {

	testing::Lcg rng(20261013);
	int cases = 0, nonEmpty = 0;
	for (int iter = 0; iter < 60; ++iter) {
		const size_t n = static_cast<size_t>(rng.uniformInt(0, 9));
		std::vector<MdiShadowCandidate> cands;
		cands.reserve(n);
		for (size_t i = 0; i < n; ++i) {
			MdiShadowCandidate c;
			c.objectSlot = static_cast<uint32_t>(rng.uniformInt(0, 4000));
			// 可见候选的 meshGroupCount 必须 > 0（不变量 2 的定义域）；不可见的故意给 0..4000 两态
			const bool visible = rng.uniformInt(0, 1) == 1;
			c.meshGroupCount = static_cast<uint32_t>(visible ? rng.uniformInt(1, 4000)
			                                                 : rng.uniformInt(0, 4000));
			c.clusterStart = static_cast<uint32_t>(rng.uniformInt(0, 4000));
			c.visible = visible;
			cands.push_back(c);
		}
		INFO("iter ", iter, " 候选数 ", n);
		const auto built = buildMdiShadowRows(cands);
		REQUIRE(built.has_value());
		++cases;
		if (n > 0)
			++nonEmpty;

		// ① 行数 == 候选数 + 逐行闭式期望（🔴 ★M-3T-c：gcX = visible ? 1 : 0，身份字段与
		// meshGroupCount 照抄——word[5] 与可见性无关）
		REQUIRE(built->rows.size() == n);
		uint64_t wantSum = 0, wantMeshSum = 0, wantEmpty = 0;
		for (size_t i = 0; i < n; ++i) {
			const MdiShadowRow& r = built->rows[i];
			const MdiShadowCandidate& c = cands[i];
			const uint32_t wantX = c.visible ? 1u : 0u;
			INFO("iter ", iter, " 行 ", i);
			CHECK(r.groupCountX == wantX);
			CHECK(r.objectIndex == c.objectSlot);
			CHECK(r.clusterStart == c.clusterStart);
			CHECK(r.meshGroupCount == c.meshGroupCount);
			CHECK(r.groupCountY == 1u);
			CHECK(r.groupCountZ == 1u);
			wantSum += wantX;
			wantMeshSum += wantX == 0u ? 0u : c.meshGroupCount;  // 只算可见行
			wantEmpty += wantX == 0u ? 1u : 0u;
		}
		// ② 聚合锚 == 独立重算（期望值出处 = 上面那份闭式 wantX，不是被测累加器）
		CHECK(built->sumGroupCountX == wantSum);
		CHECK(built->sumMeshGroupCount == wantMeshSum);
		CHECK(built->emptyRows == wantEmpty);
		CHECK(built->rowCount == n);
		// 🔴 不变量 3b 的随机形态读数：每可见行恰一枚 task（wantSum 由 wantX 独立累加）
		CHECK(built->sumGroupCountX == built->rowCount - built->emptyRows);
		// ③ 构建器产物必过对账（同一份判据的两个调用点）
		CHECK_FALSE(validateMdiShadowRows(*built, cands).has_value());

		// ④ 打乱输入 ⟹ 行数组按同一置换重排，而聚合锚**不变**（序是契约、聚合量不是）
		if (n >= 2u) {
			std::vector<size_t> order(n);
			for (size_t i = 0; i < n; ++i)
				order[i] = i;
			for (size_t i = n - 1; i > 0; --i) {  // Fisher–Yates（Lcg，禁真随机）
				const size_t j = static_cast<size_t>(rng.uniformInt(0, static_cast<int>(i)));
				std::swap(order[i], order[j]);
			}
			std::vector<MdiShadowCandidate> shuffled;
			shuffled.reserve(n);
			std::vector<uint32_t> wantOrder;
			for (size_t i = 0; i < n; ++i) {
				shuffled.push_back(cands[order[i]]);
				wantOrder.push_back(cands[order[i]].objectSlot);
			}
			const auto shuf = buildMdiShadowRows(shuffled);
			REQUIRE(shuf.has_value());
			CHECK(objectIndexSequence(*shuf) == wantOrder);
			CHECK(shuf->sumGroupCountX == built->sumGroupCountX);
			CHECK(shuf->sumMeshGroupCount == built->sumMeshGroupCount);
			CHECK(shuf->emptyRows == built->emptyRows);
			CHECK(shuf->rowCount == built->rowCount);
		}
	}
	CHECK(cases == 60);     // 前提守卫：一条都没造出来 = 上面全是空遍历假绿
	CHECK(nonEmpty > 40);   // 非空形态必须占多数（否则「行序」那组断言整批空跑）
}

TEST_CASE("Lcg 属性组二（种子 20261013，★M-1a 尺寸算术）：pair 恒等 × 容量全扫 × 少 1 字节必红") {
	// 期望值全部由本文件的**独立算式**给出（`wantRow` / `wantTail` 手写），不取被测函数读数
	// ——skill `rgl-test-discipline`「断言独立于实现」。三个数互不相等由 rng 保证（下界见 uniformInt）。
	testing::Lcg rng(20261013);
	int cases = 0;
	for (int iter = 0; iter < 40; ++iter) {
		const uint32_t sliceRows = static_cast<uint32_t>(rng.uniformInt(2, 40));
		const uint32_t slices = static_cast<uint32_t>(rng.uniformInt(2, 5));
		const uint32_t frameSlot = static_cast<uint32_t>(rng.uniformInt(0, static_cast<int>(slices) - 1));
		const uint32_t firstRow = static_cast<uint32_t>(rng.uniformInt(0, static_cast<int>(sliceRows) - 1));
		const uint32_t drawCount = static_cast<uint32_t>(
		    rng.uniformInt(1, static_cast<int>(sliceRows - firstRow)));
		// 独立算式（出处 = §19.5 的公式，不是被测函数）
		const uint64_t wantRow = static_cast<uint64_t>(frameSlot) * sliceRows + firstRow;
		const uint64_t wantSliceBytes = static_cast<uint64_t>(sliceRows) * 24ull;
		const uint64_t wantCapacity = wantSliceBytes * slices;
		const uint64_t wantTail = wantRow * 24ull + 24ull * (drawCount - 1ull) + 12ull;
		INFO("iter ", iter, " rows ", sliceRows, " slices ", slices, " slot ", frameSlot,
			" first ", firstRow, " count ", drawCount);
		const auto loc = mdiShadowRowLocation(sliceRows, frameSlot, firstRow);
		CHECK(static_cast<uint64_t>(loc.rowBase) == wantRow);
		CHECK(loc.argsOffset == wantRow * 24ull);
		// 🔴 同源恒等（两半必须来自同一个 row）
		CHECK(loc.argsOffset == static_cast<uint64_t>(loc.rowBase) * kMdiShadowRowStride);
		CHECK(mdiShadowSliceBytes(sliceRows) == wantSliceBytes);
		CHECK(mdiShadowArgsCapacityBytes(sliceRows, slices) == wantCapacity);
		CHECK(mdiShadowTailBytes(loc.argsOffset, drawCount) == wantTail);
		// 合法组合必须放行（最后一个 slice 的最后一段恰好贴到容量）
		CHECK(mdiShadowRangeFits(wantCapacity, loc.argsOffset, drawCount));
		// 🔴 边界两半各钉一条。判据本体是「末行尾」而不是整张容量：容量按 stride 算（24B/行）而
		// 设备每行只读 12B ⟹ 真容量恒留 `stride − 12` 的余量，「容量 − 1 必报」那种写法**永不红**
		// （首版就写成那样，被抓后改成以尾为锚）。等号成立 = 放行（spec: less than or equal），
		// 尾减 1 = 必须报。
		CHECK(mdiShadowRangeFits(wantTail, loc.argsOffset, drawCount));
		CHECK_FALSE(mdiShadowRangeFits(wantTail - 1u, loc.argsOffset, drawCount));
		// 尾随跨度 == argsBytes 的口径（stride×(count−1)+12，不是 count×stride）
		CHECK(mdiShadowTailBytes(loc.argsOffset, drawCount) - loc.argsOffset
			== 24ull * (drawCount - 1ull) + 12ull);
		CHECK(loc.argsOffset % 4u == 0u);  // VUID-…-offset-02710
		++cases;
	}
	CHECK(cases == 40);  // 前提守卫
}

}  // namespace RGL
