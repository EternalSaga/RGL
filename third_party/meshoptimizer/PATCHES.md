# third_party/meshoptimizer —— vendored `clusterlod.h`

## 这里有什么

| 文件 | 来源 |
|---|---|
| `clusterlod.h` | zeux/meshoptimizer **v1.3** tag 的 `demo/clusterlod.h`，**逐字节未改**，SHA256 = `71f95c1f97ef567235540c038b41906091fcef1ef68046f2933cd5989f94ea93` |
| `LICENSE.md` | 同 tag 的 `LICENSE.md`（MIT，Copyright (c) 2016-2026 Arseny Kapoulkine） |

**库本体不在这里**：`meshoptimizer` 走 vcpkg（`find_package(meshoptimizer CONFIG)` ⟹
`meshoptimizer::meshoptimizer`），port 见 `third_party/vcpkg-ports/meshoptimizer/`。

## 为什么这个头要 vendored（而库走 vcpkg）

vcpkg port 只安装 `src/`（库 + `meshoptimizer.h`），**不安装 `demo/`**——而
`clusterlod.h`（BTO 式簇 LOD DAG 的唯一现成实现：簇化 + 逐级简化 + 误差累积 + group DAG +
`clodBuildHierarchy` 空间层级）就住在 `demo/` 里。它是**纯头文件、无 .cpp**，所以 vendored
一个文件即可，不需要 vendor 整个库。

**为什么 port 要 overlay 钉 1.3**（`third_party/vcpkg-ports/`，`just config` 传
`-DVCPKG_OVERLAY_PORTS`）：vcpkg baseline（本机 checkout 2026.07.29）里只有 **1.2**，而
`clusterlod.h` 用到的 simplify flags `meshopt_SimplifyErrorClamped` / `meshopt_SimplifyPreserveFolds`
是 1.3 才有的（1.2 的枚举止于 `meshopt_SimplifyRegularizeLight`）。**只钉这一个 port**，
不 pull 整个 vcpkg 仓库 = 不触发其它 port 升版与三机全量重编。

## 🔴 版本必须与库一致

`clusterlod.h` 直接引用 simplify flags 枚举与 `meshopt_simplifyWithAttributes` 签名 ⟹
**升这个头必须同时升 overlay port 的版本**（反之亦然）。1.2 库 + 1.3 头 = 编译期就炸（这是好事，
不会静默错）。

## 升级流程（改这个文件的人必须全做完）

1. 从新 tag 取 `demo/clusterlod.h` 覆盖本文件，更新上面的 SHA256。
2. 同步 `third_party/vcpkg-ports/meshoptimizer/{vcpkg.json,portfile.cmake}` 的 `version` 与
   `SHA512`（portfile 用 `REF "v${VERSION}"`）。
3. `just config && just build-tools`，然后对 bistro 跑 `meshlet_pack --verify` 与统计表。
4. 与 `docs/mdi_plan.md` §7.2 的产物表**逐项对拍**；数字变了就更新那张表并注明 meshoptimizer 版本。

## 为什么数字对拍是护栏而不是吹毛求疵

§7 的全部可行性结论（27,487 个 L0 簇 / 61,547 簇 / 7,598 group / 8,705 层级节点 / 12 级 /
L0 三角 = 输入的 100%）都是**在这个 SHA256 上实测**的。簇化与简化是浮点数值路径
（`simplifier.cpp` 无 SIMD，但 clusterizer 有 SIMD 路径 + 构建 triplet 带 `-march=native`
与 FMA 收缩）⟹ **同机可复现，跨机不保证逐位**。所以：产物逐位比对只在同机做（项目回归判据本来就
是同机自比），跨机只比统计量（簇数 / 各级三角数 / 数据面体积）。
