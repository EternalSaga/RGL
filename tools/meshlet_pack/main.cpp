// meshlet_pack —— 离线簇化打包器（簇化轴 ★0-a）。
//
//   用法: meshlet_pack <in.glb> [--out <bin>] [--verify] [--stats] [--must-l0-diag <源单位>]
//
// glb → meshlet 簇 LOD DAG 产物（顶点池 + 8bit 簇索引 + 逐簇包围体 + group DAG +
// 空间层级），布局契约 = format.h；统计表 = --stats（与 docs/mdi_plan.md §7.2 逐项
// 对拍）；自检 = --verify（三条 + 结构辅助，变异红线全文见 verify.cpp 头注释）。
//
// 依赖红线（§7.8 拍板 3）：只链 meshoptimizer + tinygltf，**不链任何 rgl 引擎库**
// ——引擎侧只读产物、不链 meshoptimizer，两侧不共享代码只共享格式契约。
//
// TU 切缝（tools/ 侧样式规则：单文件 < 500 代码行；缝按改动面切，imgqa 先例）：
//   · glb_source.*  = 输入侧（tinygltf 读 glb，no-op image loader；口径 = §7 dump.py）
//   · packer.*      = 转换（clodBuild 逐 prim + clodBuildHierarchy 全局一次；钉死配置）
//   · components.*  = 判据（位置焊接 + 连通分量 + 逐级覆盖 → must_L0）与 L0 段 group 重排
//   · verify.*      = 自检与变异红线（判据本体）
//   · serialize.*   = 输出侧（format.h 落盘）
//   · stats.*       = 读数表（对拍面）
//   · sha1.*        = 缓存键（stdlib-only 手写 SHA1）
//   · main.cpp      = CLI 与编排（读 → 转 → stats → verify → 写）
//
// 认知复杂度护栏（>15 警告）逼出的函数级拆缝（零基线起步，不登 tool_baseline）：
// main = parseArgs（参数形态）+ run（编排），glb_source 的 expandPrim = 顶点段/索引段
// （两条失败面），verify 的 ②④ = 一条不变量一个 helper——缝说明就地写在各拆点注释。
//
// 产物默认不落 build 目录（★0-b 才接构建期）；本批验收产物写 /tmp/opencode/mdi/out/。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "glb_source.h"
#include "packer.h"
#include "serialize.h"
#include "sha1.h"
#include "stats.h"
#include "verify.h"

using namespace RGL::meshpack;

namespace {

void usage()
{
    std::fprintf(stderr,
        "usage: meshlet_pack <in.glb> [--out <bin>] [--verify] [--stats] [--must-l0-diag <源单位>]\n"
        "  --must-l0-diag  must_L0 门槛 = 分量包围对角线下限（**源 glb 单位**，非米；\n"
        "                  bistro 的 100 = 世界 1m，因 scene.lua scale=0.01）。0 = 关闭：\n"
        "                  不排序、MustL0 段为空、簇序与 v1 逐位相同。默认 0。\n");
}

struct Options {
    std::string input, output;
    bool doVerify = false, doStats = false;
    /// must_L0 门槛（源 glb 单位的分量包围对角线）；0 = 关闭判定。口径见 components.h。
    double mustL0Diag = 0.0;
};

/// `--must-l0-diag` 的值：必须是个非负有限数（NaN 走 `!(v >= 0.0)` 这侧被拒；负数语义上就是
/// "关掉"，与其悄悄当 0 用不如拒绝——参数写错的人想要的是出声，不是默认值）。
bool takeDiag(const char* text, double& out)
{
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || *end != '\0' || !(v >= 0.0)) return false;
    out = v;
    return true;
}

/// 长选项消费。返回 true = 已识别且合法（`i` 按引用推进：带值的选项吃掉下一枚参数）；
/// 返回 false = 不识别或值非法（调用处一次红，run() 里不再出现 argv）。
bool parseOption(int argc, char** argv, int& i, Options& o)
{
    const std::string arg = argv[i];
    if (arg == "--verify") { o.doVerify = true; return true; }
    if (arg == "--stats") { o.doStats = true; return true; }
    if (arg == "--out") {
        if (++i >= argc) return false;
        o.output = argv[i];
        return true;
    }
    if (arg == "--must-l0-diag") {
        if (++i >= argc) return false;
        return takeDiag(argv[i], o.mustL0Diag);
    }
    return false;
}

// CLI 解析（缝 = 参数形态 vs 编排：判错在返回 false 处一次红，run() 里不再出现 argv）。
bool parseArgs(int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (!arg.empty() && arg[0] == '-') {
            if (!parseOption(argc, argv, i, o)) return false;
        } else if (o.input.empty()) {
            o.input = arg;
        } else {
            return false;
        }
    }
    return !o.input.empty();
}

// 流式 SHA1（481MB 不整读进内存；同时得到文件长度做早退检查）。
bool fileSha1(const std::string& path, uint8_t out[20])
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    Sha1 s;
    s.init();
    std::vector<uint8_t> buf(1 << 20);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) s.update(buf.data(), n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    if (!ok) return false;
    s.finish(out);
    return true;
}

int run(const Options& opt)
{
    const auto t0 = std::chrono::steady_clock::now();
    const auto secs = [&t0] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

    uint8_t sha1[20];
    if (!fileSha1(opt.input, sha1)) {
        std::fprintf(stderr, "meshlet_pack: 读不到输入文件: %s\n", opt.input.c_str());
        return 1;
    }
    std::fprintf(stderr, "sha1 %s (%.1fs)\n", sha1ToHex(sha1).c_str(), secs());

    std::vector<SourcePrim> prims = loadGlbSource(opt.input);
    std::fprintf(stderr, "glb loaded: %zu prims (%.1fs)\n", prims.size(), secs());

    PackedAsset asset = packAsset(prims, sha1, opt.mustL0Diag);
    std::fprintf(stderr, "packed: %llu clusters / %llu groups / %llu nodes (%.1fs)\n",
        (unsigned long long)asset.stats.totalClusters, (unsigned long long)asset.stats.groups,
        (unsigned long long)asset.stats.nodes, secs());
    if (opt.mustL0Diag > 0.0)
        std::fprintf(stderr, "must_L0(diag>=%.6g 源单位): %llu 分量 / 钉 %llu group（额外 L0 三角 %llu）/ %llu 段 (%.1fs)\n",
            opt.mustL0Diag, (unsigned long long)asset.mustL0.mustL0Components,
            (unsigned long long)asset.mustL0.pinnedGroups, (unsigned long long)asset.mustL0.pinnedTris,
            (unsigned long long)asset.mustL0Ranges.size(), secs());

    if (opt.doStats) printStats(asset, prims, opt.input.c_str(), stdout);

    // 先写再验：--verify 红不该让 --out 落空（产物是排查 verify 红因的第一现场）
    if (!opt.output.empty()) {
        writeAsset(opt.output, asset);
        std::fprintf(stderr, "wrote %s (%.1fs)\n", opt.output.c_str(), secs());
    }

    if (opt.doVerify) return verifyAsset(asset, prims, stdout) ? 0 : 1;
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt)) { usage(); return 2; }
    try {
        return run(opt);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "meshlet_pack: %s\n", e.what());
        return 1;
    }
}
