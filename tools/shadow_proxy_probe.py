#!/usr/bin/env python3
"""shadow_proxy_probe.py —— 从簇产物本体量 shadow proxy 的四个决策数（批次 N10 的 P0）。

计划 → `docs/shadow_proxy_plan.md` §7。**零引擎、零 GPU、零产物改动**：只读
`build/rgl/models/<name>.glb.meshlets.bin`，布局唯一真相源 = `tools/meshlet_pack/format.h`。

## 它回答的四个问题（答案决定 P2 的形态，不是走流程）

1. **prim 级保守到底贵多少**：把选级上界从"自身最深级"换成"细结构存活级"后，各候选 D 下的三角数
   比。占比小 ⟹ prim 级保守是终局；大 ⟹ 该付离线重排簇序的代价做分量级拆条
   （`shadow_proxy_plan.md` §5 那条"不做"就此翻案）。
2. **分量数分布**：每 prim 几个连通分量、每分量几三角 ⟹ proxy 条目数量级，以及 R4（draws 不涨）
   抵不抵得住。
3. **表体积**：按 8 个量化档组织后多大。
4. **建表耗时**：Python 版实测 + 操作计数，用来给 C++ 版定标（P1 打进启动日志的那条读数）。

## 核心判据：什么叫"这个分量的独立遮挡没了"

今天的选级语义是 **uniform 整级**（`MeshletShadowLodTable::Entry` = 每 objectSlot 一个
`(start, count)`，画的是该 prim 某一级的连续簇区间）。于是"某分量还在不在"有唯一可读的答案：

    分量 c 在级 L 的存活率 = |c 的三角 ∩ {depth==L 的簇的三角}| / |c 的 L0 三角|
    c 死亡 ⟺ 存活率 == 0（它的三角在级 L 的表示里一个都不剩）
    safeLevel(c) = c 死亡的最早一级（从未死亡 = levelCount）
    maxSafeLevel(prim) = min over c of safeLevel(c)

**为什么用"三角归零"而不是"同簇混装"当判据**（本文件初稿犯的错，写下来）：L0 簇本来就常常跨分量
——`meshopt_buildMeshlets` 按**空间邻近**聚簇，不保证连通性。L0 混装不丢遮挡（整簇全画，两个部件都
在），丢遮挡发生在**简化把某分量的三角删光**那一刻。所以判据必须是"逐分量逐级的三角覆盖"，
不是"簇纯不纯"。

**为什么这个判据在产物上算得出来**：`format.h` 头注"LOD 不增顶点（简化只在原顶点集上产索引表）"
⟹ 任意一级的任意一簇，其三角经 `Tris`(8bit 局部索引) + `Remap`(全局顶点下标) 还原成全局顶点下标，
再经 L0 算出的连通分量归属表落回分量。**逐三角归属是精确的**：一个三角的三条边都被 union 过
⟹ 三顶点必同分量（所以取首顶点即可）。

## 🔴 连通性必须先做**位置焊接**，否则整条判据给出的是碎片

源数据是 de-indexed 的（逐面顶点，flat shading 要逐面法线）⟹ 相邻三角在同一位置存**两份不同下标**
的顶点，按下标跑并查集会把一块实心平板切成几百个"分量"。本探针首跑没做焊接，**全部读数作废**
（实测：`shadow_edge_rig` 的 36×12 网格平板被判成 664 个分量、每分量 2 三角；bistro 读出的
"282,242 个分量、p50 = 4 三角"同样是这个病的形状）。焊接 = 按 pos 的 12 字节**精确位模式**合并
（de-index 是复制，坐标逐位相同 ⟹ 不需要容差；一旦加"格距"参数它就变成能调到绿的旋钮）。
判据有效性的**阴性对照**：`--expect max-comps-per-prim=1` 跑 `shadow_edge_rig`——每块板是单一封闭
壳体，几何事实要求它 == 1。

## 连通分量的作用域（一条会读错的边界，写死）

并查集只在**单个 primitive 内**跑。理由：阴影合并发生在 `clodBuild` 逐 prim 建的 DAG 里，跨 prim
的两个部件本来就在不同 DAG、从不互相合并 ⟹ 跨 prim 算连通分量会**造出一批不存在的分量边界**。
归属从 `DiskGroup.prim` 拿；L0 簇 = `refined == -1`（`format.h` 对 L0 的**定义**）。

## 用法

    python3 tools/shadow_proxy_probe.py build/rgl/models/bistro_exterior.glb.meshlets.bin
    python3 tools/shadow_proxy_probe.py <产物> --json /tmp/opencode/probe.json
    python3 tools/shadow_proxy_probe.py <产物> --expect prims=1591,l0-clusters=27487,clusters=61518
    python3 tools/shadow_proxy_probe.py <产物> --max-prim 8      # 调试：只算前 8 个 prim

**判据有效性的两条对账（改完判据必须都重跑）**：

    # ① 阴性对照：每块板是单一封闭壳体 ⟹ 必须 == 1 个分量（未焊接时读 664，红）
    python3 tools/shadow_proxy_probe.py build/rgl/models/shadow_edge_rig.glb.meshlets.bin \
        --expect max-comps-per-prim=1
    # ② 阳性锚点：故意把判据改成"按下标不焊接"，上面那条必须转红（见 --no-weld）
    python3 tools/shadow_proxy_probe.py build/rgl/models/shadow_edge_rig.glb.meshlets.bin \
        --expect max-comps-per-prim=1 --no-weld

`--expect` 是给"解析器自己也要有阳性锚点"用的：格式读错时 counts 立刻对不上，而不是安静地算出
一张错表（先例教训 → skill `rgl-test-discipline`：判据工具最坏的病不是崩，是静默给错数）。
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
from array import array
from collections import Counter, defaultdict
from mmap import ACCESS_READ, mmap

MAGIC = b"RGLCLOD\x00"
PRIM_SIZE = 16          # DiskPrim
CLUSTER_SIZE = 44       # DiskCluster
GROUP_SIZE = 36         # DiskGroup
VERTEX_STRIDE = 48      # ModelVertex（pos f32×3 @0）

# FileHeader 字段表（照 format.h 顺序）：magic8 ver4 tool4 sha20 fnv4 然后 **7**×u32（primCount..
# nodeWidth），@68 是编译器为 uint64 插的 **4B 对齐填充**（不是字段！），再 2×u64 三角数 + 7×u64
# 段偏移 = 144B，其后 reserved[12] 凑满 192B。
# 🔴 这条布局**不靠推理成立**：main() 里拿「末段末尾 == 文件字节数」硬自证，错了当场停
# （bistro 实测 offNodes + nodeCount×32 = 184,553,920 == 文件大小，逐位闭合）。
HDR_FMT = "<8sII20sIIIIIIII4xQQ7Q"
HEADER_SIZE = 192
assert struct.calcsize(HDR_FMT) == 144, "头部字段表应算到 144B（+ reserved[12] + 4B 填充 = 192B）"

# proxy 表每条目字节数（P2 形态：word[0] task 数 + clusterStart + meshGroupCount 三枚由选级派生）
ENTRY_BYTES = 12
# 候选 D 扫描档（与 `shadow_lod` 的实测决策表同集合，便于直接对账）
CANDIDATE_D = (0, 3, 5, 7, 11)
# 🔴 "从未死亡"的哨兵**必须落在真实级号域之外**。首跑用 `levelCount` 当哨兵，于是
# `shadow_edge_rig`（levelCount=5）的哨兵值 5 与真实级号 5 同值 ⟹ 9 块板全被误判成
# "出厂档会丢遮挡"，而代价表与引擎实测（六档 PNG 逐位全等）都说没丢。bistro 的
# levelCount=12 恰好躲过——**只看 bistro 这个 bug 会一路活到 C++ 移植**。
INF = 10 ** 9
# 分量门槛扫描档（三角数）——数 1 的主变量，见该段说明
THRESHOLDS = (1, 4, 16, 64, 256, 1024, 4096)
# 尺寸门槛分档（**源 glb 单位**的分量包围对角线）——数 1b 的主变量。
# 🔴 为什么还要再来一张尺寸表：三角数是**复杂度**，而阴影可见性的判据是**长度**（分量投影占不占得
# 下本级纹素）。两者会分家：一片 30 三角的大遮阳篷在阴影图里比一块 3,000 三角的浮雕墙更该保。
# 所以 P1 的判据必须长在尺寸上，本表就是给那条判据定档用的。
# 🔴 门槛用**世界米**定义、内部除以 `--glb-scale` 换成源单位。首版直接把源单位当分档，
# 于是 bistro（scale=0.01）的"1.00m"档实际是源单位 100 = 1m 侥幸对上了，而换一个 scale
# 不同的资产，同一张表的门槛含义就整体平移——分档本身变成资产相关的隐式参数。
# 分档范围按 bistro 实测的消失分量尺寸分布定（对角线 204~1624 源单位 = 2~16m）。
SIZE_BINS_M = (0.1, 0.3, 1.0, 3.0, 10.0, 30.0)


def die(msg: str) -> None:
    print(f"probe: {msg}", file=sys.stderr)
    sys.exit(2)


class Product:
    """产物文件的只读视图（mmap——bistro 那份 184MB，不整体读进内存）。"""

    def __init__(self, path: str) -> None:
        self.fh = open(path, "rb")
        # 🔴 `access` **必须写关键字**：POSIX 下 mmap 的第三个位置参数是 `flags`，把 ACCESS_READ(=1)
        # 按位置传进去会变成 MAP_SHARED ⟹ 向只读文件要写映射，报 PermissionError 而不是类型错误。
        self.mm = mmap(self.fh.fileno(), 0, access=ACCESS_READ)
        (magic, ver, self.tool_version, _sha, _fnv, self.prim_count, self.vertex_count,
         self.cluster_count, self.group_count, self.node_count, self.level_count,
         self.node_width, self.l0_tris, self.total_tris,
         off_prim, off_vpool, off_clu, off_tris, off_remap, off_groups,
         off_nodes) = struct.unpack_from(HDR_FMT, self.mm, 0)
        if magic != MAGIC:
            die(f"magic 不符：{magic!r} ≠ {MAGIC!r}（不是 meshlet_pack 产物？）")
        if ver != 1:
            die(f"formatVersion={ver}，本探针只认 1")
        self.off = dict(prim=off_prim, vpool=off_vpool, clu=off_clu,
                        tris=off_tris, remap=off_remap, groups=off_groups, nodes=off_nodes)
        # 🔴 **解析器的硬自证**：末段（Nodes）末尾必须正好等于文件字节数。这条不需要任何外部
        # 锚点，而它能抓住"字段表错一位"这类会让后面全部读数变成错表的病——比 --expect 更该信。
        size = os.fstat(self.fh.fileno()).st_size
        end = off_nodes + self.node_count * 32
        if end != size:
            die(f"头部字段表与文件**不闭合**：offNodes + nodeCount×32 = {end:,} ≠ "
                f"文件大小 {size:,}（差 {size - end:+,}B）⟹ 偏移表读错，停止出数")
        seq = [off_prim, off_vpool, off_clu, off_tris, off_remap, off_groups, off_nodes]
        if seq != sorted(seq) or seq[0] != HEADER_SIZE:
            die(f"段偏移不单调或首段不在 {HEADER_SIZE}B：{seq}")
        self.file_size = size
        self.prim_table = [struct.unpack_from("<IIIi", self.mm,
                                              off_prim + i * PRIM_SIZE)
                           for i in range(self.prim_count)]

    def close(self) -> None:
        self.mm.close()
        self.fh.close()

    def groups(self) -> list[tuple]:
        """全部 group → (depth, prim, clusterOffset, clusterCount)。

        🔴 格式串 = `<ii20xII`：depth(4) + prim(4) + center[3]/radius/error(20) + 两枚 u32 = **36B**，
        与 `static_assert(sizeof(DiskGroup) == 36)` 同数。写成 `24x` 会读出 40B ⟹ 整段错位，
        depth/prim/簇区间全变垃圾而**不报任何错**（本探针首跑即此坑，靠下面两条段内自证抓出）。
        """
        base = self.off["groups"]
        fmt = "<ii20xII"
        assert struct.calcsize(fmt) == GROUP_SIZE, "DiskGroup 布局与 format.h 的 static_assert 不符"
        return [struct.unpack_from(fmt, self.mm, base + i * GROUP_SIZE)
                for i in range(self.group_count)]

    def clusters(self, lo: int, hi: int) -> list[tuple]:
        """簇区间 [lo,hi) → (group, refined, triCount, vertCount, triOffset, remapOffset)。

        前两个字段是 i32、后四个 u32，但这里只读不判符号（refined 的 -1 由调用方按有符号读）。
        """
        base = self.off["clu"]
        out = []
        for i in range(lo, hi):
            group, refined, tc, vc, to, ro = struct.unpack_from("<iiIIII", self.mm,
                                                                base + i * CLUSTER_SIZE)
            out.append((group, refined, tc, vc, to, ro))
        return out

    def cluster_global_tris(self, c: tuple) -> array:
        """一簇的三角 → 全局顶点下标扁平数组（len = triCount×3）。

        这一步是整条判据成立的全部依赖：Tris 段存 8bit **局部**索引、Remap 段存该簇 vertCount 个
        **全局**下标，两者一拼就是全局三角（"LOD 不增顶点"保证各级共用同一份顶点池）。
        """
        _, _, tc, vc, to, ro = c
        local = self.mm[self.off["tris"] + to: self.off["tris"] + to + tc * 3]
        remap = array("I")
        remap.frombytes(self.mm[self.off["remap"] + ro * 4:
                                self.off["remap"] + (ro + vc) * 4])
        return array("I", (remap[b] for b in local))

    def prim_bounds(self, p: int) -> tuple:
        """prim 的顶点位置包围盒（世界无关，源 glb 单位；proxy 的分量尺寸判据用它）。"""
        base, cnt, _idx, _mat = self.prim_table[p]
        lo = [float("inf")] * 3
        hi = [float("-inf")] * 3
        off = self.off["vpool"] + base * VERTEX_STRIDE
        for i in range(cnt):
            x, y, z = struct.unpack_from("<3f", self.mm, off + i * VERTEX_STRIDE)
            for k, v in enumerate((x, y, z)):
                if v < lo[k]:
                    lo[k] = v
                if v > hi[k]:
                    hi[k] = v
        return lo, hi


class DisjointSet:
    """并查集。元素 = prim 内顶点在**稠密重编号**后的下标。

    为什么不拿全局顶点下标直接当 parent 下标：bistro 单 prim 最大 200 万+ 顶点，而 prim 的顶点在池里
    是连续区间（`DiskPrim.vertexBase/vertexCount`）⟹ 用 `array` 按 `v - vertexBase` 直接索引，
    4B/顶点，比 dict 快一个量级也省一个量级。
    """

    def __init__(self, n: int) -> None:
        self.p = array("i", range(n))
        self.r = bytearray(n)

    def find(self, x: int) -> int:
        p = self.p
        root = x
        while p[root] != root:
            root = p[root]
        while p[x] != root:          # 路径压缩（迭代版，避免递归深度）
            p[x], x = root, p[x]
        return root

    def union(self, a: int, b: int) -> None:
        ra, rb = self.find(a), self.find(b)
        if ra == rb:
            return
        if self.r[ra] < self.r[rb]:
            ra, rb = rb, ra
        self.p[rb] = ra
        if self.r[ra] == self.r[rb]:
            self.r[ra] += 1


def weld(prod: Product, p: int, on: bool = True) -> tuple[array, int]:
    """prim 的顶点 → **逻辑顶点**下标：按 pos 的 12 字节精确位模式合并同位置顶点。

    🔴 **这一步不是优化，是判据成立的前提**（本探针首跑没做，全部读数作废）：源数据是 de-indexed
    的（逐面顶点，flat shading 要逐面法线），相邻三角在同一位置存**两份不同下标**的顶点 ⟹ 按
    下标跑并查集会把一块实心平板切成几百个"分量"。实测：`shadow_edge_rig` 的 36×12 网格平板
    2,656 顶点 → 666 唯一位置（共享度 3.99×），未焊接时被判成 664 个分量、每分量 2 三角。

    为什么用**精确位模式**而不是量化容差：de-index 是从同一份数据复制的，坐标逐位相同 ⟹ 精确匹配
    就够；而一旦引入"格距"这个旋钮，它就变成可以被调到绿的参数（同 skill `rgl-test-discipline`
    那条"能调 = 能调绿"）。若将来出现"精确焊不上"的资产（顶点经过变换），**该报错出声**，
    而不是悄悄加容差——那由 `--expect max-comps-per-prim` 这条锚点抓。
    """
    base, vcnt = prod.prim_table[p][0], prod.prim_table[p][1]
    off = prod.off["vpool"] + base * VERTEX_STRIDE
    if not on:  # 变异开关：阳性锚点用（见文件头"对账 ②"），正常路径永不走这里
        lid = array("i", range(vcnt))
        return lid, vcnt, [struct.unpack_from("<3f", prod.mm, off + i * VERTEX_STRIDE)
                           for i in range(vcnt)]
    uniq: dict[bytes, int] = {}
    lid = array("i", bytes(4 * vcnt))
    pos: list[tuple] = []
    mm = prod.mm
    for i in range(vcnt):
        at = off + i * VERTEX_STRIDE
        key = mm[at:at + 12]
        j = uniq.get(key)
        if j is None:
            j = len(uniq)
            uniq[key] = j
            pos.append(struct.unpack_from("<3f", mm, at))
        lid[i] = j
    return lid, len(uniq), pos


def _fill(avail: dict, levels_present: list) -> list:
    """分量 → 逐级三角数数组，**按 prim 的实际级集合**索引（预计算，数 1c 要查 8×14.6 万次）。

    🔴 两件事必须分开，首版混一起做错过一次（症状：数 1c 对门槛八档全等）：
    ① **回落是 prim 级的事实**——prim 没有级 L 的 group 时，选级 L 回落到 ≤L 的最深存在级
      （`MeshletLodFallback::deepest`），所以"沿用上一个值"只在 `lv not in levels_present` 时对；
    ② 而"分量 c 在 prim 级 L 的表示里出现 **0 次**"就是 **0**，绝不能沿用上一个级。
    首版按 `max(avail)` 填、缺失一律沿用 ⟹ 所有早死的分量在 `want=D` 时都被算成还活着，
    于是"不受保护 = 随大流取 D"这一半恒等于"受保护"，门槛自然不动。
    """
    if not levels_present:
        return [0]
    out, cur = [], 0
    for lv in range(max(levels_present) + 1):
        if lv in levels_present:
            cur = avail.get(lv, 0)
        out.append(cur)
    return out


def analyze_prim(prod: Product, p: int, weld_on: bool = True) -> dict:
    """一个 prim 的全部分量 + 逐分量逐级覆盖 + maxSafeLevel。一趟解码，两遍使用。"""
    base, vcnt = prod.prim_table[p][0], prod.prim_table[p][1]
    lid, n_logical, pos = weld(prod, p, weld_on)

    # 该 prim 的簇按级分组（L0 = refined == -1，与 group.depth==0 口径的一致性在 main 里对账）
    groups = [g for g in prod.by_prim[p]]
    per_level: dict[int, list[tuple]] = defaultdict(list)
    l0_spans: list[tuple] = []          # (clusterOffset, clusterCount) —— L0 段的 group 区间
    for _depth, _prim, off, cnt in groups:
        if _depth == 0:
            l0_spans.append((off, cnt))
        for k, c in enumerate(prod.clusters(off, off + cnt)):
            per_level[_depth].append((off + k, c))
    l0 = [c for _gi, c in per_level[0]] if 0 in per_level else []
    if not l0:
        return {"prim": p, "empty": True}

    # ---- 第一遍：解码 L0 三角 + 在**逻辑顶点**上建并查集 ----
    l0_tris = [prod.cluster_global_tris(c) for c in l0]
    l0_gi = [gi for gi, c in per_level[0]] if 0 in per_level else []
    ds = DisjointSet(n_logical)
    unions = 0

    def logical(gv: int) -> int:
        j = gv - base
        if not (0 <= j < vcnt):
            die(f"prim {p}：簇三角的全局顶点下标越出 [vertexBase, +vertexCount) "
                f"⟹ prim 顶点区间不封闭，本探针的稠密索引假设破了（该断言不是可选的）")
        return lid[j]

    for g in l0_tris:
        for i in range(0, len(g), 3):
            a, b, cc = logical(g[i]), logical(g[i + 1]), logical(g[i + 2])
            ds.union(a, b)
            ds.union(b, cc)
            unions += 2

    # ---- 逐三角归属（精确：三顶点必同分量）+ 分量三角数 ----
    comp_l0: Counter = Counter()
    for g in l0_tris:
        for i in range(0, len(g), 3):
            comp_l0[ds.find(logical(g[i]))] += 1

    # ---- 纯度：逐簇跨几个分量（**分级算**，这是"要不要重排簇"的唯一判据）----
    # 粗级必然跨分量（简化就是靠合并），所以真正要问的是：**受保护分量在它能活的那一级上，
    # 是不是落在纯净簇里**。只有 L0 纯净 ⟹ 结论退化成"受保护分量恒 L0"，反而更简单更省。
    purity: dict[int, Counter] = defaultdict(Counter)
    comp_of_cluster: dict[int, set] = {}
    for lv in sorted(per_level):
        for gi, c in per_level[lv]:
            g = prod.cluster_global_tris(c)
            cs = {ds.find(logical(g[i])) for i in range(0, len(g), 3)}
            purity[lv][len(cs)] += 1
            comp_of_cluster[gi] = cs

    # ---- 连续性：每个分量的 L0 簇形成几个连续段（= 需要的 draw 条数下限）----
    runs_of_comp: dict[int, int] = {}
    pos_of_cluster = {gi: i for i, gi in enumerate(l0_gi)}
    comp_cluster_pos: dict[int, list] = defaultdict(list)
    for gi in l0_gi:
        for c in comp_of_cluster.get(gi, ()):
            comp_cluster_pos[c].append(pos_of_cluster[gi])
    for c, ps in comp_cluster_pos.items():
        ps.sort()
        runs_of_comp[c] = 1 + sum(1 for a, b in zip(ps, ps[1:]) if b != a + 1)

    # ---- 数 6 用：L0 段的 **group 结构**（排序粒度到底能多细）----
    # 🔴 `DiskGroup.clusterOffset/clusterCount` 要求一个 group 的簇**连续**，所以"prim 内 L0 段
    # 二分排序"实际能重排的是 **group 之间的顺序**，不是簇之间。于是排序粒度 = group，
    # 而它够不够细只能实测：一个 L0 group 跨几个分量？
    l0_groups_pure: Counter = Counter()
    l0_group_cluster_hist: Counter = Counter()
    l0_group_info: list[tuple] = []      # (簇数, L0 三角合计, 该 group 涉及的分量集合)
    for off, cnt in l0_spans:
        if not cnt:
            continue
        l0_group_cluster_hist[cnt] += 1
        cs, tri = set(), 0
        for gi in range(off, off + cnt):
            cs |= comp_of_cluster.get(gi, set())
            tri += sum(1 for _ in range(0, 0))
        # 三角合计：该 group 区间内每簇的 triCount 之和
        for k in range(cnt):
            tri += prod.clusters(off + k, off + k + 1)[0][2]
        l0_groups_pure[len(cs)] += 1
        l0_group_info.append((cnt, tri, cs))

    # ---- 每分量包围盒对角线（源 glb 单位，未乘 scene.lua 的 scale）----
    # 门槛的正确形态是**空间尺寸**（"在阴影图里占不占得下一枚纹素"），不是三角数；这里两个都算，
    # 是为了让 P0 能回答"三角数门槛与尺寸门槛差多远"——见 main 的 --list-affected 段。
    box: dict[int, list] = {}
    for j, (x, y, z) in enumerate(pos):
        b = box.get(ds.find(j))
        if b is None:
            box[ds.find(j)] = [x, x, y, y, z, z]
        else:
            if x < b[0]:
                b[0] = x
            elif x > b[1]:
                b[1] = x
            if y < b[2]:
                b[2] = y
            elif y > b[3]:
                b[3] = y
            if z < b[4]:
                b[4] = z
            elif z > b[5]:
                b[5] = z

    def diag(c: int) -> float:
        b = box[c]
        return ((b[1] - b[0]) ** 2 + (b[3] - b[2]) ** 2 + (b[5] - b[4]) ** 2) ** 0.5

    # ---- 第二遍：逐级覆盖，找每个分量的死亡级 ----
    alive_level = {c: INF for c in comp_l0}            # 未死亡 = INF（哨兵，见上面 INF 的注释）
    level_tris: dict[int, int] = {}
    # 🔴 顺手存下**逐分量逐级**的覆盖量（首版只留了死亡级，于是"分量级理想形态能省多少"这个
    # 数算不出来——而它恰恰是"要不要动打包工具"的唯一判据，见 main 的数 1c）。
    comp_level: dict[int, dict[int, int]] = {c: {} for c in comp_l0}
    for lv in sorted(per_level):
        seen: Counter = Counter()
        for _gi, c in per_level[lv]:
            g = prod.cluster_global_tris(c)
            for i in range(0, len(g), 3):
                seen[ds.find(logical(g[i]))] += 1
        level_tris[lv] = sum(seen.values())
        for c, n in seen.items():
            if c in comp_level:
                comp_level[c][lv] = n
        for comp, n in comp_l0.items():
            if alive_level[comp] == INF and seen.get(comp, 0) == 0:
                alive_level[comp] = lv
    # 从未在任何级出现过的分量（该 prim 的级集合不连续时）保持 INF
    max_safe = min(alive_level.values()) if alive_level else 0
    return {"prim": p, "empty": False, "unions": unions,
            "welded": f"{vcnt}->{n_logical}",
            "material": prod.prim_table[p][3],
            "comps": len(comp_l0), "l0_tris": sum(comp_l0.values()),
            "max_safe": max_safe, "level_tris": level_tris,
            "deepest": max(per_level) if per_level else 0,
            # (分量三角数, 该分量的死亡级, 分量包围对角线) ——门槛扫描要用这个**对应关系**，
            # 只留排序后的尺寸列表会把"谁在哪级死"丢掉（本探针首跑即如此，门槛扫描做不了）。
            "comps_detail": [(n, alive_level[c], diag(c)) for c, n in comp_l0.items()],
            "comp_level": {c: comp_level[c] for c in comp_l0},
            # 分量 → 逐级三角数**数组**，已按引擎的 `deepest` 回落策略填平（级不存在时沿用上一个
            # 存在级的值）。数 1c 要按 8 个门槛 × 14.6 万分量查，不预填平就是分钟级而不是秒级。
            "comp_at": {c: _fill(comp_level[c], sorted(per_level)) for c in comp_l0},
            # 分量 id → (L0 三角数, 死亡级, 包围对角线)。数 1c 要按**分量**取自己的级，
            # 而 comps_detail 是丢 id 的列表，所以这里保留 id 索引的那一份。
            "comps_info": {c: (n, alive_level[c], diag(c)) for c, n in comp_l0.items()},
            "levels_present": sorted(per_level),
            "comp_sizes": sorted(comp_l0.values(), reverse=True),
            "purity": {lv: dict(cnt) for lv, cnt in purity.items()},
            "runs": {c: (runs_of_comp.get(c, 1), alive_level[c], diag(c)) for c in comp_l0},
            "l0_groups": len(l0_group_info),
            "l0_group_purity": dict(l0_groups_pure),
            "l0_group_clusters": dict(l0_group_cluster_hist),
            "l0_spans": l0_spans,
            "l0_group_info": l0_group_info}


def main() -> int:
    ap = argparse.ArgumentParser(description="shadow proxy P0 探针（stdlib-only）")
    ap.add_argument("product", help="*.glb.meshlets.bin")
    ap.add_argument("--json", help="把全部读数写成 JSON（决策数据要能重放，别只留终端）")
    ap.add_argument("--expect", default="",
                    help="闭式锚点对账，形如 prims=1591,l0-clusters=27487,clusters=61518,levels=12")
    ap.add_argument("--max-prim", type=int, default=-1, help="只算前 N 个 prim（调试用）")
    ap.add_argument("--no-weld", action="store_true",
                    help="**变异开关**：关掉位置焊接。判据有效性的阳性锚点（文件头对账 ②）——"
                         "拿它跑 shadow_edge_rig 配 --expect max-comps-per-prim=1 必须红")
    ap.add_argument("--glb-scale", type=float, default=1.0, metavar="S",
                    help="源 glb 单位 → 世界米的换算（bistro 的 scene.lua scale=0.01）。"
                         "只影响打印的世界米列，不影响任何判据——门槛本身住在源单位里")
    ap.add_argument("--list-affected", type=int, default=0, metavar="T",
                    help="列出「出厂 D=5 会丢独立遮挡」且分量三角数 ≥ T 的 prim——门槛要看内容"
                         "才能定，只看代价曲线等于挑对结果最有利的数")
    args = ap.parse_args()

    t0 = time.perf_counter()
    prod = Product(args.product)
    print(f"== 产物头 ==  {args.product}")
    print(f"  prims={prod.prim_count} vertices={prod.vertex_count:,} "
          f"clusters={prod.cluster_count:,} groups={prod.group_count:,}")
    print(f"  levels={prod.level_count} nodeWidth={prod.node_width} "
          f"toolVersion={prod.tool_version}")
    print(f"  header.l0TriangleCount={prod.l0_tris:,}  totalTriangleCount={prod.total_tris:,}")
    print(f"  文件 {prod.file_size:,}B ｜ 末段 Nodes 末尾 = "
          f"{prod.off['nodes'] + prod.node_count * 32:,}B ⟹ **偏移表闭合 ✓**"
          f"（字段表错一位就走不到这行）")

    # ---- 分组索引（groups 只解析一次）----
    groups = prod.groups()
    prod.by_prim = defaultdict(list)
    for g in groups:
        prod.by_prim[g[1]].append(g)

    # 🔴 **段内自证 1**：`format.h` 明写"每簇恰在其出生级输出一次"⟹ 全部 group 的簇区间必须
    # **互不重叠、无缝覆盖** [0, clusterCount)。这条比任何外部锚点都硬：它错 = 我对 DiskGroup 的
    # 理解错，而那种错会让下面每一个数都变成"看起来合理的错表"。
    spans = sorted((off, off + cnt) for _d, _p, off, cnt in groups if cnt)
    if not spans or spans[0][0] != 0 or spans[-1][1] != prod.cluster_count:
        die("group 簇区间未覆盖全部簇：首 "
            f"{spans[0] if spans else None} 末 {spans[-1] if spans else None}"
            f"（clusterCount={prod.cluster_count}）⟹ DiskGroup 布局读错")
    for a, b in zip(spans, spans[1:]):
        if a[1] != b[0]:
            die(f"group 簇区间{'重叠' if a[1] > b[0] else '有缝'}于 {a[1]:,} ⟹ DiskGroup 布局读错")
    # 🔴 **段内自证 2**：prim 表必须无缝覆盖顶点池（否则"prim 顶点区间封闭"这条探针赖以成立的
    # 假设就是假的，而它会以"越界"的形式在很后面才炸，归因成本极高）。
    pspans = sorted((b, b + n) for b, n, _i, _m in prod.prim_table if n)
    if pspans[0][0] != 0 or pspans[-1][1] != prod.vertex_count:
        die(f"prim 表未覆盖顶点池 [0,{prod.vertex_count:,})：首 {pspans[0]} 末 {pspans[-1]}")
    for a, b in zip(pspans, pspans[1:]):
        if a[1] != b[0]:
            die(f"prim 顶点区间{'重叠' if a[1] > b[0] else '有缝'}于 {a[1]:,} ⟹ DiskPrim 布局读错")
    print(f"\n== 段内自证 ==\n"
          f"  group 簇区间无缝覆盖全部 {prod.cluster_count:,} 簇 ✓（{len(spans):,} 段）\n"
          f"  prim 顶点区间无缝覆盖全部 {prod.vertex_count:,} 顶点 ✓（{len(pspans):,} 段）")
    l0_by_group = sum(c for d, _p, _o, c in groups if d == 0)
    depth_hist = Counter(d for d, _p, _o, _c in groups)
    print("  group 按 depth 分布：" +
          "  ".join(f"d{k}:{v}" for k, v in sorted(depth_hist.items())))
    # 两条 L0 口径**独立复算**对账：`refined == -1`（format.h 对 L0 的定义）与 `group.depth == 0`。
    # 引擎侧由 meshlet_lod 的 invariant 3 钉这条一致性；这里复算是为了**探针自己**不是错表。
    l0_by_refined = 0
    for p, gs in prod.by_prim.items():
        for depth, _prim, off, cnt in gs:
            if depth != 0:
                continue
            l0_by_refined += sum(1 for c in prod.clusters(off, off + cnt) if c[1] == -1)

    anchors = {"prims": prod.prim_count, "clusters": prod.cluster_count,
               "levels": prod.level_count, "groups": prod.group_count,
               "l0-clusters": l0_by_group, "l0-clusters-refined": l0_by_refined}
    print(f"\n== 锚点 ==\n  按 group.depth==0 的 L0 簇 = {l0_by_group:,}"
          f"  ｜ 按 refined==-1 = {l0_by_refined:,}"
          f"  ｜ 两口径{'一致 ✓' if l0_by_group == l0_by_refined else '不一致 ✗'}")
    # 🔴 `--expect` 分**两轮**对：解析类（读头就能验）与分析类（要跑完才知道）。分析类里
    # `max-comps-per-prim` 是**判据有效性的阴性对照**——`shadow_edge_rig` 的每块板是单一封闭壳体
    # ⟹ 几何事实要求它 == 1。未做位置焊接时它读 664，当场把整条判据的碎片化病暴露出来。
    expects = {}
    for kv in filter(None, args.expect.split(",")):
        k, _, v = kv.partition("=")
        expects[k.strip()] = int(v)

    def check(table: dict, stage: str) -> None:
        for k, want in list(expects.items()):
            got = table.get(k)
            if got is None:
                continue
            if got != want:
                die(f"锚点 {k}（{stage}）对不上：产物给 {got:,}，期望 {want:,} "
                    f"⟹ 解析或判据有问题，**停止出数**（错表比没表更坏）")
            print(f"  {k} = {got:,} ✓（{stage}）")
            expects.pop(k)

    check(anchors, "解析")

    # ---- 逐 prim 分析 ----
    prims = sorted(prod.by_prim)
    if args.max_prim > 0:
        prims = prims[: args.max_prim]
    res = []
    t_an = time.perf_counter()
    for n, p in enumerate(prims):
        res.append(analyze_prim(prod, p, not args.no_weld))
        if n % 100 == 0:
            print(f"  prim {n + 1}/{len(prims)}  "
                  f"{time.perf_counter() - t_an:.1f}s", flush=True)
    t_an = time.perf_counter() - t_an
    res = [r for r in res if not r.get("empty")]
    print(f"\n分析 {len(res)} 个有簇 prim，{t_an:.1f}s")

    # ---- 数 2：分量分布 ----
    comp_sizes = [s for r in res for s in r["comp_sizes"]]
    per_prim_n = Counter(r["comps"] for r in res)
    l0_total = sum(r["l0_tris"] for r in res)
    check({"max-comps-per-prim": max(per_prim_n) if per_prim_n else 0,
           "components": len(comp_sizes), "l0-tris": l0_total}, "分析")
    if expects:
        die(f"--expect 不认得键 {sorted(expects)}")
    welded = [r["welded"] for r in res][:3]
    print(f"  位置焊接抽样（顶点数→唯一位置）：{'  '.join(welded)}")
    print(f"\n== 数 2：连通分量分布 ==")
    print(f"  分量总数 = {len(comp_sizes):,}（prim 数 {len(res)} ⟹ 均值 "
          f"{len(comp_sizes) / len(res):.1f}）")
    cum = 0
    for k in sorted(per_prim_n):
        cum += per_prim_n[k]
        if k <= 8:
            print(f"    {k:>3} 个分量：{per_prim_n[k]:>5} prim"
                  f"（累计 {cum / len(res) * 100:.1f}%）")
    # 尾部不打逐档（bistro 首跑了 90 行分布表，把结论淹了）——改打分位数，一行给完形状
    cps = sorted(r["comps"] for r in res)
    print(f"  每 prim 分量数：p50={cps[len(cps) // 2]} p90={cps[int(len(cps) * .9)]} "
          f"p99={cps[int(len(cps) * .99)]} max={cps[-1]:,}"
          f"（均值 {len(comp_sizes) / max(len(res), 1):.1f}）")
    sizes = sorted(comp_sizes)
    print(f"  每分量三角数：min={sizes[0]} p50={sizes[len(sizes) // 2]} "
          f"p90={sizes[int(len(sizes) * .9)]} p99={sizes[int(len(sizes) * .99)]} max={sizes[-1]:,}")
    for thr in (1, 16, 128):
        n = sum(1 for s in sizes if s <= thr)
        print(f"    ≤{thr:>3} 三角的分量：{n:,} / {len(sizes):,} = {n / len(sizes) * 100:.1f}%")

    # ---- 数 1：prim 级保守的代价（按"要保住多大的部件"扫门槛）----
    def tris_at(r: dict, lv: int) -> int:
        """该 prim 选到级 lv 时实际会画的三角数。

        🔴 回落必须照引擎的 `MeshletLodFallback::deepest` = **≤ lv 的最深存在级**，不是
        `min(lv, 最深级)`：后者在"级集合不连续"的 prim 上会去查一个**不存在**的级而得 0
        （本探针首跑即此坑——bistro 有 prim 的级集合是 {0,1,3}，查级 2 得 0 = 把"画了"读成"没画"）。
        """
        while lv > 0 and lv not in r["level_tris"]:
            lv -= 1
        return r["level_tris"].get(lv, 0)

    def safe_level(r: dict, threshold: int) -> int:
        """门槛 threshold（**三角数**）下该 prim 能选的**最深一级**；无受保护分量 = 不受限。"""
        cand = [max(lv - 1, 0) for n, lv, _d in r["comps_detail"] if n >= threshold and lv < INF]
        return min(cand) if cand else INF

    print(f"\n== 数 1：prim 级保守的代价（uniform 整级语义下）==")
    print(f"  有簇 prim 的 L0 三角合计 = {l0_total:,}")
    print(f"  🔴 **门槛 T 不是可调旋钮，是本表的主变量**：只有三角数 ≥ T 的分量才算『要保住的")
    print(f"     独立遮挡体』。T=1（不设门槛）是**假上界**——摄影测量重建网格里有大量 1~4 三角的")
    print(f"     裂缝碎片，它们在粗级消失不影响遮挡，却会把整 prim 拖到 L0。真实门槛该由纹素尺度")
    print(f"     定（分量投影 ≥ N 枚本级纹素，闭式），本表给的是『代价 ↔ 要保住多大的部件』曲线。")
    print(f"\n  {'T':>5} {'条目数':>10} {'占全部分量':>10} │" +
          "".join(f"{'D=' + str(d) + ' 代价':>12}" for d in CANDIDATE_D if d) +
          f"{'D=5 被拖 prim':>14}")
    cost_rows = []
    for T in THRESHOLDS:
        entries = sum(1 for r in res for n, *_ in r["comps_detail"] if n >= T)
        row = (f"  {T:>5} {entries:>10,} {entries / max(len(comp_sizes), 1) * 100:>9.1f}% │")
        for D in CANDIDATE_D:
            if D == 0:
                continue
            now = sum(tris_at(r, D) for r in res)
            cons = sum(tris_at(r, min(D, safe_level(r, T))) for r in res)
            cost_rows.append({"threshold": T, "D": D, "tris": now, "conservative": cons,
                              "ratio": cons / max(now, 1), "entries": entries})
            row += f"{cons / max(now, 1):>11.2f}×"
        dragged = sum(1 for r in res
                      if tris_at(r, min(5, safe_level(r, T))) > tris_at(r, 5))
        row += f"{dragged:>14}"
        print(row)
    # ---- 数 1b：门槛换成空间尺寸（P1 的判据该长在这上面）----
    dmins = [m / args.glb_scale for m in SIZE_BINS_M]
    def safe_level_size(r: dict, dmin: float) -> int:
        """门槛 dmin（**对角线**）下该 prim 能选的最深一级。

        🔴 **`lv - 1` 不是笔误，是首跑漏掉的一个 off-by-one**：`alive_level[c] = L` 的含义是
        "级 L 的表示里 c 的三角已经归零"，所以保住 c 要求选级 **≤ L-1**。首版直接取 L ⟹ 保守
        选到 c 已经死了的那一级，代价被**系统性低估**（bistro D=5 首跑报 1.46×）。
        """
        cand = [max(lv - 1, 0) for _n, lv, d in r["comps_detail"] if d >= dmin and lv < INF]
        return min(cand) if cand else INF

    print(f"\n== 数 1b：门槛换成**空间尺寸**（对角线，源 glb 单位；世界米按 --glb-scale "
          f"{args.glb_scale} 换算；分档按世界米定义，见 SIZE_BINS_M 注释）==")
    print(f"  {'对角线≥':>9} {'世界米':>8} {'消失分量':>9} {'涉及prim':>8} {'条目数':>9} │"
          f"{'D=5 代价':>10}{'D=7 代价':>10}{'D=11 代价':>10}")
    size_rows = []
    for dmin in dmins:
        dying = sum(1 for r in res for _n, lv, d in r["comps_detail"] if d >= dmin and lv <= 5)
        prims_hit = sum(1 for r in res
                        if any(d >= dmin and lv <= 5 for _n, lv, d in r["comps_detail"]))
        entries = sum(1 for r in res for _n, _lv, d in r["comps_detail"] if d >= dmin)
        row = (f"  {dmin:>9.2f} {dmin * args.glb_scale:>8.3f} {dying:>9,} {prims_hit:>8} "
               f"{entries:>9,} \u2502")
        rec = {"diag_min": dmin, "dying_components": dying, "prims_affected": prims_hit,
               "entries": entries}
        for D in (5, 7, 11):
            now = sum(tris_at(r, D) for r in res)
            cons = sum(tris_at(r, min(D, safe_level_size(r, dmin))) for r in res)
            row += f"{cons / max(now, 1):>9.2f}\u00d7"
            rec[f"ratio_D{D}"] = cons / max(now, 1)
        size_rows.append(rec)
        print(row)

    # ---- 数 1c：分量级理想形态——"要不要动打包工具"的唯一判据 ----
    def comp_tris_at(r: dict, comp: int, lv: int) -> int:
        """分量 comp 在级 lv 的三角数（回落已在 `comp_at` 里预填平成 deepest 策略）。"""
        arr = r["comp_at"][comp]
        return arr[lv] if lv < len(arr) else arr[-1]

    def comp_level_total(r: dict, D: int, dmin: float) -> int:
        """分量级理想形态下该 prim 在选级上限 D 时的三角数。

        每个分量**各取自己的级**：受保护（对角线 ≥ dmin）的分量取 `min(D, 死亡级-1)`，
        不受保护的直接取 D（即"随大流"，它在 D 已死也不管——这如实反映"我们不保护它"）。
        """
        total = 0
        for c, avail in r["comp_level"].items():
            _n, death, d = r["comps_info"][c]
            want = D if (d < dmin or death >= INF) else min(D, max(death - 1, 0))
            total += comp_tris_at(r, c, want)
        return total

    print(f"\n== 数 1c：**分量级理想形态**（每个分量各取自己的级）vs prim 级 vs 现状 ==")
    print(f"  这是「同分量的簇必须在产物里连续」那条离线重排能拿到的**收益上限**——")
    print(f"  上限都不划算，就不该动 `meshlet_pack`。门槛同样按对角线（世界米）。")
    print(f"  {'门槛':>7} {'现状 D=5':>12} {'prim级 D=5':>12} {'分量级 D=5':>12}"
          f"{'分量级/现状':>11} {'分量级 D=7':>12} {'D=3 全量':>12}")
    comp_rows = []
    for dmin in dmins:
        now5 = sum(tris_at(r, 5) for r in res)
        prim5 = sum(tris_at(r, min(5, safe_level_size(r, dmin))) for r in res)
        cf5 = sum(comp_level_total(r, 5, dmin) for r in res)
        cf7 = sum(comp_level_total(r, 7, dmin) for r in res)
        d3 = sum(tris_at(r, 3) for r in res)
        comp_rows.append({"diag_min": dmin, "now_D5": now5, "prim_D5": prim5,
                          "comp_D5": cf5, "comp_D7": cf7, "D3": d3,
                          "comp_vs_now": cf5 / max(now5, 1)})
        print(f"  {dmin * args.glb_scale:>6.2f}m {now5:>12,} {prim5:>12,} {cf5:>12,}"
              f"{cf5 / max(now5, 1):>10.2f}× {cf7:>12,} {d3:>12,}")

    # ---- 数 5：簇纯度与连续性——"加对照表"够不够，还是必须重排簇 ----
    print(f"\n== 数 5：**簇纯度**（一簇跨几个分量）与**连续性**（同分量的 L0 簇形成几段）==")
    print(f"  这是「加一张对照表」与「重排簇」哪个够用的唯一判据：纯度 100% 的级上对照表就够；")
    print(f"  纯度低的级上混装簇**物理拆不开**（同一张 8bit 索引表 + 同一份 remap），加什么索引都没用。")
    print(f"  {'级':>3} {'簇数':>9} {'纯净=1分量':>11} {'跨2':>9} {'跨3+':>9} {'平均跨分量':>10}")
    pur_tot = defaultdict(Counter)
    for r in res:
        for lv, cnt in r["purity"].items():
            pur_tot[lv].update(cnt)
    purity_rows = []
    for lv in sorted(pur_tot):
        cnt = pur_tot[lv]
        tot = sum(cnt.values())
        row = {"level": lv, "clusters": tot,
               "pure_share": cnt.get(1, 0) / max(tot, 1),
               "cross2": cnt.get(2, 0),
               "cross3plus": sum(v for k, v in cnt.items() if k >= 3),
               "avg_comps": sum(k * v for k, v in cnt.items()) / max(tot, 1)}
        purity_rows.append(row)
        print(f"  {lv:>3} {tot:>9,} {row['pure_share'] * 100:>10.1f}% {row['cross2']:>9,}"
              f" {row['cross3plus']:>9,} {row['avg_comps']:>10.2f}")

    dmin1 = 1.0 / args.glb_scale
    prot = [v for r in res for v in r["runs"].values() if v[2] >= dmin1]
    print(f"\n  受保护分量（对角线 ≥1m）共 {len(prot):,} 个：")
    dh = Counter((min(d - 1, 0) if d < INF else -1) for _rn, d, _dg in prot)
    print(f"    需要的选级（死亡级−1；-1 = 恒存活、不受限）：" +
          "  ".join(f"{k}:{v}" for k, v in sorted(dh.items())))
    rh = Counter(rn for rn, _d, _dg in prot)
    print(f"    它的 L0 簇形成几段（1 = 一条 draw 就够）：" +
          "  ".join(f"{k}段:{v}" for k, v in sorted(rh.items()) if k <= 5) +
          f"  ｜ >5 段 {sum(v for k, v in rh.items() if k > 5)}")
    print(f"    段数中位 = {sorted(rh.elements())[len(prot) // 2] if prot else 0}"
          f"，最大 = {max(rh) if rh else 0}")

    # ---- 数 6：排序粒度 = group（不是簇、不是分量），它够不够细？----
    print(f"\n== 数 6：**排序粒度**——`DiskGroup.clusterOffset/clusterCount` 要求一个 group 的簇"
          f"连续 ⟹ 实际能重排的是 group 之间的顺序 ==")
    g_hist = Counter()
    g_pure = Counter()
    g_per_prim = Counter()
    for r in res:
        g_per_prim[r["l0_groups"]] += 1
        for k, v in r["l0_group_purity"].items():
            g_pure[k] += v
        for k, v in r["l0_group_clusters"].items():
            g_hist[k] += v
    tg = sum(g_pure.values())
    print(f"  L0 group 总数 = {tg:,}；每 prim 的 L0 group 数：" +
          "  ".join(f"{k}:{v}" for k, v in sorted(g_per_prim.items()) if k <= 4) +
          f"  ｜ >4 的 prim {sum(v for k, v in g_per_prim.items() if k > 4)}")
    print(f"  每 group 的簇数：p50={sorted(g_hist.elements())[sum(g_hist.values()) // 2]} "
          f"p90={sorted(g_hist.elements())[int(sum(g_hist.values()) * .9)]} "
          f"max={max(g_hist)}（共 {sum(g_hist.values()):,} group）")
    print(f"  **一个 L0 group 跨几个分量**：" +
          "  ".join(f"{k}分量:{g_pure[k]:,}({g_pure[k] / tg * 100:.1f}%)"
                    for k in sorted(g_pure) if k <= 4) +
          f"  ｜ ≥5 分量 {sum(v for k, v in g_pure.items() if k >= 5):,}"
          f"（{sum(v for k, v in g_pure.items() if k >= 5) / tg * 100:.1f}%）")
    print(f"  按 group 粒度做 must_L0（含必须 L0 分量的 group **整组**钉 L0）的代价：")
    print(f"    {'门槛':>7} {'要钉的 group':>12} {'draws 增量':>11} {'额外 L0 三角':>13}"
          f"{'相对现状 D=5':>13}")
    for m in SIZE_BINS_M:
        dm = m / args.glb_scale
        must = [(r, gi) for r in res for gi in r["l0_group_info"]
                if any(d >= dm and lv == 1 for c in gi[2]
                       for _n, lv, d in [r["comps_info"][c]] for _ in (0,))]
        n_g = len(must)
        extra = sum(gi[1] for _r, gi in must)
        now5 = sum(tris_at(r, 5) for r in res)
        print(f"    {m:>6.2f}m {n_g:>12,} {n_g * prod.level_count:>11,} {extra:>13,}"
              f"{extra / max(now5, 1):>12.2f}×")

    # ---- 门槛的"看内容"对账：拒绝让门槛只由代价曲线决定 ----
    if args.list_affected > 0:
        T = args.list_affected
        rows = []
        for r in res:
            hit = [(n, lv, d) for n, lv, d in r["comps_detail"] if n >= T and lv <= 5]
            if hit:
                rows.append((r["prim"], r["material"], max(h[2] for h in hit),
                             sum(h[0] for h in hit), min(h[1] for h in hit), len(hit)))
        rows.sort(key=lambda x: -x[2])
        print(f"\n== 门槛 T={T} 下受影响的 prim（出厂 D=5 会丢独立遮挡的），按消失分量尺寸排序 ==")
        print(f"  共 {len(rows)} 个 prim。🔴 **这张表存在的理由：门槛不能只由代价曲线选**——"
              f"那等于挑对结果最有利的数；必须看过内容，确认列出来的是花箱/牛腿/窗棂这类")
        print(f"     真该保的部件，而不是重建裂缝碎片。对角线是**源 glb 单位**（`format.h` 坐标系"
              f"一节：未乘 scene.lua 的 scale=0.01），故最后一列换算成世界米。")
        print(f"  {'prim':>5} {'material':>9} {'对角线(源单位)':>13} {'世界米':>8}"
              f" {'消失三角合计':>11} {'最早死亡级':>9} {'分量数':>6}")
        for row in rows[:40]:
            print(f"  {row[0]:>5} {row[1]:>9} {row[2]:>13.2f} "
                  f"{row[2] * args.glb_scale:>8.3f}"
                  f" {row[3]:>11,} {row[4]:>9} {row[5]:>6}")
        if len(rows) > 40:
            print(f"  …（另 {len(rows) - 40} 行）")

    safe_hist = Counter(r["max_safe"] for r in res)
    print(f"\n  不设门槛时的 maxSafeLevel 分布：" +
          "  ".join(f"{k}:{v}" for k, v in sorted(safe_hist.items())
                    if k < INF) +
          f"  ｜ 恒存活 {safe_hist.get(INF, 0)}")

    # ---- 数 3：表体积（随门槛走，与数 1 同一套条目集）----
    print(f"\n== 数 3：proxy 表体积 ==")
    entries_by_T = {}
    for T in THRESHOLDS:
        e = sum(1 for r in res for n, *_ in r["comps_detail"] if n >= T)
        entries_by_T[T] = e
        print(f"  门槛 T={T:>5}：条目 {e:>9,} → 按 (分量, 8 档) 组织 "
              f"{e * 8 * ENTRY_BYTES / 1e6:>7.2f} MB（每条目 {ENTRY_BYTES}B）")
    print(f"  对照「prim 级保守」的极简形态（每 prim 一枚 maxSafeLevel，1B）：{len(res):,} B")

    # ---- 数 4：耗时 ----
    unions = sum(r["unions"] for r in res)
    print(f"\n== 数 4：建表耗时（Python 版，只用于给 C++ 定标）==")
    print(f"  union 次数 = {unions:,}   分析耗时 = {t_an:.1f}s"
          f"（全程 {time.perf_counter() - t0:.1f}s 含解析与门槛扫描）")
    # 🔴 **两条独立口径互为对账**，不是多写一行好看：本探针首跑把 ① 的换算方向写反（写成 ×10
    # 慢而不是 ÷50 快），两条当场差 500 倍、立刻可疑——单口径就会把 55s 当结论用。同台账 §1.2
    # 教训 1：判据工具最坏的病不是崩，是静默给错数。
    print(f"  ⟹ C++ 粗估（两条口径应互相印证）：")
    print(f"       ① 按 50× 语言差：{t_an:.1f}s ÷ 50 ≈ {t_an / 50:.2f}s")
    print(f"       ② 按 union 吞吐 50M/s：{unions:,} ÷ 50e6 ≈ {unions / 50e6:.2f}s")
    print(f"  🔴 两条**都不是实测**，P1 打进启动日志的那条读数才算数（先例教训 = 债-102）")

    prod.close()
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"product": args.product, "prim_count": prod.prim_count,
                       "level_count": prod.level_count,
                       "anchors": anchors, "prims_analyzed": len(res),
                       "component_count": len(comp_sizes),
                       "comps_per_prim_hist": {str(k): v for k, v in sorted(per_prim_n.items())},
                       "comp_tris_p50": sizes[len(sizes) // 2],
                       "comp_tris_p90": sizes[int(len(sizes) * .9)],
                       "comp_tris_p99": sizes[int(len(sizes) * .99)],
                       "comp_tris_max": sizes[-1],
                       "components_le_128": sum(1 for s in sizes if s <= 128),
                       "max_safe_hist_unthresholded":
                           {str(k): v for k, v in sorted(safe_hist.items())},
                       "entries_by_threshold": {str(k): v for k, v in entries_by_T.items()},
                       "size_curve": size_rows,
                       "component_level_curve": comp_rows,
                       "cluster_purity_by_level": purity_rows,
                       "cost_rows": cost_rows,
                       "unions": unions, "seconds_analyze": t_an}, f, indent=2)
        print(f"\n读数已写 {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
